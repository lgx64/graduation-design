#include <string.h>

static void copy8(char *dst, const char *src) {
    memcpy(dst, src, 8);
}

void bo_context_safe(void) {
    char ok[16] = {0};
    char src[32] = {0};
    copy8(ok, src);
}
