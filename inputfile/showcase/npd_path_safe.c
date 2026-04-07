#include <stdlib.h>

void npd_path_safe(int flag) {
    int *p = (int *)malloc(sizeof(int));
    if (flag) {
        p = NULL;
    }

    // 路径保护：空值直接返回
    if (p == NULL) return;
    *p = 42;
    free(p);
}
