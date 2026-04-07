#ifndef BUFFER_OVERFLOW_PASS_H
#define BUFFER_OVERFLOW_PASS_H

#include "../Core/CheckCommon.h"
//检测四类越界风险
//memcpy/memmove/memset长度超过目标对象剩余空间
//strcpy/strncpy拷贝超过目标对象剩余空间
//GEP + load/store 的索引访问越界
//“包装函数”跨函数传播导致的越界（上下文摘要）
class BufferOverflowPass : public CheckPassBase {
public:
    std::string name() const override;
    void run(ModuleMap &modules, std::vector<BugRecord> &out) override;
};

#endif
