// to test enforcement

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {

    int fd = open("/etc/shadow", O_RDONLY);

    if (fd >= 0) {
        printf("Opened shadow\n");
        close(fd);
    } else {
        perror("open");
    }

    printf("Program continued running\n");

    return 0;
}