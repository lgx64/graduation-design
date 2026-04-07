#include "cross_file_bug/module_memory.inc"
#include "cross_file_bug/module_use.inc"
#include "cross_file_bug/module_controller.inc"

void cross_file_bug_entry(void) {
    controller_bug(123);
}
