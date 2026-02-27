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

static int parse_redirections(char **argv, int *argc, int *redirect_in, char **infile, int *redirect_out, int *append, char **outfile)
{
    int w = 0;
    for (int r = 0; r < *argc; r++)
    {
        if (strcmp(argv[r], "<") == 0)
        {
            if (*redirect_in || r + 1 >= *argc)
                return -1;
            *redirect_in = 1;
            *infile = argv[r + 1];
            r++;
            continue;
        }
        if (strcmp(argv[r], ">") == 0 || strcmp(argv[r], ">>") == 0)
        {
            if (*redirect_out || r + 1 >= *argc)
                return -1;
            *redirect_out = 1;
            *append = (strcmp(argv[r], ">>") == 0);
            *outfile = argv[r + 1];
            r++;
            continue;
        }
        argv[w++] = argv[r];
    }
    argv[w] = NULL;
    *argc = w;
    if (*argc == 0)
        return -1;
    return 0;
}

static int is_builtin(const char *s)
{
    return s && (!strcmp(s, "cd") || !strcmp(s, "exit") || !strcmp(s, "jobs") || !strcmp(s, "fg"));
}

static int find_pipe(char **argv, int argc)
{
    for (int i = 0; i < argc; i++)
    {
        if (strcmp(argv[i], "|") == 0)
            return i;
    }
    return -1;
}

static void exec_with_path(char **argv)
{
    char *prog = resolve_program(argv[0]);
    if (!prog)
    {
        fprintf(stderr, "Error: invalid program\n");
        _exit(1);
    }
    execv(prog, argv);
    _exit(1);
}

static void run_single_pipe(char **argvL, char **argvR, int redirect_in, char *infile,
                            int redirect_out, int append, char *outfile)
{
    int pfd[2];
    if (pipe(pfd) < 0)
    {
        fprintf(stderr, "Error: invalid program\n");
        return;
    }

    pid_t pid1 = fork();
    if (pid1 < 0)
    {
        fprintf(stderr, "Error: invalid program\n");
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (pid1 == 0)
    {
        // child
        child_restore_signals();

        if (redirect_in)
        {
            int fd = open(infile, O_RDONLY);
            if (fd < 0)
            {
                fprintf(stderr, "Error: invalid file\n");
                _exit(1);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }

        dup2(pfd[1], STDOUT_FILENO);

        close(pfd[0]);
        close(pfd[1]);

        exec_with_path(argvL);
    }

    pid_t pid2 = fork();
    if (pid2 < 0)
    {
        fprintf(stderr, "Error: invalid program\n");
        close(pfd[0]);
        close(pfd[1]);
        int st;
        waitpid(pid1, &st, 0);
        return;
    }
    if (pid2 == 0)
    {
        // child
        child_restore_signals();

        dup2(pfd[0], STDIN_FILENO);

        if (redirect_out)
        {
            int fd;
            if (append)
                fd = open(outfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
            else
                fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0)
            {
                fprintf(stderr, "Error: invalid file\n");
                _exit(1);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }

        close(pfd[0]);
        close(pfd[1]);

        exec_with_path(argvR);
    }

    // parent
    close(pfd[0]);
    close(pfd[1]);

    int st;
    waitpid(pid1, &st, 0);
    waitpid(pid2, &st, 0);
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

        int pipe_idx = find_pipe(argv, argc);

        if (pipe_idx >= 0)
        {
            for (int j = pipe_idx + 1; j < argc; j++)
            {
                if (strcmp(argv[j], "|") == 0)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    continue;
                }
            }

            // pipe cannot be head or tail
            if (pipe_idx == 0 || pipe_idx == argc - 1)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            argv[pipe_idx] = NULL;
            char **argvL = argv;
            int argcL = pipe_idx;
            char **argvR = &argv[pipe_idx + 1];
            int argcR = argc - pipe_idx - 1;

            // builtin cannot be piped
            if (is_builtin(argvL[0]) || is_builtin(argvR[0]))
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            // parse redirections
            int Lin = 0, Lout = 0, Lapp = 0;
            char *Linf = NULL, *Loutf = NULL;
            int Rin = 0, Rout = 0, Rapp = 0;
            char *Rinf = NULL, *Routf = NULL;

            if (parse_redirections(argvL, &argcL, &Lin, &Linf, &Lout, &Lapp, &Loutf) < 0 ||
                parse_redirections(argvR, &argcR, &Rin, &Rinf, &Rout, &Rapp, &Routf) < 0)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            if (Lout || Rin)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            run_single_pipe(argvL, argvR, Lin, Linf, Rout, Rapp, Routf);

            free(argv);
            free(line_copy);
            continue;
        }

        // no pipe
        if (parse_redirections(argv, &argc, &redirect_in, &infile, &redirect_out, &append, &outfile) < 0)
        {
            fprintf(stderr, "Error: invalid command\n");
            free(argv);
            free(line_copy);
            continue;
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