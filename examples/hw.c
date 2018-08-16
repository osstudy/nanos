#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

void main(int argc, char **argv)
{
    printf("hello world!\n");
    printf("args: %d\n", argc); 
    for (int i = 0; i < argc; i++) printf ("   %s\n", argv[i]);
    printf("env: %s\n", getenv("USER"));
}

