#include "DoubleFreePass.h"

DoubleFreePass::DoubleFreePass(const std::set<std::string> &freeFuncs) : FreeFuncs(freeFuncs) {}

std::string DoubleFreePass::name() const {
    return "DoubleFree";
}

namespace {

enum class FreeState {
    NotFreed = 0,
    MaybeFreed = 1,
    Freed = 2,
};

using FreeEnv = std::map<Value *, FreeState>;

static FreeState joinFreeState(FreeState a, FreeState b) {
    if (a == b) return a;
    if (a == FreeState::NotFreed && b == FreeState::NotFreed) return FreeState::NotFreed;
    if (a == FreeState::Freed && b == FreeState::Freed) return FreeState::Freed;
    return FreeState::MaybeFreed;
}

static FreeState envGet(const FreeEnv &env, Value *r) {
    auto it = env.find(r);
    if (it == env.end()) return FreeState::NotFreed;
    return it->second;
}

static void envSet(FreeEnv &env, Value *r, FreeState s) {
    if (!r) return;
    env[r] = s;
}

static bool mergeEnv(FreeEnv &dst, const FreeEnv &src) {
    bool changed = false;
    for (const auto &kv : src) {
        Value *k = kv.first;
        FreeState oldS = envGet(dst, k);
        FreeState newS = joinFreeState(oldS, kv.second);
        if (newS != oldS) {
            dst[k] = newS;
            changed = true;
        }
    }
    return changed;
}

struct FuncDFSummary {//跨函数摘要
    std::set<unsigned> freedArgs;
    std::set<unsigned> mustFreedArgs;
};

static bool hasConditionalBranch(Function &F) {
    for (BasicBlock &BB : F) {
        if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
            if (BI->isConditional()) return true;
        }
        if (isa<SwitchInst>(BB.getTerminator())) return true;
    }
    return false;
}

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

    if (!hasConditionalBranch(F)) {
        out.mustFreedArgs = out.freedArgs;
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
            if (now.freedArgs != it.second.freedArgs ||
                now.mustFreedArgs != it.second.mustFreedArgs) {
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
                             FreeEnv &env) {//处理store指令对指针参数的覆盖情况，如果store指令的目标地址对应的指针参数被覆盖了，那么就将该指针参数释放状态重置，避免误报重复释放
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI) return;
    Value *slotRoot = finalPtrSlot(SI->getPointerOperand(), uf);
    if (slotRoot) envSet(env, slotRoot, FreeState::NotFreed);
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
                             FreeEnv &env,
                             std::set<std::string> &dedup,
                             std::vector<BugRecord> &out) {
    Value *arg0 = finalPtrSlot(CI->getArgOperand(0), uf);
    if (!arg0) return;
    if (envGet(env, arg0) == FreeState::Freed) {
        recordDoubleFreeBug(F, CI, 0, false, dedup, out);
    }
    envSet(env, arg0, FreeState::Freed);
}

static void handleSummaryFree(Function &F,
                              CallInst *CI,
                              PointerSlotUF &uf,
                              const std::map<Function *, FuncDFSummary> &summary,
                              FreeEnv &env,
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
        bool isMust = sit->second.mustFreedArgs.count(ai) > 0;
        if (isMust) {
            if (envGet(env, r) == FreeState::Freed) {
                recordDoubleFreeBug(F, CI, ai, true, dedup, out);
            }
            envSet(env, r, FreeState::Freed);
        } else {
            // 仅可能释放：保持保守，减少冲突路径误报。
            FreeState oldS = envGet(env, r);
            if (oldS == FreeState::Freed) envSet(env, r, FreeState::Freed);
            else envSet(env, r, FreeState::MaybeFreed);
        }
    }
}

static void analyzeOneFunction(Function &F,
                               const std::set<std::string> &freeFuncs,
                               const std::map<Function *, FuncDFSummary> &summary,
                               std::set<std::string> &dedup,
                               std::vector<BugRecord> &out) {
    PointerSlotUF uf = buildPointerSlotUF(F);
    std::map<BasicBlock *, FreeEnv> inEnv;
    std::set<BasicBlock *> inited;
    std::deque<BasicBlock *> q;

    BasicBlock *entry = &F.getEntryBlock();
    inEnv[entry] = FreeEnv();
    inited.insert(entry);
    q.push_back(entry);

    while (!q.empty()) {
        BasicBlock *BB = q.front();
        q.pop_front();
        FreeEnv env = inEnv[BB];

        for (Instruction &I : *BB) {
            handleStoreReset(I, uf, env);

            auto *CI = dyn_cast<CallInst>(&I);
            if (!CI) continue;

            std::string callee = getCalledFuncName(CI).str();
            if (freeFuncs.count(callee) && CI->arg_size() >= 1) {
                handleDirectFree(F, CI, uf, env, dedup, out);
                continue;
            }

            handleSummaryFree(F, CI, uf, summary, env, dedup, out);
        }

        Instruction *T = BB->getTerminator();
        for (unsigned i = 0; i < T->getNumSuccessors(); ++i) {
            BasicBlock *S = T->getSuccessor(i);
            if (!inited.count(S)) {
                inEnv[S] = env;
                inited.insert(S);
                q.push_back(S);
                continue;
            }
            if (mergeEnv(inEnv[S], env)) {
                q.push_back(S);
            }
        }
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
