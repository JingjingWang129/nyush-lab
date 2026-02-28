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

typedef struct
{
    int jid;
    pid_t pgid; // process group id
    char *cmdline;
} job_t;

static job_t jobs[128];
static int jobs_n = 0;
static int next_jid = 1;

static int add_job(pid_t pgid, const char *cmdline)
{
    if (jobs_n >= 128)
        return -1;
    jobs[jobs_n].jid = next_jid++;
    jobs[jobs_n].pgid = pgid;
    jobs[jobs_n].cmdline = strdup(cmdline ? cmdline : "");
    jobs_n++;
    suspended_jobs = jobs_n;
    return jobs[jobs_n - 1].jid;
}

static int find_job_index(int jid)
{
    for (int i = 0; i < jobs_n; i++)
        if (jobs[i].jid == jid)
            return i;
    return -1;
}

static void remove_job_index(int idx)
{
    if (idx < 0 || idx >= jobs_n)
        return;
    free(jobs[idx].cmdline);
    jobs[idx] = jobs[jobs_n - 1];
    jobs_n--;
    suspended_jobs = jobs_n;
}

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

static void run_pipeline(char ***argvs, int ncmd, int redirect_in, char *infile,
                         int redirect_out, int append, char *outfile, const char *cmdline)
{
    pid_t pgid = -1;
    int prev_read = -1;
    pid_t shell_pgid = getpgrp();

    for (int i = 0; i < ncmd; i++)
    {
        int pfd[2] = {-1, -1};
        if (i != ncmd - 1)
        {
            if (pipe(pfd) < 0)
            {
                fprintf(stderr, "Error: invalid program\n");
                if (prev_read != -1)
                    close(prev_read);
                return;
            }
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "Error: invalid program\n");
            if (prev_read != -1)
            {
                close(prev_read);
            }
            if (pfd[0] != -1)
            {
                close(pfd[0]);
                close(pfd[1]);
            }
            return;
        }

        if (pid == 0)
        {
            // child
            child_restore_signals();

            if (pgid == -1)
            {
                setpgid(0, 0);
            }
            else
            {
                setpgid(0, pgid);
            }

            // stdin
            if (i == 0 && redirect_in)
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
            else if (i > 0)
            {
                dup2(prev_read, STDIN_FILENO);
            }

            // stdout
            if (i == ncmd - 1 && redirect_out)
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
            else if (i != ncmd - 1)
            {
                dup2(pfd[1], STDOUT_FILENO);
            }

            if (prev_read != -1)
                close(prev_read);
            if (pfd[0] != -1)
                close(pfd[0]);
            if (pfd[1] != -1)
                close(pfd[1]);

            // exec
            char *prog = resolve_program(argvs[i][0]);
            if (!prog)
            {
                fprintf(stderr, "Error: invalid program\n");
                _exit(1);
            }
            execv(prog, argvs[i]);
            _exit(1);
        }

        // parent
        if (pgid == -1)
        {
            pgid = pid;
            setpgid(pid, pgid);
            tcsetpgrp(STDIN_FILENO, pgid);
        }
        else
        {
            setpgid(pid, pgid);
        }

        if (prev_read != -1)
            close(prev_read);

        if (i != ncmd - 1)
        {
            close(pfd[1]);
            prev_read = pfd[0];
        }
    }

    if (prev_read != -1)
        close(prev_read);

    int stopped = 0;
    int live = ncmd;

    while (live > 0)
    {
        int st;
        pid_t w = waitpid(-pgid, &st, WUNTRACED);
        if (w < 0)
            break;

        if (WIFSTOPPED(st))
        {
            stopped = 1;
            break;
        }

        if (WIFEXITED(st) || WIFSIGNALED(st))
        {
            live--;
        }
    }

    tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (stopped)
    {
        add_job(pgid, cmdline);
    }
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

        if (strcmp(argv[0], "jobs") == 0)
        {
            if (argc != 1)
            {
                fprintf(stderr, "Error: invalid command\n");
            }
            else
            {
                for (int i = 0; i < jobs_n; i++)
                {
                    printf("[%d] %s\n", jobs[i].jid, jobs[i].cmdline);
                }
            }
            free(argv);
            free(line_copy);
            continue;
        }

        if (strcmp(argv[0], "fg") == 0)
        {
            if (argc != 2)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            char *end = NULL;
            long jid_l = strtol(argv[1], &end, 10);
            if (!end || *end != '\0' || jid_l <= 0 || jid_l > 1000000)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            int idx = find_job_index((int)jid_l);
            if (idx < 0)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            pid_t pgid = jobs[idx].pgid;
            kill(-pgid, SIGCONT);

            pid_t shell_pgid = getpgrp();
            tcsetpgrp(STDIN_FILENO, pgid);

            int st;
            while (1)
            {
                pid_t w = waitpid(-pgid, &st, WUNTRACED);
                if (w < 0)
                {
                    if (errno == EINTR)
                        continue;
                    break;
                }
                if (WIFSTOPPED(st))
                {
                    break;
                }
                if (WIFEXITED(st) || WIFSIGNALED(st))
                {
                    remove_job_index(idx);
                    break;
                }
            }
            tcsetpgrp(STDIN_FILENO, shell_pgid);

            free(argv);
            free(line_copy);
            continue;
        }

        int has_pipe = 0;
        for (int i = 0; i < argc; i++)
        {
            if (!strcmp(argv[i], "|"))
            {
                has_pipe = 1;
                break;
            }
        }

        if (has_pipe)
        {
            // no head，tail,  ||
            if (!strcmp(argv[0], "|") || !strcmp(argv[argc - 1], "|"))
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }
            for (int i = 0; i < argc - 1; i++)
            {
                if (!strcmp(argv[i], "|") && !strcmp(argv[i + 1], "|"))
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    goto next_prompt;
                }
            }

            // count cmds
            int ncmd = 1;
            for (int i = 0; i < argc; i++)
                if (!strcmp(argv[i], "|"))
                    ncmd++;
            if (ncmd > 512)
            {
                fprintf(stderr, "Error: invalid command\n");
                free(argv);
                free(line_copy);
                continue;
            }

            // split，replace |
            char **argvs[512];
            int idx = 0;
            argvs[idx++] = argv;

            for (int i = 0; i < argc; i++)
            {
                if (!strcmp(argv[i], "|"))
                {
                    argv[i] = NULL;
                    if (i + 1 >= argc)
                    {
                        fprintf(stderr, "Error: invalid command\n");
                        free(argv);
                        free(line_copy);
                        goto next_prompt;
                    }
                    argvs[idx++] = &argv[i + 1];
                }
            }

            int redirect_in = 0, redirect_out = 0, append = 0;
            char *infile = NULL, *outfile = NULL;

            for (int k = 0; k < ncmd; k++)
            {
                if (argvs[k][0] == NULL)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    goto next_prompt;
                }
                if (is_builtin(argvs[k][0]))
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    goto next_prompt;
                }

                int argc_k = 0;
                while (argvs[k][argc_k] != NULL)
                    argc_k++;

                int Kin = 0, Kout = 0, Kapp = 0;
                char *Kinf = NULL, *Koutf = NULL;

                if (parse_redirections(argvs[k], &argc_k, &Kin, &Kinf, &Kout, &Kapp, &Koutf) < 0)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    goto next_prompt;
                }

                if (k != 0 && Kin)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    goto next_prompt;
                }

                if (k != ncmd - 1 && Kout)
                {
                    fprintf(stderr, "Error: invalid command\n");
                    free(argv);
                    free(line_copy);
                    goto next_prompt;
                }

                if (k == 0 && Kin)
                {
                    redirect_in = 1;
                    infile = Kinf;
                }
                if (k == ncmd - 1 && Kout)
                {
                    redirect_out = 1;
                    append = Kapp;
                    outfile = Koutf;
                }
            }

            run_pipeline(argvs, ncmd, redirect_in, infile, redirect_out, append, outfile, line);

            free(argv);
            free(line_copy);
            continue;

        next_prompt:
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
            pid_t shell_pgid = getpgrp();
            setpgid(pid, pid);
            tcsetpgrp(STDIN_FILENO, pid);

            int st;
            waitpid(pid, &st, WUNTRACED);

            tcsetpgrp(STDIN_FILENO, shell_pgid);
            if (WIFSTOPPED(st))
            {
                add_job(pid, line);
            }
        }

        free(prog);
        free(argv);
        free(line_copy);
    }

    free(line);
    return 0;
}