#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_ARGS 16
#define MAX_ARG_LEN 512
#define MAX_NAME_LEN 64
#define MAX_RETVAL_LEN 128
#define MAX_RAW_LEN 1024

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
    char syscall[64];
    char args[16][512];
    int argc;
    char retval[128];
    int exit_code;
    char raw[1024];
} syscall_record_t;

static char *strip_prefix(char *line, int *pid, int *has_pid)
{
    *has_pid = 0;

    if (strncmp(line, "[pid ", 5) == 0) {
        char *end = strchr(line, ']');
        if (end) {
            *pid = atoi(line + 5);
            *has_pid = 1;
            end++;
            while (*end == ' ')
                end++;
            return end;
        }
    }
    return line;
}
static int split_args(const char *arg_str, char args[16][512])
{
    int argc = 0;
    int depth = 0;
    int in_quotes = 0;
    const char *start = arg_str;
    const char *p = arg_str;
    while (*p && argc < 16) {
        if (*p == '"' && (p == arg_str || *(p - 1) != '\\')) {
            in_quotes = !in_quotes;
        }
        else if (!in_quotes &&
                 (*p == '(' || *p == '{' || *p == '[')) {
            depth++;
        }
        else if (!in_quotes &&
                 (*p == ')' || *p == '}' || *p == ']')) {
            depth--;
        }
        else if (!in_quotes && depth == 0 && *p == ',') {
               int len = (int)(p - start);
            if (len >= 512) len = 511;
            while (*start == ' ') start++;
            len = (int)(p - start);
            if (len < 0) len = 0;
            if (len >= 512) len = 511;
            strncpy(args[argc], start, len);
            args[argc][len] = '\0';
            argc++;
            start = p + 1;
        }
        p++;
    }

    if (argc < 16 && *start != '\0') {

        while (*start == ' ')
            start++;

        int len = (int)(p - start);
        if (len < 0) len = 0;
        if (len >= 512) len = 511;
        strncpy(args[argc], start, len);
        args[argc][len] = '\0';

        int l = (int)strlen(args[argc]);

        while (l > 0 && args[argc][l - 1] == ' ') {
            args[argc][--l] = '\0';
        }

        if (l > 0) argc++;
    }
    return argc;
}

int parse_line(char *line, syscall_record_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    char *p = strip_prefix(line, &rec->pid, &rec->has_pid);
    size_t l = strlen(p);
    while (l > 0 &&
           (p[l - 1] == '\n' || p[l - 1] == '\r')) {
        p[--l] = '\0';
    }
    if (l == 0)
        return 0;
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

        int nlen = space
            ? (int)(space - name_start)
            : (int)strlen(name_start);

        if (nlen >= MAX_NAME_LEN)
            nlen = MAX_NAME_LEN - 1;

        strncpy(rec->syscall, name_start, nlen);
        rec->syscall[nlen] = '\0';

        return 1;
    }
    if (strncmp(p, "<...  ", 5) == 0) {
        rec->kind = LINE_RESUMED;
        char *name_start = p + 5;
        char *name_end = strstr(name_start, " resumed>");
        if (name_end) {

            int nlen = (int)(name_end - name_start);

            if (nlen >= 64)
                nlen = 63;

            strncpy(rec->syscall, name_start, nlen);
            rec->syscall[nlen] = '\0';

            char *eq = strrchr(p, '=');

            if (eq) {

                char *val = eq + 1;

                while (*val == ' ')
                    val++;

                strncpy(rec->retval,
                        val,
                        MAX_RETVAL_LEN - 1);
            }
        }

        return 1;
    }
    char *paren = strchr(p, '(');

    if (!paren) {

        rec->kind = LINE_UNKNOWN;

        strncpy(rec->syscall,
                p,
                MAX_NAME_LEN - 1);

        return 1;
    }

    int nlen = (int)(paren - p);

    if (nlen >= 64)
        nlen = 63;

    strncpy(rec->syscall, p, nlen);
    rec->syscall[nlen] = '\0';
    if (strstr(p, "<unfinished ...>")) {

        rec->kind = LINE_UNFINISHED;

        char *args_start = paren + 1;
        char *cut = strstr(args_start, "<unfinished");

        if (cut) {
            char buf[2048];
            int len = (int)(cut - args_start);
            if (len >= (int)sizeof(buf))
                len = sizeof(buf) - 1;
            strncpy(buf, args_start, len);
            buf[len] = '\0';
            rec->argc = split_args(buf, rec->args);
        }
        return 1;
    }
    rec->kind = LINE_SYSCALL;
    char *args_start = paren + 1;
    char *eq = strrchr(p, '=');
    if (eq) {
        char *close_paren = eq - 1;
        while (close_paren > args_start &&
               *close_paren != ')') {
            close_paren--;
        }
        if (*close_paren == ')') {
            int args_len = (int)(close_paren - args_start);
            if (args_len > 0) {
                char buf[2048];
                if (args_len >= (int)sizeof(buf))
                    args_len = sizeof(buf) - 1;

                strncpy(buf, args_start, args_len);
                buf[args_len] = '\0';

                rec->argc = split_args(buf, rec->args);
            }
        }
        char *val = eq + 1;
        while (*val == ' ')
            val++;
        strncpy(rec->retval,
                val,
                MAX_RETVAL_LEN - 1);
        rec->retval[MAX_RETVAL_LEN - 1] = '\0';
    }
    return 1;
}

static const char *kind_name(line_kind_t k) {
    switch (k) {
        case LINE_SYSCALL:    return "SYSCALL";
        case LINE_UNFINISHED: return "UNFINISHED";
        case LINE_RESUMED:    return "RESUMED";
        case LINE_SIGNAL:     return "SIGNAL";
        case LINE_EXIT:       return "EXIT";
        default:              return "UNKNOWN";
    }
}
 
int main(int argc, char *argv[]) {
    FILE *in = stdin;
    if (argc > 1) {
        in = fopen(argv[1], "r");
        if (!in) { perror("fopen"); return 1; }
    }
 
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    syscall_record_t rec;
 
    while ((nread = getline(&line, &len, in)) != -1) {
        if (!parse_line(line, &rec)) continue;
 
        printf("---\n");
        printf("kind:    %s\n", kind_name(rec.kind));
        if (rec.has_pid) printf("pid:     %d\n", rec.pid);
        if (rec.kind == LINE_EXIT) {
            printf("exit_code: %d\n", rec.exit_code);
            continue;
        }
        printf("syscall: %s\n", rec.syscall);
        for (int i = 0; i < rec.argc; i++) {
            printf("  arg[%d]: %s\n", i, rec.args[i]);
        }
        if (rec.retval[0]) printf("retval:  %s\n", rec.retval);
        if (rec.kind == LINE_SIGNAL) printf("raw:     %s\n", rec.raw);
    }
    free(line);
    if (in != stdin) fclose(in);
    return 0;
}
