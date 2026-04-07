#include <stdlib.h>
#include "module_api.h"

void module_release(int *p) {
    free(p);
}

void module_release_twice(int *p) {
    module_release(p);
    module_release(p);
}
