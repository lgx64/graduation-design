#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool checked_mul_size(size_t a, size_t b, size_t *out) {
    return !__builtin_mul_overflow(a, b, out);
}

void complex_safe_memory_2(size_t n, size_t m) {
    size_t len = 0;
    if (!checked_mul_size(n, m, &len)) {
        return;
    }

    char *buf = (char *)malloc(len == 0 ? 1 : len);
    if (!buf) return;

    memset(buf, 0, len == 0 ? 1 : len);
    free(buf);
}
