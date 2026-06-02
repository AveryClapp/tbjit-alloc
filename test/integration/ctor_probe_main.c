/* Trivial host for the ctorprobe library. The interesting work happens in
 * libctorprobe's constructor (see ctorprobe.c); main just confirms the process
 * reached userspace at all. */
#include <unistd.h>

int main(void) {
    (void)write(1, "MAIN_OK\n", 8);
    return 0;
}
