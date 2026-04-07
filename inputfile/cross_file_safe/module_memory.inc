#include <stdlib.h>
#include "module_api.h"

int *safe_module_alloc(int seed) {
    int *p = (int *)malloc(sizeof(int));
    if (!p) return NULL;
    *p = seed;
    return p;
}

void safe_module_release(int *p) {
    if (p) {
        free(p);
    }
}
