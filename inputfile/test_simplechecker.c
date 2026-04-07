#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void null_deref_case(void) {
    // NullPointerDeref: 直接对空地址写入（便于静态规则命中）
    *(int *)0 = 1;
}

// 这个样例专门演示“槽位（slot）”的重要性：
// p 先被存到一个内存槽位里（slot = &p），后续每次通过 *slot 读取。
// 在 LLVM IR 里会变成多次 load 同一个槽位地址。
// 如果检测器不把这些 load 统一到同一个 slot，上下文就可能匹配失败。
static void slot_based_case(void) {
    int *p = (int *)malloc(sizeof(int));
    if (!p) return;

    *p = 42;
    int **slot = &p;

    // 第一次通过 slot 读取并释放
    free(*slot);

    // 第二次通过同一个 slot 再读（应识别为 Use-After-Free）
    printf("slot-uaf=%d\n", **slot);

    // 第三次通过同一个 slot 再释放（应识别为 Double-Free）
    free(*slot);
}

static void uaf_case(void) {
    int *p = (int *)malloc(sizeof(int));
    if (!p) return;
    *p = 7;
    free(p);
    // UAF: free 后继续使用
    printf("uaf=%d\n", *p);
}

static void double_free_case(void) {
    int *q = (int *)malloc(sizeof(int));
    if (!q) return;
    free(q);
    // Double free
    free(q);
}

static void integer_overflow_case(int x, int y) {
    // IntegerOverflow: 使用无符号乘法，结果作为 malloc 大小
    // 该形式通常不会携带 nsw/nuw 标记，更容易触发规则
    size_t ux = (size_t)(unsigned int)x;
    size_t uy = (size_t)(unsigned int)y;
    char *p = (char *)malloc(ux * uy);
    if (p) free(p);
}

static void buffer_overflow_case(void) {
    char dst[8];
    char src[32] = {0};
    // BufferOverflow: 拷贝长度大于目标缓冲区大小
    memcpy(dst, src, 16);
    (void)dst[0];
}

int main(void) {
    null_deref_case();
    slot_based_case();
    uaf_case();
    double_free_case();
    integer_overflow_case(100000, 100000);
    buffer_overflow_case();
    return 0;
}
