#include "parser.h"

#include "parser.h"

// DEBUG mode to print shell status on each iteration step. Turned off by deefault
// #define DEBUG

#ifdef DEBUG
#include <time.h>
#define DEBUG_PRINT(fmt, ...) do { \
    struct timespec ts; \
    clock_gettime(CLOCK_REALTIME, &ts); \
    fprintf(stderr, "\033[33m[DEBUG %ld.%03ld] %s:%d %s():\033[0m " fmt, \
            ts.tv_sec, ts.tv_nsec/1000000, __FILE__, __LINE__, __func__, ##__VA_ARGS__); \
} while (0)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/stat.h>

#define EXIT_SHELL_CODE 256

static int process_single_operation(struct command *task);
static int process_pipeline_operations(const struct command_line *full_line);
static bool involves_pipeline(const struct command_line *l);
static int execute_job(const struct command_line *job_line, bool background);
static int handle_operation_queue(const struct command_line *full_line);

static bool operation_present(const char *op_name) {
    if (strchr(op_name, '/') != NULL) return access(op_name, X_OK) == 0;
    const char *sys_path = getenv("PATH");
    if (!sys_path) return false;
    char *path_copy = strdup(sys_path);
    if (!path_copy) return false;
    bool found = false;
    char *saveptr;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, op_name);
        if (access(full, X_OK) == 0) { found = true; break; }
        dir = strtok_r(NULL, ":", &saveptr);
    }
    free(path_copy);
    return found;
}

static int process_single_operation(struct command *task) {
    DEBUG_PRINT("Processing command: %s\n", task->exe);
    
    if (strcmp(task->exe, "cd") == 0) {
        const char *target = (task->arg_count > 0) ? task->args[0] : ".";
        if (chdir(target) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }

    if (strcmp(task->exe, "exit") == 0) {
        int code = (task->arg_count > 0) ? atoi(task->args[0]) : 0;
        return EXIT_SHELL_CODE + code;
    }
    
    if (!operation_present(task->exe)) {
        fprintf(stderr, "%s: command not found\n", task->exe);
        return 127;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) {
        DEBUG_PRINT("Child process for %s (PID %d)\n", task->exe, getpid());
        
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd != -1) {
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        
        char **argv = malloc(sizeof(char*) * (task->arg_count + 2));
        if (!argv) _exit(1);
        argv[0] = task->exe;
        for (uint32_t i = 0; i < task->arg_count; ++i) {
            argv[i+1] = task->args[i];
        }
        argv[task->arg_count+1] = NULL;
        
        execvp(task->exe, argv);
        DEBUG_PRINT("execvp failed for %s: %s\n", task->exe, strerror(errno));
        free(argv);
        _exit(1);
    }
    
    DEBUG_PRINT("Parent waiting for PID %d (%s)\n", pid, task->exe);
    int status;
    waitpid(pid, &status, 0);
    DEBUG_PRINT("Process %d exited with status %d\n", pid, WEXITSTATUS(status));
    return (WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
}

static int process_pipeline_operations(const struct command_line *full_line) {
    int count = 0;
    for (struct expr *e = full_line->head; e; e = e->next) {
        if (e->type == EXPR_TYPE_COMMAND) count++;
    }
    if (count == 0) return 0;

    pid_t *pids = malloc(sizeof(pid_t) * count);
    if (!pids) {
        perror("malloc");
        return 1;
    }

    struct command *commands = malloc(sizeof(struct command) * count);
    if (!commands) {
        perror("malloc");
        free(pids);
        return 1;
    }

    int idx = 0;
    for (struct expr *e = full_line->head; e; e = e->next) {
        if (e->type == EXPR_TYPE_COMMAND) {
            commands[idx++] = e->cmd;
        }
    }

    int pipefd[2] = { -1, -1 };
    int in_fd = STDIN_FILENO;

    for (int i = 0; i < count; i++) {
        if (i < count - 1) {
            if (pipe(pipefd) < 0) {
                perror("pipe");
                for (int j = 0; j < i; j++) kill(pids[j], SIGTERM);
                free(pids);
                free(commands);
                return 1;
            }
        }

        pids[i] = fork();
        if (pids[i] < 0) {
            for (int j = 0; j < i; j++) kill(pids[j], SIGTERM);
            if (i < count - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            free(pids);
            free(commands);
            return 1;
        }

        if (pids[i] == 0) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd != -1) {
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }

            if (i > 0) {
                dup2(in_fd, STDIN_FILENO);
            }

            if (i == count - 1 && full_line->out_type != OUTPUT_TYPE_STDOUT) {
                struct stat st;
                const char *outfile = full_line->out_file;

                if (stat(outfile, &st) == 0 && S_ISFIFO(st.st_mode)) {
                    pid_t w1 = fork();
                    if (w1 < 0) _exit(1);
                    if (w1 == 0) {
                        int fdw = open(outfile, O_WRONLY);
                        if (fdw < 0) _exit(1);
                        dup2(fdw, STDOUT_FILENO);
                        close(fdw);
                        int st2 = process_single_operation(&commands[i]);
                        _exit((st2 >= EXIT_SHELL_CODE) ? st2 - EXIT_SHELL_CODE : st2);
                    }
                    _exit(0);
                }

                int flags = (full_line->out_type == OUTPUT_TYPE_FILE_NEW)
                            ? (O_WRONLY | O_CREAT | O_TRUNC)
                            : (O_WRONLY | O_CREAT | O_APPEND);
                int fd = open(outfile, flags, 0666);
                if (fd < 0) _exit(1);
                dup2(fd, STDOUT_FILENO);
                close(fd);
            }
            else if (i < count - 1) {
                dup2(pipefd[1], STDOUT_FILENO);
            }

            if (i < count - 1) {
                close(pipefd[0]);
                close(pipefd[1]);
            }
            if (i > 0) {
                close(in_fd);
            }

            if (strcmp(commands[i].exe, "exit") == 0) {
                int code = (commands[i].arg_count > 0) ? atoi(commands[i].args[0]) : 0;
                _exit(code);
            }

            char **argv = malloc(sizeof(char*) * (commands[i].arg_count + 2));
            if (!argv) _exit(1);
            argv[0] = commands[i].exe;
            for (uint32_t j = 0; j < commands[i].arg_count; ++j) {
                argv[j+1] = commands[i].args[j];
            }
            argv[commands[i].arg_count+1] = NULL;
            execvp(commands[i].exe, argv);
            free(argv);
            _exit(1);
        }

        if (i > 0) {
            close(in_fd);
        }
        if (i < count - 1) {
            close(pipefd[1]);
            in_fd = pipefd[0];
        }
    }

    if (in_fd != STDIN_FILENO) {
        close(in_fd);
    }

    int final_status = 0, last_exit_index = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(commands[i].exe, "exit") == 0) {
            last_exit_index = i;
        }
    }
    for (int i = 0; i < count; ++i) {
        int st;
        while (waitpid(pids[i], &st, 0) < 0) {
            if (errno != EINTR) break;
        }
        if (last_exit_index < 0 && i == count - 1) {
            final_status = WEXITSTATUS(st);
        }
    }
    if (last_exit_index >= 0) {
        final_status = (commands[last_exit_index].arg_count > 0)
                         ? atoi(commands[last_exit_index].args[0])
                         : 0;
    }

    free(pids);
    free(commands);
    return final_status;
}

static bool involves_pipeline(const struct command_line *l) {
    if (!l || !l->head) return false;
    struct expr *current = l->head;
    while (current) {
        if (current->type == EXPR_TYPE_PIPE) {
            return true;
        }
        current = current->next;
    }
    return false;
}

static int execute_job(const struct command_line *job_line, bool background) {
    DEBUG_PRINT("Executing job (background=%d)\n", background);

    if (!job_line->head) return 0;

    if (involves_pipeline(job_line)) {
        DEBUG_PRINT("Job involves pipeline\n");
        if (background) {
            DEBUG_PRINT("Forking for background pipeline\n");
            pid_t bg_pid = fork();
            if (bg_pid < 0) {
                perror("fork");
                return 1;
            }
            if (bg_pid == 0) {
                DEBUG_PRINT("Background pipeline child (PID %d)\n", getpid());
                
                setsid();
                
                int null_in = open("/dev/null", O_RDONLY);
                if (null_in != -1) {
                    dup2(null_in, STDIN_FILENO);
                    close(null_in);
                }
                int null_fd = open("/dev/null", O_WRONLY);
                if (null_fd != -1) {
                    dup2(null_fd, STDOUT_FILENO);
                    dup2(null_fd, STDERR_FILENO);
                    close(null_fd);
                }
                
                int status = process_pipeline_operations(job_line);
                DEBUG_PRINT("Background pipeline child exiting with %d\n", status);
                _exit(status);
            }
            DEBUG_PRINT("Background pipeline launched with PID %d\n", bg_pid);
            return 0;
        }
        return process_pipeline_operations(job_line);
    }
    
    if (job_line->out_type != OUTPUT_TYPE_STDOUT) {
        struct stat st;
        const char *outfile = job_line->out_file;

        if (stat(outfile, &st) == 0 && S_ISFIFO(st.st_mode)) {
            DEBUG_PRINT("Detected FIFO %s → using non-blocking double-fork\n", outfile);
            pid_t p1 = fork();
            if (p1 < 0) {
                perror("fork");
                return 1;
            }
            if (p1 == 0) {
                setsid();
                pid_t p2 = fork();
                if (p2 < 0) _exit(1);
                if (p2 == 0) {
                    DEBUG_PRINT("FIFO-writer (PID %d) opening blocking\n", getpid());
                    int fd = open(outfile, O_WRONLY);
                    if (fd < 0) _exit(1);
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                    int st2 = process_single_operation(&job_line->head->cmd);
                    _exit((st2 >= EXIT_SHELL_CODE) ? st2 - EXIT_SHELL_CODE : st2);
                }
                _exit(0);
            }
            waitpid(p1, NULL, 0);
            return 0;
        }

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        if (pid == 0) {
            if (background) {
                int null_in = open("/dev/null", O_RDONLY);
                if (null_in != -1) {
                    dup2(null_in, STDIN_FILENO);
                    close(null_in);
                }
            }

            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd != -1) {
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }

            int open_flags = (job_line->out_type == OUTPUT_TYPE_FILE_NEW)
                ? (O_WRONLY | O_CREAT | O_TRUNC)
                : (O_WRONLY | O_CREAT | O_APPEND);
            int fd = open(outfile, open_flags, 0666);
            if (fd < 0) _exit(1);
            dup2(fd, STDOUT_FILENO);
            close(fd);

            int status = process_single_operation(&job_line->head->cmd);
            _exit((status >= EXIT_SHELL_CODE) ? status - EXIT_SHELL_CODE : status);
        }

        if (background) {
            return 0;
        }
        int wst;
        waitpid(pid, &wst, 0);
        return WEXITSTATUS(wst);
    }

    if (background) {
        pid_t bg_pid = fork();
        if (bg_pid < 0) {
            perror("fork");
            return 1;
        }
        if (bg_pid == 0) {
            setsid();
            int null_in = open("/dev/null", O_RDONLY);
            if (null_in != -1) {
                dup2(null_in, STDIN_FILENO);
                close(null_in);
            }
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd != -1) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
            int status = process_single_operation(&job_line->head->cmd);
            _exit((status >= EXIT_SHELL_CODE) ? status - EXIT_SHELL_CODE : status);
        }
        return 0;
    }

    return process_single_operation(&job_line->head->cmd);
}

static int handle_operation_queue(const struct command_line *full_line) {
    DEBUG_PRINT("Handling operation queue\n");
    if (!full_line || !full_line->head) {
        return 0;
    }

    bool background = false;
    struct expr *current = full_line->head;
    struct expr *prev = NULL;
    struct expr *last_cmd = NULL;
    
    while (current) {
        if (current->type == EXPR_TYPE_COMMAND) {
            last_cmd = current;
        }
        prev = current;
        current = current->next;
    }
    
    if (last_cmd && strcmp(last_cmd->cmd.exe, "&") == 0 && last_cmd->cmd.arg_count == 0) {
        background = true;
        
        if (prev && prev != last_cmd) {
            struct expr *p = full_line->head;
            while (p && p->next != last_cmd) p = p->next;
            if (p) {
                p->next = NULL;
            }
        } else {
            return 0;
        }
    }

    if (background) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        
        if (pid == 0) {
            int null_in = open("/dev/null", O_RDONLY);
            if (null_in != -1) {
                dup2(null_in, STDIN_FILENO);
                close(null_in);
            }
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd != -1) {
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
            
            setsid();
            int status = handle_operation_queue(full_line);
            exit(status);
        }
        return 0;
    }

    int last_status = 0;
    bool should_run_next = true;
    current = full_line->head;

    while (current) {
        struct expr *job_start = current;
        struct expr *job_end = current;
        
        while (job_end) {
            if (job_end->type == EXPR_TYPE_AND || job_end->type == EXPR_TYPE_OR) {
                break;
            }
            job_end = job_end->next;
        }

        struct command_line job_line = *full_line;
        job_line.head = job_start;
        bool is_last_job = (job_end == NULL);
        job_line.out_type = is_last_job ? full_line->out_type : OUTPUT_TYPE_STDOUT;
        job_line.out_file = is_last_job ? full_line->out_file : NULL;

        struct expr *saved_next = NULL;
        if (job_end) {
            saved_next = job_end->next;
            job_end->next = NULL;
        }

        int job_status = 0;
        if (should_run_next) {
            job_status = execute_job(&job_line, background);
        }

        if (job_status >= EXIT_SHELL_CODE) {
            if (job_end) {
                job_end->next = saved_next;
            }
            return job_status;
        }

        if (job_end) {
            job_end->next = saved_next;
            enum expr_type op = job_end->type;
            last_status = job_status;
            
            if (op == EXPR_TYPE_AND) {
                should_run_next = (last_status == 0);
            } else if (op == EXPR_TYPE_OR) {
                should_run_next = (last_status != 0);
            }
            current = job_end->next;
        } else {
            last_status = job_status;
            current = NULL;
        }
    }

    return last_status;
}

void sigchld_handler(int sig) {
    (void)sig;
    DEBUG_PRINT("SIGCHLD received\n");
    int saved_errno = errno;
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        DEBUG_PRINT("Reaped child %d\n", pid);
    }
    errno = saved_errno;
}

int main() {
    DEBUG_PRINT("Shell starting (PID %d)\n", getpid());

    signal(SIGINT, SIG_IGN);

    struct parser *parser = parser_new();
    if (!parser) {
        perror("parser_new");
        return 1;
    }
    
    bool interactive = isatty(STDIN_FILENO);
    char buf[4096];
    int exit_status = 0;
    
    while (1) {
        DEBUG_PRINT("Top of main loop\n");
        
        int child_status;
        while (waitpid(-1, &child_status, WNOHANG) > 0) {
            DEBUG_PRINT("Non-blocking wait found exited child\n");
        }

        if (interactive) {
            printf("> ");
            fflush(stdout);
        }
        
        ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
        if (r < 0) { 
            if (errno == EINTR) continue; 
            perror("read"); 
            break; 
        }
        
        if (r == 0) {
            DEBUG_PRINT("EOF detected\n");
            if (interactive) {
                printf("\n");
                parser_feed(parser, "\n", 1);
            } else {
                parser_feed(parser, "", 0);
            }
            struct command_line *cl2 = NULL;
            while (parser_pop_next(parser, &cl2) == PARSER_ERR_NONE && cl2) {
                DEBUG_PRINT("Processing command from EOF buffer\n");
                exit_status = handle_operation_queue(cl2);
                command_line_delete(cl2);
                cl2 = NULL;
                if (exit_status >= EXIT_SHELL_CODE) {
                    exit_status -= EXIT_SHELL_CODE;
                    parser_delete(parser);
                    return exit_status;
                }
            }
            break;
        }
        
        DEBUG_PRINT("Read %zd bytes from stdin\n", r);
        parser_feed(parser, buf, r);
        
        struct command_line *cl = NULL;
        while (1) {
            enum parser_error err = parser_pop_next(parser, &cl);
            if (err == PARSER_ERR_NONE && !cl) {
                DEBUG_PRINT("Parser returned no command\n");
                break;
            }
            
            if (err != PARSER_ERR_NONE) { 
                fprintf(stderr, "Parser error\n"); 
                break; 
            }
            
            DEBUG_PRINT("Processing new command line\n");
            exit_status = handle_operation_queue(cl);
            DEBUG_PRINT("Command processed with status %d\n", exit_status);
            command_line_delete(cl);
            cl = NULL;
            
            if (exit_status >= EXIT_SHELL_CODE) {
                exit_status -= EXIT_SHELL_CODE;
                parser_delete(parser);
                return exit_status;
            }
        }
    }
    
    DEBUG_PRINT("Shell exiting with status %d\n", exit_status);
    parser_delete(parser);
    return exit_status;
}