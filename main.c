// to run 
//gcc -o main main.c -Wall -Wextra
//gcc -o sample sample.c
// ./main ./sample.c (any of the samples)
// -k for enforcement
// -p <pid> to attach to a running process

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
 
#define READ_END  0
#define WRITE_END 1
#define MAX_ARGS 16
#define MAX_ARG_LEN 512
#define MAX_NAME_LEN 64
#define MAX_RETVAL_LEN 128
#define MAX_RAW_LEN 1024
#define MAX_RULES 32
#define MAX_SUMMARY 256

// parser
typedef enum {
    LINE_SYSCALL,
    LINE_UNFINISHED,
    LINE_RESUMED,
    LINE_SIGNAL,
    LINE_EXIT,
    LINE_UNKNOWN
} line_kind_t;
 
typedef struct {
    line_kind_t kind;
    int has_pid;
    int pid;
    char syscall[MAX_NAME_LEN];
    char args[MAX_ARGS][MAX_ARG_LEN];
    int argc;
    char retval[MAX_RETVAL_LEN];
    int exit_code;
    char raw[MAX_RAW_LEN];
} syscall_record_t;
 
static char *strip_pid_prefix(char *line, int *pid, int *has_pid) {
    *has_pid = 0;
    if (strncmp(line, "[pid ", 5) == 0) {
        char *end = strchr(line, ']');
        if (end) {
            *pid = atoi(line + 5);
            *has_pid = 1;
            end++;
            while (*end == ' ') end++;
            return end;
        }
    }
    return line;
}
 
static int split_args(const char *arg_str, char args[MAX_ARGS][MAX_ARG_LEN]) {
    int argc = 0;
    int depth = 0;
    int in_quotes = 0;
    const char *start = arg_str;
    const char *p = arg_str;
 
    while (*p && argc < MAX_ARGS) {
        if (*p == '"' && (p == arg_str || *(p - 1) != '\\')) {
            in_quotes = !in_quotes;
        } else if (!in_quotes && (*p == '(' || *p == '{' || *p == '[')) {
            depth++;
        } else if (!in_quotes && (*p == ')' || *p == '}' || *p == ']')) {
            depth--;
        } else if (!in_quotes && depth == 0 && *p == ',') {
            while (*start == ' ') start++;
            int len = (int)(p - start);
            if (len < 0) len = 0;
            if (len >= MAX_ARG_LEN) len = MAX_ARG_LEN - 1;
            strncpy(args[argc], start, len);
            args[argc][len] = '\0';
            argc++;
            start = p + 1;
        }
        p++;
    }
    if (argc < MAX_ARGS && *start != '\0') {
        while (*start == ' ') start++;
        int len = (int)(p - start);
        if (len < 0) len = 0;
        if (len >= MAX_ARG_LEN) len = MAX_ARG_LEN - 1;
        strncpy(args[argc], start, len);
        args[argc][len] = '\0';
        int l = (int)strlen(args[argc]);
        while (l > 0 && args[argc][l - 1] == ' ') { args[argc][--l] = '\0'; }
        if (l > 0) argc++;
    }
    return argc;
}
 
static int parse_line(char *line, syscall_record_t *rec) {
    memset(rec, 0, sizeof(*rec));
    char *p = strip_pid_prefix(line, &rec->pid, &rec->has_pid);
 
    size_t l = strlen(p);
    while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r')) p[--l] = '\0';
    if (l == 0) return 0;
 
    strncpy(rec->raw, p, MAX_RAW_LEN - 1);
 
    if (strncmp(p, "+++ exited with ", 16) == 0) {
        rec->kind = LINE_EXIT;
        rec->exit_code = atoi(p + 16);
        return 1;
    }
 
    if (strncmp(p, "--- ", 4) == 0) {
        rec->kind = LINE_SIGNAL;
        char *name_start = p + 4;
        char *space = strchr(name_start, ' ');
        int nlen = space ? (int)(space - name_start) : (int)strlen(name_start);
        if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
        strncpy(rec->syscall, name_start, nlen);
        rec->syscall[nlen] = '\0';
        return 1;
    }
    if (strncmp(p, "<... ", 5) == 0) {
        rec->kind = LINE_RESUMED;
        char *name_start = p + 5;
        char *name_end = strstr(name_start, " resumed>");
        if (name_end) {
            int nlen = (int)(name_end - name_start);
            if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
            strncpy(rec->syscall, name_start, nlen);
            rec->syscall[nlen] = '\0';
            char *eq = strrchr(p, '=');
            if (eq) {
                char *val = eq + 1;
                while (*val == ' ') val++;
                strncpy(rec->retval, val, MAX_RETVAL_LEN - 1);
            }
        }
        return 1;
    }
    char *paren = strchr(p, '(');
    if (!paren) {
        rec->kind = LINE_UNKNOWN;
        strncpy(rec->syscall, p, MAX_NAME_LEN - 1);
        return 1;
    }
    int nlen = (int)(paren - p);
    if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
    strncpy(rec->syscall, p, nlen);
    rec->syscall[nlen] = '\0';
 
    if (strstr(p, "<unfinished ...>")) {
        rec->kind = LINE_UNFINISHED;
        char *args_start = paren + 1;
        char *cut = strstr(args_start, "<unfinished");
        if (cut) {
            char buf[2048];
            int len = (int)(cut - args_start);
            if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
            strncpy(buf, args_start, len);
            buf[len] = '\0';
            rec->argc = split_args(buf, rec->args);
        }
        return 1;
    }
    char *scan = paren + 1;
    int depth = 1;
    int in_quotes = 0;
    char *close_paren = NULL;
    while (*scan) {
        if (*scan == '"' && *(scan - 1) != '\\') {
            in_quotes = !in_quotes;
        } else if (!in_quotes) {
            if (*scan == '(' || *scan == '{' || *scan == '[') depth++;
            else if (*scan == ')' || *scan == '}' || *scan == ']') {
                depth--;
                if (depth == 0 && *scan == ')') { close_paren = scan; break; }
            }
        }
        scan++;
    }
    if (!close_paren) {
        rec->kind = LINE_UNKNOWN;
        return 1;
    }
    int args_len = (int)(close_paren - (paren + 1));
    if (args_len < 0) args_len = 0;
    char args_buf[2048];
    if (args_len >= (int)sizeof(args_buf)) args_len = sizeof(args_buf) - 1;
    strncpy(args_buf, paren + 1, args_len);
    args_buf[args_len] = '\0';
    rec->argc = split_args(args_buf, rec->args);
 
    char *after = close_paren + 1;
    while (*after == ' ') after++;
    if (*after == '=') {
        after++;
        while (*after == ' ') after++;
        strncpy(rec->retval, after, MAX_RETVAL_LEN - 1);
    }
    rec->kind = LINE_SYSCALL;
    return 1;
}
//---
typedef struct {
    const char *name; 
    const char *syscall;      
    const char *arg_substr;
    int enforce;               
} rule_t;

static rule_t rules[] = {
    { "sensitive-file-read",  "openat",  "/etc/shadow",   1 },
    { "sensitive-file-read",  "openat",  "/etc/passwd",   0 },
    { "sensitive-file-read",  "open",    "/etc/shadow",   1 },
    { "ssh-key-access",       "openat",  ".ssh/id_",      1 },
    { "ssh-key-access",       "open",    ".ssh/id_",      1 },
    { "shell-spawn",          "execve",  "/bin/sh",       0 },
    { "shell-spawn",          "execve",  "/bin/bash",     0 },
    { "temp-file-delete",     "unlink",  "/tmp",          0 },
    { "dns-lookup",           "connect", "sin_port=htons(53)",  0 },
    { "network-connect",      "connect", "AF_INET",             0 },
    { "outbound-connect-any", "connect", NULL,                   0 },
};
#define NUM_RULES (int)(sizeof(rules) / sizeof(rules[0]))

//summary counter
typedef struct {
    char name[MAX_NAME_LEN];
    int count;
} summary_entry_t;
 
static summary_entry_t summary[MAX_SUMMARY];
static int summary_count = 0;
static int alert_count = 0;
 
static void record_summary(const char *syscall) {
    for (int i = 0; i < summary_count; i++) {
        if (strcmp(summary[i].name, syscall) == 0) {
            summary[i].count++;
            return;
        }
    }
    if (summary_count < MAX_SUMMARY) {
        strncpy(summary[summary_count].name, syscall, MAX_NAME_LEN - 1);
        summary[summary_count].count = 1;
        summary_count++;
    }
}
static rule_t *check_rules(const syscall_record_t *rec) {
    for (int i = 0; i < NUM_RULES; i++) {
        if (strcmp(rec->syscall, rules[i].syscall) != 0) continue;
        if (rules[i].arg_substr == NULL) return &rules[i];
 
        for (int a = 0; a < rec->argc; a++) {
            if (strstr(rec->args[a], rules[i].arg_substr)) {
                return &rules[i];
            }
        }
    }
    return NULL;
}

// MAIN

int main(int argc, char *argv[]) {
    int enforce_mode = 0;
    int attach_mode = 0;
    pid_t attach_pid = 0;
    int arg_offset = 1;
 
    while (arg_offset < argc) {
        if (strcmp(argv[arg_offset], "-k") == 0) {
            enforce_mode = 1;
            arg_offset++;
        } else if (strcmp(argv[arg_offset], "-p") == 0) {
            if (arg_offset + 1 >= argc) {
                fprintf(stderr, "-p requires a PID argument\n");
                return 1;
            }
            attach_mode = 1;
            attach_pid = (pid_t)atoi(argv[arg_offset + 1]);
            if (attach_pid <= 0) {
                fprintf(stderr, "invalid PID: %s\n", argv[arg_offset + 1]);
                return 1;
            }
            arg_offset += 2;
        } else {
            break;
        }
    }
 
    if (!attach_mode && argc - arg_offset < 1) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  %s [-k] <target_program> [args...]   (launch mode)\n", argv[0]);
        fprintf(stderr, "  %s [-k] -p <pid>                     (attach mode)\n", argv[0]);
        fprintf(stderr, "  -k   enforce mode: kill target on first matching alert\n");
        return 1;
    }
 
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }
 
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
 
    if (pid == 0) {
        close(pipefd[READ_END]);
        int real_stderr = dup(STDERR_FILENO);
        if (dup2(pipefd[WRITE_END], STDERR_FILENO) == -1) {
            perror("dup2");
            _exit(1);
        }
        close(pipefd[WRITE_END]);

        if (attach_mode) {
            char pid_str[16];
            snprintf(pid_str, sizeof(pid_str), "%d", attach_pid);
            execlp("strace", "strace", "-p", pid_str, "-e", "trace=network", NULL);
            dprintf(real_stderr, "execlp strace (attach) failed: %s\n", strerror(errno));
            _exit(1);
        } else {
            setpgid(0, 0);
 
            int target_argc = argc - arg_offset;
            char *strace_argv[target_argc + 6];
            int i = 0;
            strace_argv[i++] = "strace";
            strace_argv[i++] = "-f";
            strace_argv[i++] = "-e";
            strace_argv[i++] = "trace=file,network,process";
            for (int j = arg_offset; j < argc; j++) {
                strace_argv[i++] = argv[j];
            }
            strace_argv[i] = NULL;
 
            execvp("strace", strace_argv);
            dprintf(real_stderr, "execvp strace failed: %s\n", strerror(errno));
            _exit(1);
        }
 
    } else {
        close(pipefd[WRITE_END]);
        FILE *trace_stream = fdopen(pipefd[READ_END], "r");
        if (!trace_stream) {
            perror("fdopen");
            return 1;
        }
 
        if (attach_mode) {
            printf("attached to PID %d — enforce mode: %s\n",
                   attach_pid, enforce_mode ? "ON (-k)" : "off (log-only)");
        } else {
            printf("watching PID %d (%s) — enforce mode: %s\n",
                   pid, argv[arg_offset], enforce_mode ? "ON (-k)" : "off (log-only)");
        }
 
        char *line = NULL;
        size_t len = 0;
        ssize_t nread;
        syscall_record_t rec;
        int killed = 0;
 
        while ((nread = getline(&line, &len, trace_stream)) != -1) {
            if (!parse_line(line, &rec)) continue;
 
            if (rec.kind == LINE_SYSCALL) {
                record_summary(rec.syscall);
 
                rule_t *hit = check_rules(&rec);
                if (hit) {
                    alert_count++;
                    printf("[ALERT] rule=\"%s\" syscall=%s(", hit->name, rec.syscall);
                    for (int a = 0; a < rec.argc; a++) {
                        printf("%s%s", a > 0 ? ", " : "", rec.args[a]);
                    }
                    printf(") retval=%s\n", rec.retval);
 
                    if (enforce_mode && hit->enforce && !killed) {
                        if (attach_mode) {
                            printf("enforcing: killing PID %d "
                                   "(triggered by rule \"%s\")\n", attach_pid, hit->name);
                            kill(attach_pid, SIGKILL);
                        } else {
                            printf("enforcing: killing process group %d "
                                   "(triggered by rule \"%s\")\n", pid, hit->name);
                            killpg(pid, SIGKILL);
                        }
                        killed = 1;
                    }
                }
            }
        }
 
        free(line);
        fclose(trace_stream);
 
        int status;
        waitpid(pid, &status, 0); 
 
        printf("\n FINAL REPORT\n");
        if (WIFEXITED(status)) {
            printf("target exited with code %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("target killed by signal %d%s\n", WTERMSIG(status),
                   killed ? " (spy enforcement)" : "");
        }
        printf("total alerts: %d\n", alert_count);
        printf("syscall counts:\n");
        for (int i = 0; i < summary_count; i++) {
            printf("  %-20s %d\n", summary[i].name, summary[i].count);
        }
    }
    return 0;
}