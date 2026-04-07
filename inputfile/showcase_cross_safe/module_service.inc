#include <string.h>
#include "module_api.h"

void safe_copy(char dst[32], const char *src) {
    memcpy(dst, src, 16); // <= 32, 安全
}
