#include <stdio.h>
#include <unistd.h>
#include <signal.h>

int main() {
    printf("Test program started, PID: %d\n", getpid());
    sleep(10); 
    return 0;
}