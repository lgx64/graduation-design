#include <stdlib.h>
#include "module_api.h"

void controller_bug(int seed) {
    int *a = (int *)malloc(sizeof(int));
    if (!a) return;
    *a = seed;
    module_release(a);
    module_use(a); // UAF

    int *b = (int *)malloc(sizeof(int));
    if (!b) return;
    *b = seed + 1;
    module_release_twice(b); // Double-Free
}
