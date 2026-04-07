#include "module_api.h"

void controller_safe(void) {
    char src[64] = {0};
    char dst[32] = {0};

    safe_copy(dst, src);

    char *p = safe_alloc_userbuf();
    if (!p) return;
    safe_release_once(p); // 仅释放一次，不再使用
}
