#ifndef SHOWCASE_CROSS_BUG_API_H
#define SHOWCASE_CROSS_BUG_API_H

char *alloc_userbuf(void);
void release_once(char *p);
void release_twice(char *p);
void sink_use(char *p);
void dangerous_copy(char *dst, const char *src);
void controller_bug(void);

#endif
