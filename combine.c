#define _POSIX_C_SOURCE 200809L

#include <unistd.h>     /* STDOUT_FILENO, STDERR_FILENO */
#include <stdio.h>      /* dprintf */
#include <err.h>        /* err, warn */
#include <poll.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>

#define MESSAGE_SIZE 72
#define BUFFER_SIZE 1024
#define PREFIX_SIZE 9

// Function to return the minimum of two integers
int min(int a, int b) {
    return a < b ? a : b;
}

// Function to write a message to a file descriptor
int write_message(int fd, const char message_buffer[MESSAGE_SIZE], int message_len) {
    ssize_t total = 0;
    ssize_t written = 0;

    // Write the message to the file descriptor in a loop until the whole message is written
    while (total < message_len) {
        if ((written = write(fd, message_buffer + total, message_len - total)) < 0) {
            break;
        }
        total += written;
    }

    // Return -1 if there was an error writing the message
    if (written == -1) {
        return -1;
    }

    return 0;
}

// Function to shift buffer content to the left by a specified number of bytes
void shift_buffer(char (*buffer)[BUFFER_SIZE], int to_copy) {
    for (int i = to_copy; i < BUFFER_SIZE; i++) {
        (*buffer)[i - to_copy] = (*buffer)[i];
        (*buffer)[i] = 0;
    }
}

int wait_poll(struct pollfd *pfds, int fd_index) {
    while (true) {
            if (poll(pfds, 2, -1) == -1) {
                return -1;
            }

            if (pfds[fd_index].revents & (POLLIN | POLLHUP)) {
                return 0;
            }
        }
}

// Function to read a message from the poll file descriptor and store it in the message buffer
int get_message(struct pollfd *pfds, int fd_index, char (*read_buffer)[BUFFER_SIZE],
                char (*message_buffer)[MESSAGE_SIZE], ssize_t *to_read, int *cropped, bool *is_new_line) {
    int message_len = 0;
    ssize_t read_bytes = *to_read;
    char *end = NULL;

    while (true) {
        // Look for a newline character in the read buffer
        end = memchr(*read_buffer, '\n', read_bytes);

        // Copy data from the read buffer to the message buffer
        if (message_len < MESSAGE_SIZE) {
            int to_add = MESSAGE_SIZE - message_len;
            memcpy(*message_buffer + message_len, read_buffer, min(to_add, read_bytes));
        }

        // If a newline character is found, finalize the message and shift the buffer
        if (end != NULL) {
            message_len += end - (*read_buffer) + 1;
            (*message_buffer)[min(message_len, MESSAGE_SIZE - 1)] = '\n';
            shift_buffer(read_buffer, message_len);

            *to_read = read_bytes - message_len > 0 ? read_bytes - message_len : 0;
            *cropped = message_len - MESSAGE_SIZE;
            *is_new_line = true;

            return message_len < MESSAGE_SIZE ? message_len : MESSAGE_SIZE;
        }

        message_len += read_bytes;

        if (wait_poll(pfds, fd_index) == -1) {
            return -1;
        }

        // Read the data from the file descriptor
        if (pfds[fd_index].revents & POLLIN) {
            if ((read_bytes = read(pfds[fd_index].fd, *read_buffer, BUFFER_SIZE)) <= 0) {
                break;
            }
        } else {
            break;
        }
    }

    if (read_bytes == -1) {
        return -1;
    }
    if (message_len > 0) {
        (*message_buffer)[min(message_len, MESSAGE_SIZE - 1)] = '\n';
        message_len++;
    }
    *cropped = message_len - MESSAGE_SIZE;
    *is_new_line = false;
    memset(*read_buffer, 0, BUFFER_SIZE);
    *to_read = 0;
    
    return message_len < MESSAGE_SIZE ? message_len : MESSAGE_SIZE;
}

// Function to read from a file descriptor and write the output with a prefix to another file descriptor
int read_and_write(struct pollfd *pfds, int out_fd, int fd_index, char (*read_buffer)[BUFFER_SIZE], const char *prefix, ssize_t *to_read) {
    char message[MESSAGE_SIZE] = {0};
    char actual_message[MESSAGE_SIZE + PREFIX_SIZE] = {0};
    int message_len = 0;
    int cropped = 0;
    bool is_new_line = false;

    // Get the message from the file descriptor
    if ((message_len = get_message(pfds, fd_index, read_buffer, &message, to_read, &cropped, &is_new_line)) == -1) {
        return -1;
    }

    if (message_len > 0) {
        // Add the prefix to the message
        snprintf(actual_message, PREFIX_SIZE + 1, "[%s] ", prefix);
        memcpy(actual_message + PREFIX_SIZE, message, message_len);

        // Write the message to the output file descriptor
        if (write_message(out_fd, actual_message, message_len + PREFIX_SIZE) == -1) {
            return -1;
        }

        // Write a message indicating the number of cropped characters
        if (cropped > 0) {
            message_len = snprintf(actual_message, MESSAGE_SIZE, "%s: cropped %d characters\n", prefix, cropped);
            if (write_message(out_fd, actual_message, message_len) == -1) {
                return -1;
            }
        }

        // Write a message indicating if there was no newline character at the end of the message
        if (!is_new_line) {
            message_len = snprintf(actual_message, MESSAGE_SIZE, "%s: no newline\n", prefix);
            if (write_message(out_fd, actual_message, message_len) == -1) {
                return -1;
            }
        }
    }
    return message_len;
}

// Function to combine stdout and stderr outputs into a single file descriptor
int combine_outputs(int fd_stdout, int fd_stderr, int fd_out) {
    struct pollfd pfds[2];
    
    pfds[0].fd = fd_stdout;
    pfds[0].events = POLLIN;
    pfds[1].fd = fd_stderr;
    pfds[1].events = POLLIN;

    char stdout_read_buffer[BUFFER_SIZE] = {0};
    char stderr_read_buffer[BUFFER_SIZE] = {0};

    ssize_t stdout_to_read = 0;
    ssize_t stderr_to_read = 0;
    int read_bytes1 = 0;
    int read_bytes2 = 0;

    // Loop to read from both stdout and stderr, and write the output to a single file descriptor
    while (true) {
        if ((read_bytes1 = read_and_write(pfds, fd_out, 0, &stdout_read_buffer, "STDOUT", &stdout_to_read)) == -1) {
            return -1;
        }
        if ((read_bytes2 = read_and_write(pfds, fd_out, 1, &stderr_read_buffer, "STDERR", &stderr_to_read)) == -1) {
            return -1;
        }
        if (!(stdout_to_read > 0 || stderr_to_read > 0 || read_bytes1 > 0 || read_bytes2 > 0)) {
            break;
        }
    }

    return 0;
}

// Main combine function to execute a program and combine its stdout and stderr
int combine(char **argv, int fd_out, int *status) {
    int stdout_pipe_fds[2] = {0};
    int stderr_pipe_fds[2] = {0};
    int retval = 0;

    // Create pipes for stdout and stderr
    if ((pipe(stdout_pipe_fds) == -1) || (pipe(stderr_pipe_fds) == -1)) {
        retval = -1;
        goto err;
    }

    // Fork a child process to execute the program
    int pid = fork();
    if (pid == -1) {
        retval = -1;
        goto err;
    }

    if (pid == 0) { // Child process
        close(stdout_pipe_fds[0]);
        close(stderr_pipe_fds[0]);

        // Redirect stdout and stderr to the pipes
        if (dup2(stdout_pipe_fds[1], STDOUT_FILENO) == -1 || dup2(stderr_pipe_fds[1], STDERR_FILENO) == -1) {
            exit(100);
        }
        close(stdout_pipe_fds[1]);
        close(stderr_pipe_fds[1]);

        // Execute the program with the provided arguments
        execvp(argv[0], argv);
        exit(100);
    } else { // Parent process
        close(stdout_pipe_fds[1]);
        close(stderr_pipe_fds[1]);

        // Combine the outputs from the pipes and write them to the output file descriptor
        if (combine_outputs(stdout_pipe_fds[0], stderr_pipe_fds[0], fd_out) == -1) {
            return -1;
        }

        // Wait for the child process to finish
        if (waitpid(pid, status, 0) == -1) {
            return -1;
        }
        close(stdout_pipe_fds[0]);
        close(stderr_pipe_fds[0]);

        return 0;
    }

err:
    // Error handling to close the pipe file descriptors
    for (int i = 0; i < 2; i++) {
        if (stdout_pipe_fds[i] != -1) {
            close(stdout_pipe_fds[i]);
        }
        if (stderr_pipe_fds[i] != -1) {
            close(stderr_pipe_fds[i]);
        }
    }
    return retval;
}
