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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "monitor_ioctl.h"

#define STACK_SIZE     (1024*1024)
#define MAX_CONTAINERS 16
#define CONTROL_PATH   "/tmp/mini_runtime.sock"
#define LOGS_DIR       "logs"
#define LOG_BUF_SLOTS  512
#define LOG_LINE_MAX   256
#define DEFAULT_SOFT_MIB 40UL
#define DEFAULT_HARD_MIB 64UL
#define CMD_PS    1
#define CMD_START 2
#define CMD_STOP  3
#define CMD_LOGS  4
#define CMD_RUN   5

typedef struct {
    int cmd;
    char id[64], rootfs[256], command[256];
    unsigned long soft_limit_bytes, hard_limit_bytes;
    int nice_val;
} control_request_t;

typedef struct {
    int exit_code;
    char message[512];
} control_response_t;

typedef struct {
    char lines[LOG_BUF_SLOTS][LOG_LINE_MAX];
    int head, tail, count, done;
    pthread_mutex_t lock;
    pthread_cond_t not_empty, not_full;
} log_buffer_t;

typedef enum {
    STATE_STARTING=0, STATE_RUNNING, STATE_STOPPED,
    STATE_KILLED, STATE_EXITED, STATE_HARD_LIMIT_KILLED
} container_state_t;

static const char *const state_names[] = {
    "starting","running","stopped","killed","exited","hard_limit_killed"
};

typedef struct {
    char id[64];
    pid_t pid;
    container_state_t state;
    int stop_requested, exit_code, nice_val, run_client_fd;
    unsigned long soft_limit_bytes, hard_limit_bytes;
    time_t start_time;
    char log_file[256];
    log_buffer_t log_buf;
    pthread_t producer_tid, consumer_tid;
} container_t;

static container_t containers[MAX_CONTAINERS];
static int container_count = 0;
static pthread_mutex_t containers_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t keep_running = 1;
static sigset_t g_sigchld_mask;

static int send_control_request(const control_request_t *req, control_response_t *resp);

static void safe_mkdir(const char *p) { struct stat st; if (stat(p,&st)!=0) mkdir(p,0755); }
static const char *state_str(container_state_t s) {
    return ((unsigned)s<=(unsigned)STATE_HARD_LIMIT_KILLED) ? state_names[(int)s] : "unknown";
}
static void xwrite(int fd, const void *buf, size_t len) { ssize_t r=write(fd,buf,len); (void)r; }

static void log_buf_init(log_buffer_t *b) {
    memset(b,0,sizeof(*b));
    pthread_mutex_init(&b->lock,NULL);
    pthread_cond_init(&b->not_empty,NULL);
    pthread_cond_init(&b->not_full,NULL);
}
static void log_buf_destroy(log_buffer_t *b) {
    pthread_mutex_destroy(&b->lock);
    pthread_cond_destroy(&b->not_empty);
    pthread_cond_destroy(&b->not_full);
}
static void log_buf_push(log_buffer_t *b, const char *line) {
    pthread_mutex_lock(&b->lock);
    while (b->count==LOG_BUF_SLOTS && !b->done) pthread_cond_wait(&b->not_full,&b->lock);
    if (b->count < LOG_BUF_SLOTS) {
        snprintf(b->lines[b->head],LOG_LINE_MAX,"%s",line);
        b->head=(b->head+1)%LOG_BUF_SLOTS; b->count++;
        pthread_cond_signal(&b->not_empty);
    }
    pthread_mutex_unlock(&b->lock);
}
static void log_buf_mark_done(log_buffer_t *b) {
    pthread_mutex_lock(&b->lock);
    b->done=1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->lock);
}

typedef struct { int pipe_fd; log_buffer_t *buf; } producer_arg_t;
static void *producer_thread(void *arg) {
    producer_arg_t *a=(producer_arg_t*)arg;
    int fd=a->pipe_fd; log_buffer_t *buf=a->buf; free(a);
    char raw[LOG_LINE_MAX], line[LOG_LINE_MAX]; int pos=0; ssize_t n;
    while ((n=read(fd,raw,sizeof(raw)-1))>0) {
        raw[n]='\0';
        for (int i=0;i<(int)n;i++) {
            if (pos<LOG_LINE_MAX-1) line[pos++]=raw[i];
            if (raw[i]=='\n'||pos==LOG_LINE_MAX-1) { line[pos]='\0'; log_buf_push(buf,line); pos=0; }
        }
    }
    if (pos>0) { line[pos]='\0'; log_buf_push(buf,line); }
    close(fd); log_buf_mark_done(buf);
    return NULL;
}

typedef struct { log_buffer_t *buf; char log_file[256]; } consumer_arg_t;
static void *consumer_thread(void *arg) {
    consumer_arg_t *a=(consumer_arg_t*)arg;
    log_buffer_t *buf=a->buf; char path[256];
    snprintf(path,sizeof(path),"%s",a->log_file); free(a);
    FILE *f=fopen(path,"a");
    for (;;) {
        pthread_mutex_lock(&buf->lock);
        while (buf->count==0 && !buf->done) pthread_cond_wait(&buf->not_empty,&buf->lock);
        if (buf->count==0 && buf->done) { pthread_mutex_unlock(&buf->lock); break; }
        char line[LOG_LINE_MAX];
        snprintf(line,sizeof(line),"%s",buf->lines[buf->tail]);
        buf->tail=(buf->tail+1)%LOG_BUF_SLOTS; buf->count--;
        pthread_cond_signal(&buf->not_full);
        pthread_mutex_unlock(&buf->lock);
        if (f) { fputs(line,f); fflush(f); }
        printf("[LOG] %s",line); fflush(stdout);
    }
    if (f) fclose(f);
    return NULL;
}

typedef struct { char rootfs[PATH_MAX],command[256]; int pipe_fd,nice_val; } clone_args_t;
static int container_func(void *arg) {
    clone_args_t *a=(clone_args_t*)arg;
    if (sethostname("container",9)!=0) perror("sethostname");
    if (chroot(a->rootfs)!=0) { perror("chroot"); return 1; }
    if (chdir("/")!=0) { perror("chdir"); return 1; }
    if (mount("proc","/proc","proc",0,NULL)!=0) perror("mount proc");
    if (dup2(a->pipe_fd,STDOUT_FILENO)<0||dup2(a->pipe_fd,STDERR_FILENO)<0) { perror("dup2"); return 1; }
    close(a->pipe_fd);
    /* force unbuffered so every write reaches the pipe immediately */
    setvbuf(stdout,NULL,_IONBF,0);
    setvbuf(stderr,NULL,_IONBF,0);
    if (a->nice_val!=0) setpriority(PRIO_PROCESS,0,a->nice_val);
    if (a->command[0]!='\0') {
        char *argv_sh[]={"/bin/sh","-c",a->command,NULL};
        execvp("/bin/sh",argv_sh); perror("execvp"); return 1;
    }
    /* default heartbeat: pure C loop, dprintf writes directly to fd — no buffering */
    while (1) {
        dprintf(STDOUT_FILENO,"[container pid=%d] running\n",getpid());
        sleep(2);
    }
    return 0;
}

static container_t *find_container(const char *id) {
    for (int i=0;i<container_count;i++)
        if (strcmp(containers[i].id,id)==0) return &containers[i];
    return NULL;
}

static void sig_write(const char *msg, size_t len) {
    ssize_t r; do { r=write(STDOUT_FILENO,msg,len); } while (r<0&&errno==EINTR);
}
static void handle_sigint(int sig) {
    (void)sig; sig_write("\n[Supervisor] SIGINT - shutting down\n",37); keep_running=0;
}
static void handle_sigterm(int sig) {
    (void)sig; sig_write("\n[Supervisor] SIGTERM - shutting down\n",38); keep_running=0;
}
static void handle_sigchld(int sig) {
    (void)sig; int status; pid_t pid;
    while ((pid=waitpid(-1,&status,WNOHANG))>0) {
        pthread_mutex_lock(&containers_lock);
        for (int i=0;i<container_count;i++) {
            container_t *c=&containers[i];
            if (c->pid!=pid) continue;
            int signum=WIFSIGNALED(status)?WTERMSIG(status):0;
            c->exit_code=WIFEXITED(status)?WEXITSTATUS(status):128+signum;
            if (WIFEXITED(status)) c->state=STATE_EXITED;
            else if (WIFSIGNALED(status)) {
                if (c->stop_requested) c->state=STATE_STOPPED;
                else if (signum==SIGKILL) c->state=STATE_HARD_LIMIT_KILLED;
                else c->state=STATE_KILLED;
            }
            if (c->run_client_fd>=0) {
                control_response_t resp; memset(&resp,0,sizeof(resp));
                resp.exit_code=c->exit_code;
                snprintf(resp.message,sizeof(resp.message),
                    "Container '%s' finished: %s (exit_code=%d)\n",
                    c->id,state_str(c->state),c->exit_code);
                xwrite(c->run_client_fd,&resp,sizeof(resp));
                close(c->run_client_fd); c->run_client_fd=-1;
            }
            break;
        }
        pthread_mutex_unlock(&containers_lock);
    }
}

static void register_with_monitor(pid_t pid, const char *id, unsigned long soft, unsigned long hard) {
    int fd=open("/dev/container_monitor",O_RDWR); if (fd<0) return;
    struct monitor_request req; memset(&req,0,sizeof(req));
    req.pid=pid; req.soft_limit_bytes=soft; req.hard_limit_bytes=hard;
    memcpy(req.container_id,id,MONITOR_NAME_LEN-1); req.container_id[MONITOR_NAME_LEN-1]='\0';
    if (ioctl(fd,MONITOR_REGISTER,&req)<0) perror("ioctl MONITOR_REGISTER");
    close(fd);
}
static void unregister_with_monitor(pid_t pid, const char *id) {
    int fd=open("/dev/container_monitor",O_RDWR); if (fd<0) return;
    struct monitor_request req; memset(&req,0,sizeof(req));
    req.pid=pid; memcpy(req.container_id,id,MONITOR_NAME_LEN-1); req.container_id[MONITOR_NAME_LEN-1]='\0';
    (void)ioctl(fd,MONITOR_UNREGISTER,&req); close(fd);
}

static void print_containers_locked(void) {
    printf("\n%-16s %-8s %-22s %-10s %-10s %-10s %-5s\n",
        "ID","PID","STATE","STARTED","SOFT(MiB)","HARD(MiB)","EXIT");
    for (int i=0;i<82;i++) putchar('-');
    putchar('\n');
    for (int i=0;i<container_count;i++) {
        container_t *c=&containers[i]; char ts[32];
        struct tm *t=localtime(&c->start_time); strftime(ts,sizeof(ts),"%H:%M:%S",t);
        printf("%-16s %-8d %-22s %-10s %-10lu %-10lu %-5d\n",
            c->id,c->pid,state_str(c->state),ts,
            c->soft_limit_bytes/(1024UL*1024UL),c->hard_limit_bytes/(1024UL*1024UL),c->exit_code);
    }
    putchar('\n');
}

static int do_start_container(const control_request_t *req, int run_client_fd) {
    pthread_mutex_lock(&containers_lock);
    int dup_id=(find_container(req->id)!=NULL), full=(container_count>=MAX_CONTAINERS);
    pthread_mutex_unlock(&containers_lock);
    if (dup_id) { printf("[Supervisor] ID '%s' already in use\n",req->id); return -1; }
    if (full)   { printf("[Supervisor] Max containers reached\n"); return -1; }

    char abs_rootfs[PATH_MAX];
    if (realpath(req->rootfs,abs_rootfs)==NULL) { perror("realpath"); return -1; }

    int pipefd[2];
    if (pipe(pipefd)<0) { perror("pipe"); return -1; }
    fcntl(pipefd[0],F_SETFD,FD_CLOEXEC);

    clone_args_t *ca=malloc(sizeof(clone_args_t));
    if (!ca) { close(pipefd[0]); close(pipefd[1]); return -1; }
    snprintf(ca->rootfs,sizeof(ca->rootfs),"%s",abs_rootfs);
    snprintf(ca->command,sizeof(ca->command),"%s",req->command);
    ca->pipe_fd=pipefd[1]; ca->nice_val=req->nice_val;

    char *stack=malloc(STACK_SIZE);
    if (!stack) { free(ca); close(pipefd[0]); close(pipefd[1]); return -1; }

    sigset_t old_mask; sigprocmask(SIG_BLOCK,&g_sigchld_mask,&old_mask);
    pid_t pid=clone(container_func,stack+STACK_SIZE,CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS|SIGCHLD,ca);
    close(pipefd[1]); free(stack); free(ca);
    if (pid<0) { perror("clone"); close(pipefd[0]); sigprocmask(SIG_SETMASK,&old_mask,NULL); return -1; }

    unsigned long soft=req->soft_limit_bytes?req->soft_limit_bytes:DEFAULT_SOFT_MIB*1024UL*1024UL;
    unsigned long hard=req->hard_limit_bytes?req->hard_limit_bytes:DEFAULT_HARD_MIB*1024UL*1024UL;
    safe_mkdir(LOGS_DIR);
    char log_file[256]; snprintf(log_file,sizeof(log_file),"%s/%s.log",LOGS_DIR,req->id);

    pthread_mutex_lock(&containers_lock);
    container_t *c=&containers[container_count]; memset(c,0,sizeof(*c));
    snprintf(c->id,sizeof(c->id),"%s",req->id);
    snprintf(c->log_file,sizeof(c->log_file),"%s",log_file);
    c->pid=pid; c->state=STATE_RUNNING; c->stop_requested=0;
    c->start_time=time(NULL); c->exit_code=0;
    c->soft_limit_bytes=soft; c->hard_limit_bytes=hard;
    c->nice_val=req->nice_val; c->run_client_fd=run_client_fd;
    log_buf_init(&c->log_buf);

    producer_arg_t *parg=malloc(sizeof(producer_arg_t));
    parg->pipe_fd=pipefd[0]; parg->buf=&c->log_buf;
    pthread_create(&c->producer_tid,NULL,producer_thread,parg);

    consumer_arg_t *carg=malloc(sizeof(consumer_arg_t));
    carg->buf=&c->log_buf; snprintf(carg->log_file,sizeof(carg->log_file),"%s",log_file);
    pthread_create(&c->consumer_tid,NULL,consumer_thread,carg);

    container_count++;
    pthread_mutex_unlock(&containers_lock);
    sigprocmask(SIG_SETMASK,&old_mask,NULL);

    register_with_monitor(pid,req->id,soft,hard);
    printf("[Supervisor] Started '%s' PID=%d log=%s soft=%luMiB hard=%luMiB\n",
        req->id,pid,log_file,soft/(1024UL*1024UL),hard/(1024UL*1024UL));
    return 0;
}

static int run_supervisor(const char *rootfs_input) {
    char rootfs[PATH_MAX];
    if (realpath(rootfs_input,rootfs)==NULL) { perror("realpath"); return 1; }
    printf("[Supervisor] Started. Base rootfs: %s\n",rootfs);
    safe_mkdir(LOGS_DIR);

    sigemptyset(&g_sigchld_mask); sigaddset(&g_sigchld_mask,SIGCHLD);
    struct sigaction sa; memset(&sa,0,sizeof(sa));
    sa.sa_handler=handle_sigchld; sa.sa_flags=SA_RESTART|SA_NOCLDSTOP;
    sigaction(SIGCHLD,&sa,NULL);
    signal(SIGINT,handle_sigint); signal(SIGTERM,handle_sigterm);

    int server_fd=socket(AF_UNIX,SOCK_STREAM,0);
    if (server_fd<0) { perror("socket"); return 1; }
    unlink(CONTROL_PATH);
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX;
    strncpy(addr.sun_path,CONTROL_PATH,sizeof(addr.sun_path)-1);
    if (bind(server_fd,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("bind"); close(server_fd); return 1; }
    chmod(CONTROL_PATH,0777);
    if (listen(server_fd,8)<0) { perror("listen"); close(server_fd); return 1; }
    fcntl(server_fd,F_SETFL,O_NONBLOCK);
    printf("[Supervisor] Control socket ready at %s\n",CONTROL_PATH);

    while (keep_running) {
        int client_fd=accept(server_fd,NULL,NULL);
        if (client_fd<0) {
            if (errno==EAGAIN||errno==EWOULDBLOCK) { usleep(50000); continue; }
            if (!keep_running) break;
            continue;
        }
        control_request_t req; control_response_t resp; memset(&resp,0,sizeof(resp));
        if (read(client_fd,&req,sizeof(req))<=0) { close(client_fd); continue; }

        if (req.cmd==CMD_PS) {
            pthread_mutex_lock(&containers_lock); print_containers_locked(); pthread_mutex_unlock(&containers_lock);
            snprintf(resp.message,sizeof(resp.message),"Listed %d container(s)\n",container_count);
            xwrite(client_fd,&resp,sizeof(resp)); close(client_fd);
        } else if (req.cmd==CMD_START) {
            printf("[Supervisor] CMD_START '%s'\n",req.id);
            int r=do_start_container(&req,-1);
            snprintf(resp.message,sizeof(resp.message),r==0?"OK: started '%s'\n":"ERR: failed to start '%s'\n",req.id);
            xwrite(client_fd,&resp,sizeof(resp)); close(client_fd);
        } else if (req.cmd==CMD_RUN) {
            printf("[Supervisor] CMD_RUN '%s'\n",req.id);
            int r=do_start_container(&req,client_fd);
            if (r!=0) {
                snprintf(resp.message,sizeof(resp.message),"ERR: failed to start '%s'\n",req.id);
                xwrite(client_fd,&resp,sizeof(resp)); close(client_fd);
            }
        } else if (req.cmd==CMD_LOGS) {
            pthread_mutex_lock(&containers_lock);
            container_t *c=find_container(req.id);
            char log_path[256]={'\0'};
            if (c) snprintf(log_path,sizeof(log_path),"%s",c->log_file);
            pthread_mutex_unlock(&containers_lock);
            if (log_path[0]=='\0') {
                snprintf(resp.message,sizeof(resp.message),"ERR: container '%s' not found\n",req.id);
                xwrite(client_fd,&resp,sizeof(resp)); close(client_fd); continue;
            }
            snprintf(resp.message,sizeof(resp.message),"OK: log file %s\n",log_path);
            xwrite(client_fd,&resp,sizeof(resp));
            int lfd=open(log_path,O_RDONLY);
            if (lfd>=0) { char chunk[512]; ssize_t n; while ((n=read(lfd,chunk,sizeof(chunk)))>0) xwrite(client_fd,chunk,(size_t)n); close(lfd); }
            close(client_fd);
        } else if (req.cmd==CMD_STOP) {
            printf("[Supervisor] CMD_STOP '%s'\n",req.id);
            int found=0; pid_t kill_pid=-1;
            pthread_mutex_lock(&containers_lock);
            container_t *c=find_container(req.id);
            if (c) { found=1; kill_pid=c->pid; c->stop_requested=1; if (c->state==STATE_RUNNING||c->state==STATE_STARTING) c->state=STATE_STOPPED; }
            pthread_mutex_unlock(&containers_lock);
            if (found) { unregister_with_monitor(kill_pid,req.id); if (kill_pid>0) kill(kill_pid,SIGKILL); snprintf(resp.message,sizeof(resp.message),"OK: stop signal sent to '%s'\n",req.id); }
            else snprintf(resp.message,sizeof(resp.message),"ERR: container '%s' not found\n",req.id);
            xwrite(client_fd,&resp,sizeof(resp)); close(client_fd);
        } else {
            snprintf(resp.message,sizeof(resp.message),"ERR: unknown command\n");
            xwrite(client_fd,&resp,sizeof(resp)); close(client_fd);
        }
    }

    printf("[Supervisor] Shutting down\n");
    pthread_mutex_lock(&containers_lock);
    for (int i=0;i<container_count;i++) {
        container_t *c=&containers[i];
        if (c->state==STATE_RUNNING||c->state==STATE_STARTING) { c->stop_requested=1; kill(c->pid,SIGKILL); }
    }
    pthread_mutex_unlock(&containers_lock);
    usleep(300000);
    while (waitpid(-1,NULL,WNOHANG)>0) {}
    for (int i=0;i<container_count;i++) {
        container_t *c=&containers[i];
        log_buf_mark_done(&c->log_buf);
        pthread_join(c->producer_tid,NULL); pthread_join(c->consumer_tid,NULL);
        log_buf_destroy(&c->log_buf);
        if (c->run_client_fd>=0) { close(c->run_client_fd); c->run_client_fd=-1; }
    }
    close(server_fd); unlink(CONTROL_PATH);
    printf("[Supervisor] Clean exit\n"); return 0;
}

static int send_control_request(const control_request_t *req, control_response_t *resp) {
    int sock=socket(AF_UNIX,SOCK_STREAM,0); if (sock<0) { perror("socket"); return 1; }
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX; strncpy(addr.sun_path,CONTROL_PATH,sizeof(addr.sun_path)-1);
    if (connect(sock,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("connect — is supervisor running?"); close(sock); return 1; }
    xwrite(sock,req,sizeof(*req));
    memset(resp,0,sizeof(*resp));
    if (read(sock,resp,sizeof(*resp))>0) printf("%s",resp->message);
    if (req->cmd==CMD_LOGS) { char buf[512]; ssize_t n; while ((n=read(sock,buf,sizeof(buf)))>0) fwrite(buf,1,(size_t)n,stdout); }
    close(sock); return 0;
}

static int g_run_sock=-1;
static char g_run_id[64];
static void run_forward_stop(int sig) {
    (void)sig;
    control_request_t req; control_response_t resp; memset(&req,0,sizeof(req));
    req.cmd=CMD_STOP; snprintf(req.id,sizeof(req.id),"%s",g_run_id);
    send_control_request(&req,&resp);
}
static int cmd_run_blocking(const control_request_t *start_req) {
    int sock=socket(AF_UNIX,SOCK_STREAM,0); if (sock<0) { perror("socket"); return 1; }
    struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
    addr.sun_family=AF_UNIX; strncpy(addr.sun_path,CONTROL_PATH,sizeof(addr.sun_path)-1);
    if (connect(sock,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("connect"); close(sock); return 1; }
    g_run_sock=sock; snprintf(g_run_id,sizeof(g_run_id),"%s",start_req->id);
    signal(SIGINT,run_forward_stop); signal(SIGTERM,run_forward_stop);
    xwrite(sock,start_req,sizeof(*start_req));
    control_response_t resp; memset(&resp,0,sizeof(resp));
    ssize_t n=read(sock,&resp,sizeof(resp)); if (n>0) printf("%s",resp.message);
    close(sock); return resp.exit_code;
}

static void parse_optional_flags(int argc, char *argv[], int from,
                                  unsigned long *soft, unsigned long *hard, int *nv) {
    for (int i=from;i<argc-1;i++) {
        if (strcmp(argv[i],"--soft-mib")==0) *soft=(unsigned long)atol(argv[i+1])*1024UL*1024UL;
        else if (strcmp(argv[i],"--hard-mib")==0) *hard=(unsigned long)atol(argv[i+1])*1024UL*1024UL;
        else if (strcmp(argv[i],"--nice")==0) *nv=atoi(argv[i+1]);
    }
}

int main(int argc, char *argv[]) {
    if (argc<2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <rootfs>\n"
            "  %s start  <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run    <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n  %s logs <id>\n  %s stop <id>\n",
            argv[0],argv[0],argv[0],argv[0],argv[0],argv[0]);
        return 1;
    }
    if (strcmp(argv[1],"supervisor")==0) {
        if (argc<3) { fprintf(stderr,"Usage: %s supervisor <rootfs>\n",argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1],"ps")==0) {
        control_request_t req; control_response_t resp; memset(&req,0,sizeof(req)); req.cmd=CMD_PS;
        return send_control_request(&req,&resp);
    }
    if (strcmp(argv[1],"logs")==0) {
        if (argc<3) { fprintf(stderr,"Usage: %s logs <id>\n",argv[0]); return 1; }
        control_request_t req; control_response_t resp; memset(&req,0,sizeof(req));
        req.cmd=CMD_LOGS; snprintf(req.id,sizeof(req.id),"%s",argv[2]);
        return send_control_request(&req,&resp);
    }
    if (strcmp(argv[1],"stop")==0) {
        if (argc<3) { fprintf(stderr,"Usage: %s stop <id>\n",argv[0]); return 1; }
        control_request_t req; control_response_t resp; memset(&req,0,sizeof(req));
        req.cmd=CMD_STOP; snprintf(req.id,sizeof(req.id),"%s",argv[2]);
        return send_control_request(&req,&resp);
    }
    if (strcmp(argv[1],"start")==0||strcmp(argv[1],"run")==0) {
        if (argc<5) { fprintf(stderr,"Usage: %s %s <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n",argv[0],argv[1]); return 1; }
        control_request_t req; memset(&req,0,sizeof(req));
        req.cmd=(strcmp(argv[1],"start")==0)?CMD_START:CMD_RUN;
        snprintf(req.id,sizeof(req.id),"%s",argv[2]);
        snprintf(req.rootfs,sizeof(req.rootfs),"%s",argv[3]);
        snprintf(req.command,sizeof(req.command),"%s",argv[4]);
        req.soft_limit_bytes=0; req.hard_limit_bytes=0; req.nice_val=0;
        parse_optional_flags(argc,argv,5,&req.soft_limit_bytes,&req.hard_limit_bytes,&req.nice_val);
        if (req.cmd==CMD_RUN) return cmd_run_blocking(&req);
        control_response_t resp; return send_control_request(&req,&resp);
    }
    fprintf(stderr,"Unknown command: %s\n",argv[1]); return 1;
}
