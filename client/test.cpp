#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
    char* addr = (char*)malloc(sizeof(char) * 3);
    addr[0] = 'a';
    addr[1] = 'b';
    addr[2] = 'c';
    printf("%d\n", strlen(addr));
}
