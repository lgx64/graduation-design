#include <string.h>

void npd_call_bug(void) {
    char src[8] = "abc";
    // 明确空指针参数
    memcpy((char *)0, src, 4);
}
