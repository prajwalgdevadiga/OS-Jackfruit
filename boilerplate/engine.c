#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include "monitor_ioctl.h"   /* FIX 1: include so MONITOR_REGISTER macro is available */




struct container_args {
    char *rootfs;
    char command[256];
    int pipe_fd;
};

#define LOG_BUFFER_SIZE 1024

char log_buffer[LOG_BUFFER_SIZE][256];
int log_count = 0;

pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* FIX 4: mutex to protect containers[] and container_count from the
 * data race between the main thread and the SIGCHLD handler.
 * sigprocmask is used around writes in the main thread so the handler
 * never runs while the array is being modified. */
static pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define MAX_CONTAINERS 10
#define CONTROL_PATH "/tmp/mini_runtime.sock"

volatile sig_atomic_t keep_running = 1;

void *log_reader(void *arg) {
    /* FIX 5: arg is now a heap-allocated int so it cannot dangle */
    int *fdp = (int *)arg;
    int fd = *fdp;
    free(fdp);

    char buffer[256];

    while (1) {
        int n = read(fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;

        buffer[n] = '\0';

        pthread_mutex_lock(&log_lock);
        if (log_count < LOG_BUFFER_SIZE) {
            strcpy(log_buffer[log_count++], buffer);
        }
        pthread_mutex_unlock(&log_lock);

        printf("[LOG] %s", buffer);
        fflush(stdout);
    }

    close(fd);
    return NULL;
}

/* ---------------- CONTAINER STRUCT ---------------- */

struct container {
    char id[32];
    pid_t pid;
    char state[16];
};

struct container containers[MAX_CONTAINERS];
int container_count = 0;

/* ---------------- REQUEST STRUCT ---------------- */

typedef struct {
    int cmd;
    char id[64];
    char rootfs[256];
    char command[256];
} control_request_t;

/* ---------------- PRINT ---------------- */

void print_containers() {
    printf("\nContainer List:\n");
    printf("ID\tPID\tSTATE\n");

    for (int i = 0; i < container_count; i++) {
        printf("%s\t%d\t%s\n",
               containers[i].id,
               containers[i].pid,
               containers[i].state);
    }
}

/* ---------------- SIGNALS ---------------- */

void handle_sigint(int sig) {
    (void)sig;
    printf("\n[Supervisor] Shutting down...\n");
    keep_running = 0;
}

void handle_sigchld(int sig) {
    (void)sig;

    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        printf("[Supervisor] Reaped child %d\n", pid);

        /* FIX 4: lock the array before touching it from signal context.
         * pthread_mutex_lock is not async-signal-safe in general, but
         * since the main thread always blocks SIGCHLD with sigprocmask
         * while holding this mutex (see run_supervisor), there is no
         * deadlock risk here. */
        pthread_mutex_lock(&containers_lock);

        for (int i = 0; i < container_count; i++) {
            if (containers[i].pid == pid) {
                if (WIFEXITED(status)) {
                    strcpy(containers[i].state, "exited");
                } else if (WIFSIGNALED(status)) {
                    strcpy(containers[i].state, "killed");
                }
            }
        }

        pthread_mutex_unlock(&containers_lock);

        print_containers();
    }
}

/* ---------------- CONTAINER ---------------- */

static int container_func(void *arg) {
    struct container_args *args = (struct container_args *)arg;

    char *rootfs = args->rootfs;
    int pipe_fd = args->pipe_fd;

    printf("[Container] Starting...\n");

    sethostname("container", 9);

    if (chroot(rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    chdir("/");

    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount");
        return 1;
    }

    dup2(pipe_fd, STDOUT_FILENO);
    dup2(pipe_fd, STDERR_FILENO);
    close(pipe_fd);

    char *args_exec[] = {
        "/bin/sh",
        "-c",
        "while true; do echo running; sleep 2; done",
        NULL
    };

    char *cmd = args->command;

    if (cmd != NULL && strlen(cmd) > 0) {
        char *new_args[] = {cmd, NULL};
        execvp(new_args[0], new_args);
        perror("execvp failed");
        return 1;   /* FIX 8: must return here — do not fall through */
    } else {
        execvp(args_exec[0], args_exec);
    }

    perror("exec");
    return 1;
}

/* ---------------- CLIENT ---------------- */

static int send_control_request(const control_request_t *req) {
    int sock;
    struct sockaddr_un addr;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (write(sock, req, sizeof(*req)) < 0) {
        perror("write");
        close(sock);
        return 1;
    }

    char buffer[128] = {0};
    read(sock, buffer, sizeof(buffer));
    printf("%s", buffer);

    close(sock);
    return 0;
}

/* ---------------- CMD PS ---------------- */

static int cmd_ps() {
    control_request_t req;
    req.cmd = 1;
    return send_control_request(&req);
}

/* ---------------- SUPERVISOR ---------------- */

static int run_supervisor(const char *rootfs_input) {

    char rootfs[PATH_MAX];

    if (realpath(rootfs_input, rootfs) == NULL) {
        perror("realpath");
        exit(1);
    }

    printf("Supervisor started with rootfs: %s\n", rootfs);

    signal(SIGCHLD, handle_sigchld);
    signal(SIGINT, handle_sigint);

    int server_fd;
    struct sockaddr_un addr;

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    chmod(CONTROL_PATH, 0777);

    if (listen(server_fd, 5) < 0) {
        perror("listen");
        exit(1);
    }

    printf("[Supervisor] Control socket ready\n");

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;

    print_containers();

    /* sigset used to block SIGCHLD while we modify containers[] (FIX 4) */
    sigset_t sigchld_mask, old_mask;
    sigemptyset(&sigchld_mask);
    sigaddset(&sigchld_mask, SIGCHLD);

    while (keep_running) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (!keep_running) break;
            continue;
        }

        control_request_t req;

        if (read(client_fd, &req, sizeof(req)) > 0) {

            if (req.cmd == 1) {
                printf("[Supervisor] PS + LOGS command\n");

                print_containers();

                printf("\n---- LOGS ----\n");

                pthread_mutex_lock(&log_lock);
                for (int i = 0; i < log_count; i++) {
                    printf("%s", log_buffer[i]);
                }
                pthread_mutex_unlock(&log_lock);

                printf("\n--------------\n");
            }

            else if (req.cmd == 2) {
                printf("[Supervisor] START command received for %s\n", req.id);

                int pipefd[2];
                pipe(pipefd);

                char *stack = malloc(STACK_SIZE);

                char *abs_rootfs = realpath(req.rootfs, NULL);
                if (!abs_rootfs) {
                    perror("realpath");
                    free(stack);
                    close(client_fd);
                    continue;
                }

                struct container_args *args = malloc(sizeof(struct container_args));
                args->rootfs = abs_rootfs;
                args->pipe_fd = pipefd[1];
                strcpy(args->command, req.command);

                pid_t pid = clone(container_func, stack + STACK_SIZE, flags, args);

                /* FIX 3: free heap allocations after clone() regardless of outcome */
                free(stack);
                free(abs_rootfs);
                free(args);

                if (pid < 0) {
                    perror("clone");
                    close(pipefd[0]);
                    close(pipefd[1]);
                    close(client_fd);
                    continue;
                }

                /* FIX 1: use correct field names + MONITOR_REGISTER macro */
                int mon_fd = open("/dev/container_monitor", O_RDWR);
                if (mon_fd >= 0) {
                    /* FIX 2: use a differently-named variable so it does not
                     * shadow the outer control_request_t req */
                    struct monitor_request mon_req;
                    mon_req.pid              = pid;
                    mon_req.soft_limit_bytes = 1000000;
                    mon_req.hard_limit_bytes = 2000000;
                    ioctl(mon_fd, MONITOR_REGISTER, &mon_req);
                    close(mon_fd);
                } else {
                    perror("open /dev/container_monitor");
                }

                close(pipefd[1]);

                /* FIX 5: heap-allocate the read-end fd so the thread arg
                 * cannot dangle when the enclosing scope exits */
                int *read_fd = malloc(sizeof(int));
                *read_fd = pipefd[0];

                pthread_t tid;
                pthread_create(&tid, NULL, log_reader, read_fd);
                /* FIX 6: detach so the thread cleans up its own resources */
                pthread_detach(tid);

                /* FIX 4: block SIGCHLD while modifying the shared array */
                sigprocmask(SIG_BLOCK, &sigchld_mask, &old_mask);
                pthread_mutex_lock(&containers_lock);

                strcpy(containers[container_count].id, req.id);
                containers[container_count].pid = pid;
                strcpy(containers[container_count].state, "running");
                container_count++;

                pthread_mutex_unlock(&containers_lock);
                sigprocmask(SIG_SETMASK, &old_mask, NULL);

                printf("[Supervisor] Started container %s (PID %d)\n", req.id, pid);
            }

            else if (req.cmd == 3) {
                printf("[Supervisor] STOP command received for %s\n", req.id);

                int found = 0;

                for (int i = 0; i < container_count; i++) {
                    if (strcmp(containers[i].id, req.id) == 0) {

                        found = 1;

                        if (kill(containers[i].pid, SIGKILL) == 0) {
                            strcpy(containers[i].state, "stopping");
                            printf("[Supervisor] Sent SIGKILL to %s (PID %d)\n",
                                   req.id, containers[i].pid);
                        } else {
                            perror("kill");
                        }

                        break;
                    }
                }

                if (!found) {
                    printf("[Supervisor] Container %s not found\n", req.id);
                }
            }

            write(client_fd, "OK\n", 3);
        }

        close(client_fd);
    }

    close(server_fd);
    unlink(CONTROL_PATH);

    printf("[Supervisor] Clean exit\n");
    return 0;
}

/* ---------------- MAIN ---------------- */

int main(int argc, char *argv[]) {

    if (argc < 2) {
        printf("Usage: %s supervisor <rootfs>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "ps") == 0) {
        return cmd_ps();
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 4) {
            printf("Usage: start <id> <rootfs>\n");
            return 1;
        }

        control_request_t req;
        req.cmd = 2;

        strcpy(req.id, argv[2]);
        strcpy(req.rootfs, argv[3]);

        if (argc >= 5) {
            strcpy(req.command, argv[4]);
        } else {
            req.command[0] = '\0';
        }

        return send_control_request(&req);
    }

    if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) {
            printf("Usage: stop <id>\n");
            return 1;
        }

        control_request_t req;
        req.cmd = 3;

        strcpy(req.id, argv[2]);

        return send_control_request(&req);
    }

    printf("Unknown command\n");
    return 1;
}
