#include <stdlib.h>
#include <stdint.h>

void complex_bug_integer(unsigned int a, unsigned int b) {
    // 乘法结果直接用于分配大小，模拟整数溢出风险
    size_t ux = (size_t)a;
    size_t uy = (size_t)b;
    char *p = (char *)malloc(ux * uy);
    if (!p) return;

    p[0] = 'x';
    free(p);
}
