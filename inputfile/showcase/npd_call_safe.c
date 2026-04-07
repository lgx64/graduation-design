#include <string.h>

void npd_call_safe(void) {
    char dst[8] = {0};
    char src[8] = "abc";
    memcpy(dst, src, 4);
}
