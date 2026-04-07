#include <stdlib.h>

void npd_path_bug(int flag) {
    int *p = (int *)malloc(sizeof(int));
    if (flag) {
        p = NULL;
    }

    // 在 flag==1 路径上发生空指针解引用
    *p = 7;

    if (p) free(p);
}
