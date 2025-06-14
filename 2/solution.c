#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

static int process_single_operation(struct command *task);
static int process_pipeline_operations(const struct command_line *full_line);
static bool involves_pipeline(const struct command_line *full_line);
static bool operation_present(const char *op_name);
static int handle_operation_queue(const struct command_line *full_line);

static bool
operation_present(const char *op_name)
{
    if (strchr(op_name, '/') != NULL) {
        return access(op_name, X_OK) == 0;
    }
    
    const char *sys_path = getenv("PATH");
    if (sys_path == NULL) {
        return false;
    }
    
    char *path_copy_val = strdup(sys_path);
    if (path_copy_val == NULL) {
        return false;
    }
    
    bool found_op = false;
    char *folder_entry = strtok(path_copy_val, ":");
    
    while (folder_entry != NULL) {
        char complete_path[4096];
        snprintf(complete_path, sizeof(complete_path), "%s/%s", folder_entry, op_name);
        
        if (access(complete_path, X_OK) == 0) {
            found_op = true;
            break;
        }
        
        folder_entry = strtok(NULL, ":");
    }
    
    free(path_copy_val);
    return found_op;
}

static int
process_single_operation(struct command *task)
{
    if (strcmp(task->exe, "cd") == 0) {
        const char *target_path = ".";
        if (task->arg_count > 0)
            target_path = task->args[0];
        
        if (chdir(target_path) != 0) {
            return 1;
        }
        return 0;
    } 
    
    if (strcmp(task->exe, "exit") == 0) {
        int exit_code_val = 0;
        if (task->arg_count > 0)
            exit_code_val = atoi(task->args[0]);
        exit(exit_code_val);
    }
    
    if (!operation_present(task->exe)) {
        return 1;
    }
    
    pid_t child_process_id = fork();
    
    if (child_process_id < 0) {
        return 1;
    }
    
    if (child_process_id == 0) {
        int null_device = open("/dev/null", O_WRONLY);
        if (null_device != -1) {
            dup2(null_device, STDERR_FILENO);
            close(null_device);
        }
        
        char **arguments = malloc(sizeof(char *) * (task->arg_count + 2));
        if (arguments == NULL) {
            exit(1);
        }
        
        arguments[0] = task->exe;
        for (uint32_t i = 0; i < task->arg_count; i++) {
            arguments[i + 1] = task->args[i];
        }
        arguments[task->arg_count + 1] = NULL;
        
        execvp(task->exe, arguments);
        free(arguments);
        exit(1);
    } else {
        int op_status;
        waitpid(child_process_id, &op_status, 0);
        return WEXITSTATUS(op_status);
    }
}

static int
process_pipeline_operations(const struct command_line *full_line)
{
    int operation_counter = 0;
    struct expr *current_expression = full_line->head;
    while (current_expression != NULL) {
        if (current_expression->type == EXPR_TYPE_COMMAND)
            operation_counter++;
        current_expression = current_expression->next;
    }
    
    if (operation_counter == 0)
        return 0;
    
    struct command *operations_array = malloc(sizeof(struct command) * operation_counter);
    if (operations_array == NULL) {
        perror("malloc");
        return 1;
    }
    
    current_expression = full_line->head;
    int current_op_index = 0;
    while (current_expression != NULL) {
        if (current_expression->type == EXPR_TYPE_COMMAND) {
            operations_array[current_op_index++] = current_expression->cmd;
        }
        current_expression = current_expression->next;
    }

    int pipe_descriptors[2][2]; 
    pid_t *child_pids = malloc(sizeof(pid_t) * operation_counter);
    if (child_pids == NULL) {
        perror("malloc");
        free(operations_array);
        return 1;
    }

    for (int i = 0; i < operation_counter; i++) {
        if (i < operation_counter - 1) {
            if (pipe(pipe_descriptors[i % 2]) < 0) {
                perror("pipe");
                for (int j = 0; j < i; j++) {
                    kill(child_pids[j], SIGTERM);
                }
                free(child_pids);
                free(operations_array);
                return 1;
            }
        }

        child_pids[i] = fork();
        if (child_pids[i] < 0) {
            for (int j = 0; j < i; j++) {
                kill(child_pids[j], SIGTERM);
            }
            if (i < operation_counter - 1) {
                close(pipe_descriptors[i % 2][0]);
                close(pipe_descriptors[i % 2][1]);
            }
            free(child_pids);
            free(operations_array);
            return 1;
        }

        if (child_pids[i] == 0) {
            if (i > 0) {
                dup2(pipe_descriptors[(i + 1) % 2][0], STDIN_FILENO);
            }
            if (i < operation_counter - 1) {
                dup2(pipe_descriptors[i % 2][1], STDOUT_FILENO);
            } else if (full_line->out_type != OUTPUT_TYPE_STDOUT) {
                int file_descriptor;
                if (full_line->out_type == OUTPUT_TYPE_FILE_NEW) {
                    file_descriptor = open(full_line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                } else {
                    file_descriptor = open(full_line->out_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
                }
                if (file_descriptor < 0) {
                    exit(1);
                }
                dup2(file_descriptor, STDOUT_FILENO);
                close(file_descriptor);
            }

            if (i < operation_counter - 1) {
                close(pipe_descriptors[i % 2][0]);
                close(pipe_descriptors[i % 2][1]);
            }
            if (i > 0) {
                close(pipe_descriptors[(i + 1) % 2][0]);
                close(pipe_descriptors[(i + 1) % 2][1]);
            }

            if (strcmp(operations_array[i].exe, "exit") == 0) {
                int termination_code = 0;
                if (operations_array[i].arg_count > 0)
                    termination_code = atoi(operations_array[i].args[0]);
                if (i < operation_counter - 1) {
                    close(STDOUT_FILENO);
                }
                exit(termination_code);
            }

            if (strcmp(operations_array[i].exe, "cd") == 0) {
                const char *directory_path = ".";
                if (operations_array[i].arg_count > 0)
                    directory_path = operations_array[i].args[0];
                if (chdir(directory_path) != 0) {
                    exit(1);
                }
                exit(0);
            }

            char **process_arguments = malloc(sizeof(char *) * (operations_array[i].arg_count + 2));
            if (process_arguments == NULL) {
                exit(1);
            }
            process_arguments[0] = operations_array[i].exe;
            for (uint32_t j = 0; j < operations_array[i].arg_count; j++) {
                process_arguments[j + 1] = operations_array[i].args[j];
            }
            process_arguments[operations_array[i].arg_count + 1] = NULL;

            int discard_output = open("/dev/null", O_WRONLY);
            if (discard_output != -1) {
                dup2(discard_output, STDERR_FILENO);
                close(discard_output);
            }

            execvp(operations_array[i].exe, process_arguments);
            free(process_arguments);
            exit(1);
        }

        if (i > 0) {
            close(pipe_descriptors[(i + 1) % 2][0]);
            close(pipe_descriptors[(i + 1) % 2][1]);
        }
    }

    int final_status = 0;
    int exit_op_index = -1;
    for (int i = 0; i < operation_counter; i++) {
        if (strcmp(operations_array[i].exe, "exit") == 0) {
            exit_op_index = i;
        }
    }
    for (int i = 0; i < operation_counter; i++) {
        int individual_op_status;
        waitpid(child_pids[i], &individual_op_status, 0);
        if (i == exit_op_index) {
            final_status = WEXITSTATUS(individual_op_status);
        }
        else if (exit_op_index == -1 && i == operation_counter - 1) {
            final_status = WEXITSTATUS(individual_op_status);
        }
    }
    if (exit_op_index != -1) {
        int command_exit_code = 0;
        if (operations_array[exit_op_index].arg_count > 0) {
            command_exit_code = atoi(operations_array[exit_op_index].args[0]);
        }
        final_status = command_exit_code;
    }
    free(child_pids);
    free(operations_array);
    return final_status;
}

static bool
involves_pipeline(const struct command_line *full_line)
{
    assert(full_line != NULL);
    
    struct expr *current_expr_node = full_line->head;
    while (current_expr_node != NULL && current_expr_node->next != NULL) {
        if (current_expr_node->next->type == EXPR_TYPE_PIPE) {
            return true;
        }
        current_expr_node = current_expr_node->next;
    }
    
    return false;
}

static int
handle_operation_queue(const struct command_line *full_line)
{
    assert(full_line != NULL);
    
    if (full_line->head->type == EXPR_TYPE_COMMAND && 
        strcmp(full_line->head->cmd.exe, "exit") == 0 && 
        full_line->head->next == NULL) {
        
        int termination_code_val = 0;
        if (full_line->head->cmd.arg_count > 0)
            termination_code_val = atoi(full_line->head->cmd.args[0]);
        exit(termination_code_val);
    }
    
    if (full_line->head->type == EXPR_TYPE_COMMAND && 
        strcmp(full_line->head->cmd.exe, "cd") == 0 && 
        full_line->head->next == NULL && 
        full_line->out_type == OUTPUT_TYPE_STDOUT) {
        
        return process_single_operation(&full_line->head->cmd);
    }
    
    if (involves_pipeline(full_line)) {
        return process_pipeline_operations(full_line);
    }
    
    if (full_line->out_type != OUTPUT_TYPE_STDOUT) {
        pid_t new_child_id = fork();
        
        if (new_child_id < 0) {
            return 1;
        }
        
        if (new_child_id == 0) {
            int discard_stream = open("/dev/null", O_WRONLY);
            if (discard_stream != -1) {
                dup2(discard_stream, STDERR_FILENO);
                close(discard_stream);
            }
            
            int target_fd;
            if (full_line->out_type == OUTPUT_TYPE_FILE_NEW) {
                target_fd = open(full_line->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            } else {
                target_fd = open(full_line->out_file, O_WRONLY | O_CREAT | O_APPEND, 0666);
            }
            
            if (target_fd < 0) {
                exit(1);
            }
            
            dup2(target_fd, STDOUT_FILENO);
            close(target_fd);
            
            int command_status = process_single_operation(&full_line->head->cmd);
            
            exit(command_status);
        } else {
            int parent_status;
            waitpid(new_child_id, &parent_status, 0);
            return WEXITSTATUS(parent_status);
        }
    }
    
    return process_single_operation(&full_line->head->cmd);
}

int
main(void)
{
    char input_buffer[4096];
    ssize_t bytes_received;
    
    struct parser *line_parser = parser_new();
    if (line_parser == NULL) {
        perror("parser_new");
        return 1;
    }
    
    bool is_interactive_session = isatty(STDIN_FILENO);
    
    int final_exit_status = 0;
    
    while (1) {
        if (is_interactive_session) {
            printf("> ");
            fflush(stdout);
        }
        
        bytes_received = read(STDIN_FILENO, input_buffer, sizeof(input_buffer));
        
        if (bytes_received <= 0) {
            if (bytes_received == 0 || errno == EINTR) {
                break;
            }
            break;
        }
        
        parser_feed(line_parser, input_buffer, bytes_received);
        
        struct command_line *current_task_line = NULL;
        while (1) {
            enum parser_error parse_err = parser_pop_next(line_parser, &current_task_line);
            if (parse_err == PARSER_ERR_NONE && current_task_line == NULL)
                break;
            
            if (parse_err != PARSER_ERR_NONE) {
                break;
            }
            
            final_exit_status = handle_operation_queue(current_task_line);
            
            command_line_delete(current_task_line);
            current_task_line = NULL;
        }
    }
    
    parser_delete(line_parser);
    
    return final_exit_status;
}