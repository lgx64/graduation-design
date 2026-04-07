#ifndef DOUBLE_FREE_PASS_H
#define DOUBLE_FREE_PASS_H

#include "../Core/CheckCommon.h"

class DoubleFreePass : public CheckPassBase {
public:
    explicit DoubleFreePass(const std::set<std::string> &freeFuncs);
    std::string name() const override;
    void run(ModuleMap &modules, std::vector<BugRecord> &out) override;

private:
    std::set<std::string> FreeFuncs;
};

#endif
