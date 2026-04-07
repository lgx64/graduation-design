#ifndef CROSS_FILE_SAFE_MODULE_API_H
#define CROSS_FILE_SAFE_MODULE_API_H

void safe_module_release(int *p);
int *safe_module_alloc(int seed);
void safe_controller(int seed);

#endif
