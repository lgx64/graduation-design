#include <string.h>

static void copy24(char *dst, const char *src) {
    // 上下文摘要应记录“向第0参数拷贝24字节”
    memcpy(dst, src, 24);
}

void bo_context_bug(void) {
    char small[16] = {0};
    char src[32] = {0};
    copy24(small, src);
}
