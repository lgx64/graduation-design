#ifndef UAF_PASS_H
#define UAF_PASS_H

#include "../Core/CheckCommon.h"

class UAFPass : public CheckPassBase {
public:
    explicit UAFPass(const std::set<std::string> &freeFuncs);
    std::string name() const override;
    void run(ModuleMap &modules, std::vector<BugRecord> &out) override;

private:
    std::set<std::string> FreeFuncs;
};

#endif
