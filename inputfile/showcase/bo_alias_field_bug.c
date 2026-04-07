#include <string.h>

typedef struct {
    char tag[8];
    char payload[16];
} Packet;

static void fill20(char *dst, const char *src) {
    memcpy(dst, src, 20);
}

void bo_alias_field_bug(void) {
    Packet pkt;
    char src[64] = {0};

    // 通过字段+别名链路传递目标指针
    char *p = pkt.payload;
    char **alias = &p;
    fill20(*alias, src);
}
