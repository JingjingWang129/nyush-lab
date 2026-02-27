#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>

static int suspended_jobs = 0;

static void shell_ignore_signals(void)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
}

static void child_restore_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
}

static void print_prompt(void)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
    {
        strcpy(cwd, "/");
    }

    const char *base = strrchr(cwd, '/');
    if (!base)
        base = cwd;
    else if (strcmp(cwd, "/") == 0)
        base = "/";
    else
        base = base + 1;

    printf("[nyush %s]$ ", (strcmp(cwd, "/") == 0) ? "/" : base);
    fflush(stdout);
}

static char *resolve_program(const char *cmd)
{
    if (!cmd || cmd[0] == '\0')
        return NULL;

    if (cmd[0] == '/')
    {
        if (access(cmd, X_OK) == 0)
            return strdup(cmd);
        return NULL;
    }

    if (strchr(cmd, '/'))
    {
        if (access(cmd, X_OK) == 0)
            return strdup(cmd);
        return NULL;
    }

    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "/usr/bin/%s", cmd);
    if (access(buf, X_OK) == 0)
        return strdup(buf);
    return NULL;
}

static char **split_argv(char *line, int *argc_out)
{
    int cap = 16, argc = 0;
    char **argv = malloc(sizeof(char *) * cap);
    if (!argv)
        return NULL;

    char *save = NULL;
    char *tok = strtok_r(line, " ", &save);
    while (tok)
    {
        if (argc + 1 >= cap)
        {
            cap *= 2;
            char **tmp = realloc(argv, sizeof(char *) * cap);
            if (!tmp)
            {
                free(argv);
                return NULL;
            }
            argv = tmp;
        }
        argv[argc++] = tok;
        tok = strtok_r(NULL, " ", &save);
    }
    argv[argc] = NULL;
    if (argc_out)
        *argc_out = argc;
    return argv;
}

int main(void)
{
    shell_ignore_signals();

    char *line = NULL;
    size_t cap = 0;

    while (1)
    {
        print_prompt();

        int redirect_in = 0;
        int redirect_out = 0;
        int append = 0;
        char *infile = NULL;
        char *outfile = NULL;

        ssize_t nread = getline(&line, &cap, stdin);
        if (nread < 0)
            break;
        if (nread > 0 && line[nread - 1] == '\n')
        {
            line[nread - 1] = '\0';
        }
        if (line[0] == '\0')
            continue;

        int argc = 0;
        char *line_copy = strdup(line);
        if (!line_copy)
            continue;

        char **argv = split_argv(line_copy, &argc);
        if (!argv || argc == 0)
        {
            free(argv);
            free(line_copy);
            continue;
        }

        if (strcmp(argv[0], "cd") == 0)
        {
            if (argc != 2)
            {
                fprintf(stderr, "Error: invalid command\n");
            }
            else
            {
                if (chdir(argv[1]) != 0)
                {
                    fprintf(stderr, "Error: invalid directory\n");
                }
            }
            free(argv);
            free(line_copy);
            continue;
        }

        if (strcmp(argv[0], "exit") == 0)
        {
            if (argc != 1)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }
            if (suspended_jobs > 0)
            {
                fprintf(stderr, "Error: there are suspended jobs\n");
                free(argv);
                free(line_copy);
                continue;
            }
            free(argv);
            free(line_copy);
            break;
        }

        for (int i = 0; i < argc; i++)
        {
            if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0)
            {
                if (redirect_out || i + 1 >= argc)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    continue;
                }
                redirect_out = 1;
                append = (strcmp(argv[i], ">>") == 0);
                outfile = argv[i + 1];
                argv[i] = NULL;
                argc = i;
                break;
            }

            if (strcmp(argv[i], "<") == 0)
            {
                if (redirect_in || i + 1 >= argc)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    continue;
                }
                redirect_in = 1;
                infile = argv[i + 1];
                argv[i] = NULL;
                argc = i;
                break;
            }
        }

        char *prog = resolve_program(argv[0]);
        if (!prog)
        {
            fprintf(stderr, "Error: invalid program\n");
            free(argv);
            free(line_copy);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "Error: invalid program\n");
            free(prog);
            free(argv);
            free(line_copy);
            continue;
        }

        if (pid == 0)
        {
            // child
            child_restore_signals();
            if (redirect_out)
            {
                int fd;
                if (append)
                {
                    fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
                }
                else
                {
                    fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                }
                if (fd < 0)
                {
                    fprintf(stderr, "Error: invalid file\n");
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }

            if (redirect_in)
            {
                int fd = open(infile, O_RDONLY);
                if (fd < 0)
                {
                    fprintf(stderr, "Error: invalid file\n");
                    exit(1);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
            }

            execv(prog, argv);
            // exec failed
            _exit(1);
        }
        else
        {
            // parent
            int status = 0;
            (void)waitpid(pid, &status, 0);
        }

        free(prog);
        free(argv);
        free(line_copy);
    }

    free(line);
    return 0;
}