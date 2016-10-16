#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/types.h" 
#include "signal.h"
#include "mshell.h"
#include "mprofile.h"
#include "errdef.h"

int main(int argc, char **argv)
{
    char cmd[MAXCOMMAND], path[MAXPPATH];
    struct sigaction sa;
    int k=0;
    
    printf("\n-------------------------------\n");
    printf("Welcome to my shell!\n");
    printf("Author: Xincheng Yang, Yanqing Gong\n");
    printf("HOME:/usr/src\n");
    printf("You also can change profile path by command 'loadprofile'\n");
    printf("Eg: > loadprofile /etc/profile\n");
    printf("-------------------------------\n");
    
    sa.sa_flags = SA_NODEFER;
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,  &sa, NULL);	        // Handler for ctrl+c interrupt.
    read_profile();                         // Read profile from default profile
    
    while(1){
        getcwd(path, MAXPPATH);
        printf("\n%s> ", path);
        
        gets(cmd);
        precompile(cmd);  
        fflush(stdout);    
        k++;
        if(k>100){
            printf("Infinity\n");
            break;
        }
    }
}

