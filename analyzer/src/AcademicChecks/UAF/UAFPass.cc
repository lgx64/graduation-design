#include "UAFPass.h"
//先提取函数间的信息（跨函数摘要），再用这些信息在每个函数内进行更精细的检查，检测Use-After-Free漏洞
UAFPass::UAFPass(const std::set<std::string> &freeFuncs) : FreeFuncs(freeFuncs) {}

std::string UAFPass::name() const {
    return "UseAfterFree";
}

namespace {//namespace用于限定函数作用域，避免与其他文件中的同名函数冲突

struct FuncUAFSummary {//函数摘要
    std::set<unsigned> freedArgs;//freedArgs表示函数中被释放的指针参数的索引集合
    std::set<unsigned> usedArgs;//usedArgs表示函数中被使用的指针参数的索引集合
};

static void collectOneFunctionSummary(Function &F,
                                      const std::set<std::string> &freeFuncs,
                                      const std::map<Function *, FuncUAFSummary> &known,
                                      FuncUAFSummary &out) {//收集单个函数的摘要信息
    PointerSlotUF uf = buildPointerSlotUF(F);
    auto argRootToIdx = buildPointerArgRootIndex(F, uf);//argRootToIdx表示当前函数参数中指针类型的Value对象与参数索引之间的映射关系，用于分析函数参数的指针根源

    for (Instruction &I : instructions(&F)) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
            std::string callee = getCalledFuncName(CI).str();//获取调用指令CI中被调用函数的名称
            if (freeFuncs.count(callee) && CI->arg_size() >= 1) {//如果被调用函数在freeFuncs中且至少有一个参数，则将第一个参数对应的指针参数索引加入freedArgs集合中
                Value *r = finalPtrSlot(CI->getArgOperand(0), uf);
                auto it = argRootToIdx.find(r);
                if (it != argRootToIdx.end()) out.freedArgs.insert(it->second);
            }

            if (!freeFuncs.count(callee)) {
                for (unsigned ai = 0; ai < CI->arg_size(); ++ai) {
                    Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
                    auto it = argRootToIdx.find(r);
                    if (it != argRootToIdx.end()) out.usedArgs.insert(it->second);
                }
            }

            if (Function *CF = resolveDirectCallee(CI)) {//解析被直接调用的函数，如果是间接调用则返回nullptr
                auto sit = known.find(CF);
                if (sit != known.end()) {
                    for (unsigned ai : sit->second.freedArgs) {//调用的函数中释放了哪些指针参数，就将这些参数归并到当前函数的形参后填入freedArgs集合中
                        if (ai >= CI->arg_size()) continue;//如果被调用函数摘要中的freedArgs集合中的参数索引ai超出当前调用指令的参数数量，则跳过
                        Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
                        auto it = argRootToIdx.find(r);
                        if (it != argRootToIdx.end()) out.freedArgs.insert(it->second);
                    }
                    for (unsigned ai : sit->second.usedArgs) {//同理
                        if (ai >= CI->arg_size()) continue;
                        Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
                        auto it = argRootToIdx.find(r);
                        if (it != argRootToIdx.end()) out.usedArgs.insert(it->second);
                    }
                }
            }
        }

        if (auto *LI = dyn_cast<LoadInst>(&I)) {
            // 仅将“读取非指针值”视为对指针参数的真实解引用使用。
            if (!LI->getType()->isPointerTy()) {
                Value *r = finalPtrSlot(LI->getPointerOperand(), uf);
                auto it = argRootToIdx.find(r);
                if (it != argRootToIdx.end()) out.usedArgs.insert(it->second);
            }
            continue;
        }

        if (auto *SI = dyn_cast<StoreInst>(&I)) {
            // 仅将“写入非指针值”视为对指针参数的真实解引用使用。
            if (!SI->getValueOperand()->getType()->isPointerTy()) {
                Value *r = finalPtrSlot(SI->getPointerOperand(), uf);
                auto it = argRootToIdx.find(r);
                if (it != argRootToIdx.end()) out.usedArgs.insert(it->second);
            }
            continue;
        }
    }
}

static std::map<Function *, FuncUAFSummary>
buildInterprocSummary(ModuleMap &modules, const std::set<std::string> &freeFuncs) {//迭代求解函数间的摘要信息，直到收敛或达到最大迭代次数
    std::map<Function *, FuncUAFSummary> summary;
    for (auto &p : modules) {
        for (Function &F : *p.second) {
            if (!F.isDeclaration()) summary[&F] = FuncUAFSummary();
        }
    }

    for (int iter = 0; iter < kInterprocMaxIters; ++iter) {
        bool changed = false;
        auto old = summary;

        for (auto &it : summary) {
            Function *F = it.first;
            FuncUAFSummary now;
            collectOneFunctionSummary(*F, freeFuncs, old, now);
            if (now.freedArgs != it.second.freedArgs || now.usedArgs != it.second.usedArgs) {
                it.second = std::move(now);
                changed = true;
            }
        }

        if (!changed) break;
    }

    return summary;
}

static void handleStoreInvalidate(Instruction &I,
                                  PointerSlotUF &uf,
                                  std::set<Value *> &freed) {
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI) return;
    Value *slotRoot = finalPtrSlot(SI->getPointerOperand(), uf);
    if (slotRoot) freed.erase(slotRoot);
}

static void handleCallEffects(Function &F,
                              Instruction &I,
                              PointerSlotUF &uf,
                              const std::set<std::string> &freeFuncs,
                              const std::map<Function *, FuncUAFSummary> &summary,
                              std::set<Value *> &freed,
                              std::set<Value *> &freedByThisInst,
                              std::set<std::string> &dedup,
                              std::vector<BugRecord> &out) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI) return;

    std::string callee = getCalledFuncName(CI).str();
    if (freeFuncs.count(callee) && CI->arg_size() >= 1) {
        Value *arg0 = finalPtrSlot(CI->getArgOperand(0), uf);
        if (arg0) {
            freed.insert(arg0);
            freedByThisInst.insert(arg0);
        }
        return;
    }

    Function *CF = resolveDirectCallee(CI);
    if (!CF) return;
    auto sit = summary.find(CF);
    if (sit == summary.end()) return;

    for (unsigned ai : sit->second.freedArgs) {
        if (ai >= CI->arg_size()) continue;
        Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
        if (r) {
            freed.insert(r);
            freedByThisInst.insert(r);
        }
    }

    for (unsigned ai : sit->second.usedArgs) {
        if (ai >= CI->arg_size()) continue;
        Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
        if (!r || !freed.count(r)) continue;//如果被调用函数摘要中的usedArgs集合中的参数索引ai超出当前调用指令的参数数量，或者该参数对应的指针参数r不在freed集合中，则跳过

        std::string key = F.getName().str() + "|" + locToString(CI) + "|UAF_CALL|" + std::to_string(ai);
        if (!dedup.insert(key).second) continue;

        addBug(out, "UseAfterFree", &F, CI,
               "Freed pointer passed to callee that may use argument #" +
                   std::to_string(ai) + ". Inst=" + instToString(CI));
    }
}

static void detectOperandUAF(Function &F,
                             Instruction &I,
                             PointerSlotUF &uf,
                             const std::set<std::string> &freeFuncs,
                             const std::set<Value *> &freed,
                             const std::set<Value *> &freedByThisInst,
                             std::set<std::string> &dedup,
                             std::vector<BugRecord> &out) {
    for (Use &U : I.operands()) {
        Value *op = finalPtrSlot(U.get(), uf);
        if (!op || !freed.count(op)) continue;
        if (freedByThisInst.count(op)) continue;

        if (auto *CI = dyn_cast<CallInst>(&I)) {
            std::string callee = getCalledFuncName(CI).str();
            if (freeFuncs.count(callee)) continue;
        }

        std::string key = F.getName().str() + "|" + locToString(&I) + "|UAF";
        if (!dedup.insert(key).second) continue;

        addBug(out, "UseAfterFree", &F, &I,
               "Use of previously freed pointer. Inst=" + instToString(&I));
    }
}

static void analyzeOneFunction(Function &F,
                               const std::set<std::string> &freeFuncs,
                               const std::map<Function *, FuncUAFSummary> &summary,
                               std::set<std::string> &dedup,
                               std::vector<BugRecord> &out) {
    PointerSlotUF uf = buildPointerSlotUF(F);
    std::set<Value *> freed;//已释放的指针集合

    for (Instruction &I : instructions(&F)) {
        std::set<Value *> freedByThisInst;
        handleStoreInvalidate(I, uf, freed);
        handleCallEffects(F, I, uf, freeFuncs, summary, freed, freedByThisInst, dedup, out);
        detectOperandUAF(F, I, uf, freeFuncs, freed, freedByThisInst, dedup, out);
    }
}

} // namespace

void UAFPass::run(ModuleMap &modules, std::vector<BugRecord> &out) {//总入口函数
    std::set<std::string> dedup;//用于去重，避免同一位置报告重复的bug
    auto summary = buildInterprocSummary(modules, FreeFuncs);//跨函数摘要的构建

    for (auto &p : modules) {//遍历各文件对应的模块
        Module *M = p.second;
        for (Function &F : *M) {//按函数遍历模块中的函数
            if (F.isDeclaration()) continue; 
            analyzeOneFunction(F, FreeFuncs, summary, dedup, out);
        }
    }
}
