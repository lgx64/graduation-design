#include <stdio.h>
#include "module_api.h"

void module_use(int *p) {
    // 真实解引用，便于 UAF 在跨函数链中被识别
    printf("bug-chain=%d\n", *p);
}
