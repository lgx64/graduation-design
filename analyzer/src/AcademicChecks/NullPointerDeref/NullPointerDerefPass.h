#ifndef NULL_POINTER_DEREF_PASS_H
#define NULL_POINTER_DEREF_PASS_H

#include "../Core/CheckCommon.h"

class NullPointerDerefPass : public CheckPassBase {
public:
    std::string name() const override;
    void run(ModuleMap &modules, std::vector<BugRecord> &out) override;
};

#endif
