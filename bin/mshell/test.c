#include "stdio.h"
#include "stdlib.h"
#include "unistd.h"
#include "string.h"

int main(int argc, char **argv)
{
    printf("Executing...\n");
    char *argv2[] = {"ls","-al", "/etc", NULL};
    execvp("ls", argv2);
}