#ifndef CROSS_FILE_BUG_MODULE_API_H
#define CROSS_FILE_BUG_MODULE_API_H

void module_release(int *p);
void module_release_twice(int *p);
void module_use(int *p);
void controller_bug(int seed);

#endif
