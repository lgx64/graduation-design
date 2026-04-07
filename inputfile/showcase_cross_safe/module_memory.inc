#include <stdlib.h>
#include "module_api.h"

char *safe_alloc_userbuf(void) {
    return (char *)malloc(32);
}

void safe_release_once(char *p) {
    free(p);
}
