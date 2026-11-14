#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname u;
    uname(&u);
    printf("Hello from %s (%s)!\n", u.nodename, u.machine);
    return 0;
}
