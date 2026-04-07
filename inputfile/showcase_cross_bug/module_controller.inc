#include "module_api.h"

void controller_bug(void) {
    char src[64] = {0};

    char *p = alloc_userbuf();
    if (!p) return;
    dangerous_copy(p, src); // BufferOverflow（上下文敏感）
    release_once(p);
    sink_use(p);            // UAF（跨函数使用）

    char *q = alloc_userbuf();
    if (!q) return;
    release_twice(q);       // Double-Free（包装函数）
}
