/* mysh-fixed.c -- P3 草稿1 修复版 */
#define BUFSIZE 4096
#define MAXARGS (BUFSIZE / 2 + 1)
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>

void print_prompt(void) {
    char cwd[BUFSIZE];
    char *home = getenv("HOME");

    if (getcwd(cwd,  sizeof(cwd)) == NULL) {
        perror("getcwd");
        return;
    }

    if (home != NULL && strncmp(cwd, home, strlen(home)) == 0 &&
        (cwd[strlen(home)] == '/' || cwd[strlen(home)] == '\0')) {
            printf("~%s$ ", cwd + strlen(home));
    }
    else {
        printf("%s$ ", cwd);
    }

    fflush(stdout);
}

int find_executable(char *program, char *path, size_t size) {
    char *directories[] = {
        "/usr/local/bin",
        "/usr/bin",
        "/bin"
    };

    int i;

    if (strchr(program, '/') != NULL) {
        if (access(program, X_OK) == 0) {
            if (strlen(program) >= size) {
                return 0;
            }

            strcpy(path, program);
            return 1;
        }

        return 0;
    }

    for (i = 0; i < 3; i++) {
        int result = snprintf(path, size, "%s/%s", directories[i], program);

        if (result < 0 || (size_t) result >= size) {
            continue;
        }

        if (access(path, X_OK) == 0) {
            return 1;
        }
    }

    return  0;
}

int builtin_cd(char *args[], int count) {
    char *directory;

    if (count > 2) {
        fprintf(stderr, "cd: too many arguments\n");
        return 0;
    }

    if (count == 1) {
        directory = getenv("HOME");

        if (directory == NULL) {
            fprintf(stderr, "cd: HOME not set\n");
            return 0;
        }
    }
    else {
        directory = args[1];
    }

    if (chdir(directory) == -1) {
        perror("cd");
        return 0;
    }

    return 1;
}

int builtin_pwd(void) {
    char cwd[BUFSIZE];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("pwd");
        return 0;
    }

    printf("%s\n", cwd);

    return 1;
}

int builtin_which(char *args[], int count) {
    char path[BUFSIZE];
    int i;
    int success = 1;

    if (count < 2) {
        fprintf(stderr, "which: missing argument\n");
        return 0;
    }

    for (i = 1; i < count; i++) {
        if (find_executable(args[i], path, sizeof(path))) {
            printf("%s\n", path);
        }
        else {
            success = 0;
        }
    }

    return success;
}

int redirect_io(char *input_file, char *output_file, int append) {
    int fd;

    if (input_file != NULL) {
        fd = open(input_file, O_RDONLY);

        if (fd == -1) {
            perror(input_file);
            return 0;
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            close(fd);
            return 0;
        }

        close(fd);
    }

    if (output_file != NULL) {
        if (append) {
            fd = open(output_file, O_WRONLY | O_CREAT | O_APPEND, 0640);
        }
        else {
            fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0640);
        }

        if (fd == -1) {
            perror(output_file);
            return 0;
        }

        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2");
            close(fd);
            return 0;
        }

        close(fd);
    }

    return 1;
}

int execute_builtin(char *args[], int count, char *input_file, char *output_file, int append) {
    int saved_input = -1;
    int saved_output = -1;
    int result = 0;

    if (input_file != NULL || output_file != NULL) {
        fflush(stdout);

        saved_input = dup(STDIN_FILENO);
        saved_output = dup(STDOUT_FILENO);

        if (saved_input == -1 || saved_output == -1) {
            perror("dup");
            if (saved_input != -1) close(saved_input);
            if (saved_output != -1) close(saved_output);
            return 0;
        }

        if (!redirect_io(input_file, output_file, append)) {
            dup2(saved_input, STDIN_FILENO);
            dup2(saved_output, STDOUT_FILENO);
            close(saved_input);
            close(saved_output);
            return 0;
        }
    }

    if (strcmp(args[0], "cd") == 0) {
        result = builtin_cd(args, count);
    }
    else if (strcmp(args[0], "pwd") == 0) {
        result = builtin_pwd();
    }
    else if (strcmp(args[0], "which") == 0) {
        result = builtin_which(args, count);
    }

    if (saved_input != -1) {
        fflush(stdout);

        dup2(saved_input, STDIN_FILENO);
        dup2(saved_output, STDOUT_FILENO);

        close(saved_input);
        close(saved_output);
    }

    return result;
}

int wildcard_match(char *pattern, char *filename) {
    char *asterisk = strchr(pattern, '*');
    size_t prefix_length;
    size_t suffix_length;
    size_t filename_length;

    if (asterisk == NULL) {
        return 0;
    }

    prefix_length = asterisk - pattern;
    suffix_length = strlen(asterisk + 1);
    filename_length = strlen(filename);

    if (filename_length < prefix_length + suffix_length) {
        return 0;
    }

    if (strncmp(pattern, filename, prefix_length) != 0) {
        return 0;
    }

    if (strcmp(asterisk + 1,
               filename + filename_length - suffix_length) != 0) {
        return 0;
    }

    if (pattern[0] == '*' && filename[0] == '.') {
        return 0;
    }

    return 1;
}

int expand_wildcard(char *pattern, char *matches[], int max_matches) {
    char directory[BUFSIZE];
    char filename[BUFSIZE];
    char *slash;
    char *name_pattern;
    DIR *dir;
    struct dirent *entry;
    int count = 0;

    slash = strrchr(pattern, '/');

    if (slash != NULL) {
        size_t directory_length = slash - pattern;

        if (directory_length == 0) {
            strcpy(directory, "/");
        }
        else {
            strncpy(directory, pattern, directory_length);
            directory[directory_length] = '\0';
        }

        name_pattern = slash + 1;
    }
    else {
        strcpy(directory, ".");
        name_pattern = pattern;
    }

    dir = opendir(directory);

    if (dir == NULL) {
        return 0;
    }

    while ((entry = readdir(dir)) != NULL && count < max_matches) {
        if (wildcard_match(name_pattern, entry->d_name)) {
            if (slash != NULL) {
                size_t directory_length = strlen(directory);
                size_t entry_length = strlen(entry->d_name);
                size_t separator_length = strcmp(directory, "/") == 0 ? 0 : 1;

                if (directory_length + separator_length + entry_length >= sizeof(filename)) {
                    continue;
                }

                strcpy(filename, directory);

                if (separator_length != 0) {
                    strcat(filename, "/");
                }

                strcat(filename, entry->d_name);
            }
            else {
                if (strlen(entry->d_name) >= sizeof(filename)) {
                    continue;
                }

                strcpy(filename, entry->d_name);
            }

            matches[count] = malloc(strlen(filename) + 1);

            if (matches[count] == NULL) {
                closedir(dir);
                return count;
            }

            strcpy(matches[count], filename);
            count++;
        }
    }

    closedir(dir);

    return count;
}

int add_argument(char *token, char *args[], char *allocated[], int *count) {
    char *matches[MAXARGS];
    int num_matches;
    int i;

    if (strchr(token, '*') == NULL) {
        args[*count] = token;
        allocated[*count] = NULL;
        (*count)++;
        return 1;
    }

    if (strchr(strchr(token, '*') + 1, '*') != NULL) {
        fprintf(stderr, "mysh: syntax error\n");
        return 0;
    }

    num_matches = expand_wildcard(token, matches, MAXARGS - *count - 1);

    if (num_matches == 0) {
        args[*count] = token;
        allocated[*count] = NULL;
        (*count)++;
        return 1;
    }

    for (i = 0; i < num_matches; i++) {
        args[*count] = matches[i];
        allocated[*count] = matches[i];
        (*count)++;
    }

    return 1;
}

void execute_pipeline_command(char *command) {
    char *args[MAXARGS];
    char path[BUFSIZE];
    char *token;
    int count = 0;
    char *allocated[MAXARGS] = {NULL};
    int i;
    char *input_file = NULL;
    char *output_file = NULL;
    int append = 0;
    int error = 0;
    int result = 0;

    token = strtok(command, " \t");

    while (token != NULL && count < MAXARGS - 1) {

        if (strcmp(token, "<") == 0) {

            if (input_file != NULL) {
                fprintf(stderr, "mysh: syntax error\n");
                error = 1;
                break;
            }

            input_file = strtok(NULL, " \t");

            if (input_file == NULL ||
                strcmp(input_file, "<") == 0 ||
                strcmp(input_file, ">") == 0 ||
                strcmp(input_file, ">>") == 0) {
                fprintf(stderr, "mysh: syntax error\n");
                error = 1;
                break;
            }
        }
        else if (strcmp(token, ">") == 0 || strcmp(token, ">>") == 0) {

            if (output_file != NULL) {
                fprintf(stderr, "mysh: syntax error\n");
                error = 1;
                break;
            }

            append = (strcmp(token, ">>") == 0);

            output_file = strtok(NULL, " \t");

            if (output_file == NULL ||
                strcmp(output_file, "<") == 0 ||
                strcmp(output_file, ">") == 0 ||
                strcmp(output_file, ">>") == 0) {
                fprintf(stderr, "mysh: syntax error\n");
                error = 1;
                break;
            }
        }
        else {
            if (strchr(token, '<') != NULL) {
                fprintf(stderr, "mysh: syntax error\n");
                error = 1;
                break;
            }

            if (!add_argument(token, args, allocated, &count)) {
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }

                _exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, " \t");
    }

    if (error) {
        for (i = 0; i < count; i++) {
            free(allocated[i]);
        }

        _exit(EXIT_FAILURE);
    }

    args[count] = NULL;

    if (count == 0) {
        _exit(EXIT_SUCCESS);
    }

    if (!redirect_io(input_file, output_file, append)) {
        _exit(EXIT_FAILURE);
    }

    if (strcmp(args[0], "cd") == 0) {
        result = builtin_cd(args, count);
    }
    else if (strcmp(args[0], "pwd") == 0) {
        result = builtin_pwd();
    }
    else if (strcmp(args[0], "which") == 0) {
        result = builtin_which(args, count);
    }
    else if (strcmp(args[0], "exit") == 0) {
        _exit(EXIT_SUCCESS);
    }
    else {
        if (!find_executable(args[0], path, sizeof(path))) {
            fprintf(stderr, "mysh: %s: command not found\n", args[0]);
            _exit(EXIT_FAILURE);
        }

        execv(path, args);

        perror(args[0]);
        _exit(EXIT_FAILURE);
    }

    fflush(stdout);
    _exit(result ? EXIT_SUCCESS : EXIT_FAILURE);
}

int execute_pipeline(char *commands[], int num_commands, int interactive) {
    int pipes[MAXARGS][2];
    pid_t pids[MAXARGS];
    int i;
    int status;
    int last_status = 0;

    for (i = 0; i < num_commands - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            int j;

            perror("pipe");

            for (j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            return 0;
        }
    }

    for (i = 0; i < num_commands; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            int j;

            perror("fork");

            for (j = 0; j < num_commands - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            for (j = 0; j < i; j++) {
                while (waitpid(pids[j], &status, 0) == -1) {
                    if (errno != EINTR) {
                        perror("waitpid");
                        break;
                    }
                }
            }

            return 0;
        }

        if (pids[i] == 0) {

            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
            }
            else if (!interactive) {
                int null_fd = open("/dev/null", O_RDONLY);

                if (null_fd == -1) {
                    perror("/dev/null");
                    exit(EXIT_FAILURE);
                }

                if (dup2(null_fd, STDIN_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }

                close(null_fd);
            }

            if (i < num_commands - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2");
                    _exit(EXIT_FAILURE);
                }
            }


            for (int j = 0; j < num_commands - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            execute_pipeline_command(commands[i]);
            _exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < num_commands - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (i = 0; i < num_commands; i++) {
        while (waitpid(pids[i], &status, 0) == -1) {
            if (errno != EINTR) {
                perror("waitpid");
                return 0;
            }
        }

        if (i == num_commands - 1) {
            last_status = status;
        }
    }

    if (interactive) {
        if (WIFEXITED(last_status) && WEXITSTATUS(last_status) != 0) {
            printf("Exited with status %d\n", WEXITSTATUS(last_status));
        }
        else if (WIFSIGNALED(last_status)) {
            psignal(WTERMSIG(last_status), "Terminated by signal");
        }
    }

    return 0;
}

int execute_command(char *command, int interactive) {
    char *args[MAXARGS];
    char path[BUFSIZE];
    char *token;
    int count = 0;
    char *allocated[MAXARGS] = {NULL};
    int i;
    char *commands[MAXARGS];
    int num_commands = 0;
    int status;
    pid_t pid;
    char *input_file = NULL;
    char *output_file = NULL;
    int append = 0;

    char *comment = strchr(command, '#');

    if (comment != NULL) {
        *comment = '\0';
    }

    if (strspn(command, " \t\n") == strlen(command)) {
        return 0;
    }

    char *pipe_position = strchr(command, '|');

    if (pipe_position != NULL) {
        char *start = command;
        char *current = command;

        while (*current != '\0') {
            if (*current == '|') {
                *current = '\0';

                if (strspn(start, " \t") == strlen(start)) {
                    fprintf(stderr, "mysh: syntax error\n");
                    return 0;
                }

                commands[num_commands] = start;
                num_commands++;
                start = current + 1;
            }

            current++;
        }

        if (strspn(start, " \t") == strlen(start)) {
            fprintf(stderr, "mysh: syntax error\n");
            return 0;
        }

        commands[num_commands] = start;
        num_commands++;

        int has_exit = 0;

        for (i = 0; i < num_commands; i++) {
            char *segment = commands[i] + strspn(commands[i], " \t");
            size_t token_length = strcspn(segment, " \t");

            if (token_length == 4 && strncmp(segment, "exit", 4) == 0) {
                has_exit = 1;
            }
        }

        execute_pipeline(commands, num_commands, interactive);

        return has_exit;
    }

    token = strtok(command, " \t");

    while (token != NULL && count < MAXARGS - 1) {

        if (strcmp(token, "<") == 0) {

            if (input_file != NULL) {
                fprintf(stderr, "mysh: syntax error\n");
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }
                return 0;
            }

            input_file = strtok(NULL, " \t");

            if (input_file == NULL ||
                strcmp(input_file, "<") == 0 ||
                strcmp(input_file, ">") == 0 ||
                strcmp(input_file, ">>") == 0) {
                fprintf(stderr, "mysh: syntax error\n");
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }
                return 0;
            }
        }
        else if (strcmp(token, ">") == 0 || strcmp(token, ">>") == 0) {

            if (output_file != NULL) {
                fprintf(stderr, "mysh: syntax error\n");
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }
                return 0;
            }

            append = (strcmp(token, ">>") == 0);

            output_file = strtok(NULL, " \t");

            if (output_file == NULL ||
                strcmp(output_file, "<") == 0 ||
                strcmp(output_file, ">") == 0 ||
                strcmp(output_file, ">>") == 0) {
                fprintf(stderr, "mysh: syntax error\n");
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }
                return 0;
            }
        }
        else {
            if (strchr(token, '<') != NULL) {
                fprintf(stderr, "mysh: syntax error\n");
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }
                return 0;
            }

            if (!add_argument(token, args, allocated, &count)) {
                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }
                return 0;
            }
        }

        token = strtok(NULL, " \t");
    }

    args[count] = NULL;

    if (count == 0) {
        for (i = 0; i < count; i++) {
            free(allocated[i]);
        }

        return 0;
    }

    if (strcmp(args[0], "exit") == 0) {
        /* exit 带重定向时,先照常完成重定向(创建文件等),再退出 */
        if (input_file != NULL || output_file != NULL) {
            pid = fork();

            if (pid < 0) {
                perror("fork");

                for (i = 0; i < count; i++) {
                    free(allocated[i]);
                }

                return 0;
            }

            if (pid == 0) {
                if (!redirect_io(input_file, output_file, append)) {
                    _exit(EXIT_FAILURE);
                }

                _exit(EXIT_SUCCESS);
            }

            while (waitpid(pid, &status, 0) == -1) {
                if (errno != EINTR) {
                    perror("waitpid");
                    break;
                }
            }
        }

        for (i = 0; i < count; i++) {
            free(allocated[i]);
        }

        return 1;
    }

    if (strcmp(args[0], "cd") == 0 || strcmp(args[0], "pwd") == 0 || strcmp(args[0], "which") == 0) {
        execute_builtin(args, count, input_file, output_file, append);

        for (i = 0; i < count; i++) {
            free(allocated[i]);
        }

        return 0;
    }

    if (!find_executable(args[0], path, sizeof(path))) {
        fprintf(stderr, "mysh: %s: command not found\n", args[0]);

        for (i = 0; i < count; i++) {
            free(allocated[i]);
        }

        return 0;
    }

    pid = fork();

    if (pid < 0) {
        perror("fork");

        for (i = 0; i < count; i++) {
            free(allocated[i]);
        }

        return 0;
    }

    if (pid == 0) {
        int null_fd;

        if (!interactive) {
            null_fd = open("/dev/null", O_RDONLY);

            if (null_fd == -1) {
                perror("/dev/null");
                _exit(EXIT_FAILURE);
            }

            if (dup2(null_fd, STDIN_FILENO) == -1) {
                perror("dup2");
                _exit(EXIT_FAILURE);
            }

            close(null_fd);
        }

        if (!redirect_io(input_file, output_file, append)) {
            _exit(EXIT_FAILURE);
        }

        execv(path,args);

        perror(args[0]);
        _exit(EXIT_FAILURE);
    }

    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            perror("waitpid");

            for (i = 0; i < count; i++) {
                free(allocated[i]);
            }

            return 0;
        }
    }

    if (interactive) {
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            printf("Exited with status %d\n", WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status)) {
            psignal(WTERMSIG(status), "Terminated by signal");
        }
    }

    for (i = 0; i < count; i++) {
        free(allocated[i]);
    }

    return 0;
}

int main(int argc, char *argv[]) {
    int interactive = isatty(STDIN_FILENO);
    int input = STDIN_FILENO;
    char command[BUFSIZE];
    int new_command = 1;
    int length = 0;
    char ch;
    ssize_t bytes_read;

    if (argc > 2) {
        fprintf(stderr, "Usage: mysh [filename]\n");
        return EXIT_SUCCESS;
    }

    if (argc == 2) {
        input = open(argv[1], O_RDONLY);

        if (input == -1) {
            perror(argv[1]);
            return EXIT_FAILURE;
        }

        interactive = 0;
    }

    if (interactive) {
        printf("Welcome to my shell!\n");
    }

    while (1) {
        if (interactive && new_command) {
            print_prompt();
            new_command = 0;
        }

        bytes_read = read(input, &ch, 1);

        if (bytes_read == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("read");
            break;
        }

        if (bytes_read == 0 || ch == '\n') {
            command[length] = '\0';

            if (length > 0) {
                if (execute_command(command, interactive)) {
                    break;
                }
            }

            length = 0;
            new_command = 1;

            if (bytes_read == 0) {
                break;
            }

            continue;
        }

        if (length < BUFSIZE - 1) {
            command[length] = ch;
            length++;
        }
        else {
            fprintf(stderr, "mysh: command too long\n");

            while (ch != '\n') {
                bytes_read = read(input, &ch, 1);

                if (bytes_read <= 0) {
                    break;
                }
            }

            length = 0;
            new_command = 1;

            if (bytes_read <= 0) {
                break;
            }
        }
    }

    if (interactive) {
        printf("Exiting my shell.\n");
    }

    if (input != STDIN_FILENO) {
        close(input);
    }

    return EXIT_SUCCESS;
}
