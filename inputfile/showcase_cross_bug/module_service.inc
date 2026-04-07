#include <string.h>
#include "module_api.h"

void dangerous_copy(char *dst, const char *src) {
    // 包装函数内部固定拷贝长度（用于上下文敏感摘要传播）
    memcpy(dst, src, 32);
}
