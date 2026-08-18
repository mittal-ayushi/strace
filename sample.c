#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    printf("TESTING HELLO TESTING TESTING\n");

    int fd = open("/etc/passwd", O_RDONLY);

    if (fd >= 0) {
        printf("Opened passwd\n");
        close(fd);
    }

    return 0;
}