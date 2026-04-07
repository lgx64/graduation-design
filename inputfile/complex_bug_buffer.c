#include <string.h>

void complex_bug_buffer(void) {
    char dst[24] = {0};
    char src[16] = {0};

    // 指向尾部偏移，仅剩 4 字节可写，但复制 8 字节
    char *tail = dst + 20;
    memcpy(tail, src, 8);

    // 典型字符串越界写
    char small[8] = {0};
    strcpy(small, "0123456789AB");
}
