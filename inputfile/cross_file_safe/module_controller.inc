#include "module_api.h"

void safe_controller(int seed) {
    int *a = safe_module_alloc(seed);
    int *b = safe_module_alloc(seed + 1);

    // 正常生命周期管理：每个对象释放一次
    safe_module_release(a);
    safe_module_release(b);
}
