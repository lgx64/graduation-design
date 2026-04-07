#include <stdio.h>
#include <stdlib.h>

static void release_once(int *p) {
    free(p);
}

static void release_twice_via_wrapper(int *p) {
    release_once(p);
    release_once(p);
}

static void consume_ptr(int *p) {
    // 对参数进行真实使用，便于跨函数摘要传播到调用点
    printf("value=%d\n", *p);
}

void complex_bug_chain_uaf_df(int n) {
    int *p = (int *)malloc(sizeof(int));
    if (!p) return;

    *p = n;
    release_once(p);          // 释放
    consume_ptr(p);           // UAF（跨函数调用）

    int *q = (int *)malloc(sizeof(int));
    if (!q) return;

    *q = n + 1;
    release_twice_via_wrapper(q); // Double-Free（经包装函数）
}
