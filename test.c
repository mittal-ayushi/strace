
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <errno.h>

int main(int argc,char *argv[]){
    int pipefd[2];
    //pipefd[0] will read and pipefd[1] will write

    //error handling
    if (argc<2) {
        fprintf(stderr, "The way to use it is: %s <target program> [arguments] \n", argv[0]);
        return 1;
    }
    if (pipe(pipefd) == -1) { //pipe() lets 2 programs talk to each other. here parent and child
        perror("Error");
        return 1;
    }

    //main code
    //pid_t pid is a variable that can store process ids. pid_t is a special datatype given to processor ids. 
    // FORK works bcz it gives 2 diff processor ids to parent n child. child gets ID of 0.
    pid_t pid = fork();
    
    if (pid == 0)  { //child
        close(pipefd[0]);       //child only writes so close readend
        dup2(pipefd[1], 2); //connect pipe to write end
        close(pipefd[1]);
           char *strace_argv[argc + 6];
        int i = 0;
        strace_argv[i++] = "strace";
        strace_argv[i++] = "-f";
        strace_argv[i++] = "-e";
        strace_argv[i++] = "trace=file,network,process";
        for (int j = 1; j < argc; j++) {
            strace_argv[i++] = argv[j];
        }
        strace_argv[i] = NULL;
 
        execvp("strace", strace_argv);
        perror("execvp strace");
        _exit(1);
 
    } else { //parent side
        close(pipefd[1]);      //parent only reads
 
        FILE *trace_stream = fdopen(pipefd[0], "r");
        if (!trace_stream) {
            perror("fdopen");
            return 1;
        }
 
        char *line = NULL;
        size_t len = 0;
        ssize_t nread;
 
        printf("[spy] watching PID %d (%s)...\n", pid, argv[1]);
 
        while ((nread = getline(&line, &len, trace_stream)) != -1) {
            if (nread > 0 && line[nread - 1] == '\n') {
                line[nread - 1] = '\0';
            }
            printf("[trace] %s\n", line);
        }
 
        free(line);
        fclose(trace_stream);
 
        int status;
        waitpid(pid, &status, 0);
    }


    return 0;
}