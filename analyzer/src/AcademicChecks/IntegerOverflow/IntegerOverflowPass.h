#ifndef INTEGER_OVERFLOW_PASS_H
#define INTEGER_OVERFLOW_PASS_H

#include "../Core/CheckCommon.h"

class IntegerOverflowPass : public CheckPassBase {
public:
    explicit IntegerOverflowPass(const std::set<std::string> &allocFuncs);
    std::string name() const override;
    void run(ModuleMap &modules, std::vector<BugRecord> &out) override;

private:
    std::set<std::string> AllocFuncs;
};

#endif
