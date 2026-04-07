#include "DoubleFreePass.h"

DoubleFreePass::DoubleFreePass(const std::set<std::string> &freeFuncs) : FreeFuncs(freeFuncs) {}

std::string DoubleFreePass::name() const {
    return "DoubleFree";
}

namespace {

struct FuncDFSummary {//跨函数摘要
    std::set<unsigned> freedArgs;
};

static void collectOneFunctionSummary(Function &F,//要收集摘要信息的函数对象
                                      const std::set<std::string> &freeFuncs,//表示直接释放指针的函数名称集合
                                      const std::map<Function *, FuncDFSummary> &known,//已知的函数摘要信息，用于处理函数调用时的摘要传播
                                      FuncDFSummary &out) {//输出参数，用于存储收集到的函数摘要信息
    PointerSlotUF uf = buildPointerSlotUF(F);
    auto argRootToIdx = buildPointerArgRootIndex(F, uf);

    for (Instruction &I : instructions(&F)) {//考虑两种情况：直接调用释放函数，直接调用的函数里释放了指针
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;

        std::string callee = getCalledFuncName(CI).str();
        if (freeFuncs.count(callee) && CI->arg_size() >= 1) {
            Value *r = finalPtrSlot(CI->getArgOperand(0), uf);
            auto it = argRootToIdx.find(r);//找到指针参数r在当前函数参数中对应的索引
            if (it != argRootToIdx.end()) out.freedArgs.insert(it->second);
        }

        if (Function *CF = resolveDirectCallee(CI)) {
            auto sit = known.find(CF);
            if (sit != known.end()) {
                for (unsigned ai : sit->second.freedArgs) {//调用的函数中释放了哪些指针参数，就将这些参数归并到当前函数的形参后填入freedArgs集合中
                    if (ai >= CI->arg_size()) continue;
                    Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
                    auto it = argRootToIdx.find(r);
                    if (it != argRootToIdx.end()) out.freedArgs.insert(it->second);
                }
            }
        }
    }
}

static std::map<Function *, FuncDFSummary>
buildInterprocSummary(ModuleMap &modules, const std::set<std::string> &freeFuncs) {
    std::map<Function *, FuncDFSummary> summary;
    for (auto &p : modules) {
        for (Function &F : *p.second) {
            if (!F.isDeclaration()) summary[&F] = FuncDFSummary();//给每个函数对象初始化一个空的函数摘要
        }
    }

    for (int iter = 0; iter < kInterprocMaxIters; ++iter) {
        bool changed = false;
        auto old = summary;

        for (auto &it : summary) {
            Function *F = it.first;
            FuncDFSummary now;
            collectOneFunctionSummary(*F, freeFuncs, old, now);
            if (now.freedArgs != it.second.freedArgs) {
                it.second = std::move(now);
                changed = true;
            }
        }

        if (!changed) break;
    }

    return summary;
}

static void handleStoreReset(Instruction &I,
                             PointerSlotUF &uf,
                             std::map<Value *, unsigned> &freeCount) {//处理store指令对指针参数的覆盖情况，如果store指令的目标地址对应的指针参数被覆盖了，那么就将该指针参数的释放计数重置为0，避免误报重复释放
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI) return;
    Value *slotRoot = finalPtrSlot(SI->getPointerOperand(), uf);
    if (slotRoot) freeCount.erase(slotRoot);
}

static void recordDoubleFreeBug(Function &F,//所在函数
                                CallInst *CI,//调用指令
                                unsigned argIdx,//指针参数索引，如果是直接调用释放函数则为0，如果是通过函数摘要传播的调用则为对应的参数索引
                                bool viaCallSummary,//是否通过函数摘要传播的调用
                                std::set<std::string> &dedup,//用于去重，避免同一位置报告重复的bug
                                std::vector<BugRecord> &out) {//输出参数，用于存储检测到的DoubleFree漏洞
    std::string tag = viaCallSummary ? "DF_CALL|" + std::to_string(argIdx) : "DF";
    std::string key = F.getName().str() + "|" + locToString(CI) + "|" + tag;
    if (!dedup.insert(key).second) return;//如果已经报告过同一位置的同一类型的bug，则跳过

    if (viaCallSummary) {
        addBug(out, "DoubleFree", &F, CI,
               "Pointer may be freed repeatedly via callee argument #" +
                   std::to_string(argIdx) + ". Inst=" + instToString(CI));
        return;
    }

    addBug(out, "DoubleFree", &F, CI,
           "Pointer freed more than once. Inst=" + instToString(CI));
}

static void handleDirectFree(Function &F,
                             CallInst *CI,
                             PointerSlotUF &uf,
                             std::map<Value *, unsigned> &freeCount,
                             std::set<std::string> &dedup,
                             std::vector<BugRecord> &out) {
    Value *arg0 = finalPtrSlot(CI->getArgOperand(0), uf);
    if (!arg0) return;
    unsigned count = ++freeCount[arg0];
    if (count <= 1) return;
    recordDoubleFreeBug(F, CI, 0, false, dedup, out);
}

static void handleSummaryFree(Function &F,
                              CallInst *CI,
                              PointerSlotUF &uf,
                              const std::map<Function *, FuncDFSummary> &summary,
                              std::map<Value *, unsigned> &freeCount,
                              std::set<std::string> &dedup,
                              std::vector<BugRecord> &out) {
    Function *CF = resolveDirectCallee(CI);
    if (!CF) return;
    auto sit = summary.find(CF);
    if (sit == summary.end()) return;

    for (unsigned ai : sit->second.freedArgs) {
        if (ai >= CI->arg_size()) continue;
        Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
        if (!r) continue;
        unsigned count = ++freeCount[r];
        if (count <= 1) continue;
        recordDoubleFreeBug(F, CI, ai, true, dedup, out);
    }
}

static void analyzeOneFunction(Function &F,
                               const std::set<std::string> &freeFuncs,
                               const std::map<Function *, FuncDFSummary> &summary,
                               std::set<std::string> &dedup,
                               std::vector<BugRecord> &out) {
    PointerSlotUF uf = buildPointerSlotUF(F);
    std::map<Value *, unsigned> freeCount;

    for (Instruction &I : instructions(&F)) {
        handleStoreReset(I, uf, freeCount);

        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;

        std::string callee = getCalledFuncName(CI).str();
        if (freeFuncs.count(callee) && CI->arg_size() >= 1) {
            handleDirectFree(F, CI, uf, freeCount, dedup, out);
            continue;
        }

        handleSummaryFree(F, CI, uf, summary, freeCount, dedup, out);
    }
}

} // namespace

void DoubleFreePass::run(ModuleMap &modules, std::vector<BugRecord> &out) {
    std::set<std::string> dedup;
    auto summary = buildInterprocSummary(modules, FreeFuncs);

    for (auto &p : modules) {
        Module *M = p.second;
        for (Function &F : *M) {
            if (F.isDeclaration()) continue;
            analyzeOneFunction(F, FreeFuncs, summary, dedup, out);
        }
    }
}
