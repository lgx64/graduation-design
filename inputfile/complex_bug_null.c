#include <stdlib.h>
#include <string.h>

void complex_bug_null(void) {
    char src[8] = "abc";

    // 直接空指针参与高危调用
    memcpy((char *)0, src, 4);
}
