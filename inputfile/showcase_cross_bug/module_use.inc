#include <stdio.h>
#include "module_api.h"

void sink_use(char *p) {
    // 跨函数真实使用
    printf("first=%d\n", (int)p[0]);
}
