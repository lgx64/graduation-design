#ifndef SHOWCASE_CROSS_SAFE_API_H
#define SHOWCASE_CROSS_SAFE_API_H

char *safe_alloc_userbuf(void);
void safe_release_once(char *p);
void safe_copy(char dst[32], const char *src);
void controller_safe(void);

#endif
