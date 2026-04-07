#include <stdlib.h>
#include <string.h>

void complex_safe_memory_1(void) {
    char dst[32] = {0};
    char src[16] = "safe";
    memcpy(dst, src, 5); // 不越界

    int *p = (int *)malloc(sizeof(int));
    if (!p) return;
    *p = 42;
    free(p);

    int *q = (int *)malloc(sizeof(int));
    if (!q) return;
    *q = 7;
    free(q);
}
