#include "UAFPass.h"
//先提取函数间的信息（跨函数摘要），再用这些信息在每个函数内进行更精细的检查，检测Use-After-Free漏洞
UAFPass::UAFPass(const std::set<std::string> &freeFuncs) : FreeFuncs(freeFuncs) {}

std::string UAFPass::name() const {
    return "UseAfterFree";
}

namespace {//namespace用于限定函数作用域，避免与其他文件中的同名函数冲突

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

struct FuncUAFSummary {//函数摘要
    std::set<unsigned> freedArgs;//freedArgs表示函数中被释放的指针参数的索引集合
    std::set<unsigned> mustFreedArgs;//mustFreedArgs表示函数中在所有路径上都会释放的指针参数索引集合（保守近似）
    std::set<unsigned> usedArgs;//usedArgs表示函数中被使用的指针参数的索引集合
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

    if (!hasConditionalBranch(F)) {
        out.mustFreedArgs = out.freedArgs;
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
            if (now.freedArgs != it.second.freedArgs ||
                now.mustFreedArgs != it.second.mustFreedArgs ||
                now.usedArgs != it.second.usedArgs) {
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
                                  FreeEnv &env) {
    auto *SI = dyn_cast<StoreInst>(&I);
    if (!SI) return;
    Value *slotRoot = finalPtrSlot(SI->getPointerOperand(), uf);
    if (slotRoot) envSet(env, slotRoot, FreeState::NotFreed);
}

static void handleCallEffects(Function &F,
                              Instruction &I,
                              PointerSlotUF &uf,
                              const std::set<std::string> &freeFuncs,
                              const std::map<Function *, FuncUAFSummary> &summary,
                              FreeEnv &env,
                              std::set<std::string> &dedup,
                              std::vector<BugRecord> &out) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI) return;

    std::string callee = getCalledFuncName(CI).str();
    if (freeFuncs.count(callee) && CI->arg_size() >= 1) {
        Value *arg0 = finalPtrSlot(CI->getArgOperand(0), uf);
        if (arg0) envSet(env, arg0, FreeState::Freed);
        return;
    }

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
            envSet(env, r, FreeState::Freed);
        } else {
            FreeState oldS = envGet(env, r);
            if (oldS == FreeState::Freed) envSet(env, r, FreeState::Freed);
            else envSet(env, r, FreeState::MaybeFreed);
        }
    }

    for (unsigned ai : sit->second.usedArgs) {
        if (ai >= CI->arg_size()) continue;
        Value *r = finalPtrSlot(CI->getArgOperand(ai), uf);
        if (!r || envGet(env, r) != FreeState::Freed) continue;//仅在“必然已释放”时报告，避免冲突路径误报

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
                             const FreeEnv &env,
                             std::set<std::string> &dedup,
                             std::vector<BugRecord> &out) {
    for (Use &U : I.operands()) {
        Value *op = finalPtrSlot(U.get(), uf);
        if (!op || envGet(env, op) != FreeState::Freed) continue;

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
            handleStoreInvalidate(I, uf, env);

            auto *CI = dyn_cast<CallInst>(&I);
            bool isDirectFree = false;
            if (CI) {
                std::string callee = getCalledFuncName(CI).str();
                isDirectFree = freeFuncs.count(callee) && CI->arg_size() >= 1;
            }

            if (!isDirectFree) {
                detectOperandUAF(F, I, uf, freeFuncs, env, dedup, out);
            }

            handleCallEffects(F, I, uf, freeFuncs, summary, env, dedup, out);
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
