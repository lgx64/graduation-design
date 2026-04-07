#include <stdlib.h>
#include "module_api.h"

char *alloc_userbuf(void) {
    return (char *)malloc(16);
}

void release_once(char *p) {
    free(p);
}

void release_twice(char *p) {
    release_once(p);
    release_once(p);
}
