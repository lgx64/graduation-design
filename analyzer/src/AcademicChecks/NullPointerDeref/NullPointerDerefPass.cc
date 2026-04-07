#include "NullPointerDerefPass.h"
#include <deque>
#include <unordered_map>
//通过路径敏感分析检测空指针解引用风险（只对二分支路径且条件是判断是否为空指针的路径敏感，多分支路径继承前面路径的状态）
//在分支处按p == NULL/p != NULL细化路径状态
//对load/store/gep和危险调用参数做“可能为空”判定并报bug

std::string NullPointerDerefPass::name() const {
    return "NullPointerDeref";
}

namespace {

enum class NullState {//考虑指针状态的三种可能：未知、空、非空
    Unknown = 0,
    Null = 1,
    NonNull = 2,
};

using Env = std::unordered_map<Value *, NullState>;//每条路径维护一个抽象环境，记录当前环境下每个指针Value对象的NullState状态

struct PathItem {//worklist元素
    BasicBlock *BB;//当前块
    Env env;//当前路径环境
    int depth;//当前路径深度
};
//防止路径爆炸
constexpr int kMaxExecDepth = 256;
constexpr int kMaxBlockVisits = 32;

static NullState joinState(NullState a, NullState b) {//汇总节点状态
    if (a == b) return a;
    return NullState::Unknown;
}

static NullState envGet(const Env &env, Value *V) {//从环境中获取指针状态
    auto it = env.find(V);
    return it == env.end() ? NullState::Unknown : it->second;
}

static void envSet(Env &env, Value *V, NullState s) {//设置环境中指针状态
    if (!V) return;
    env[V] = s;
}

static bool nullDangerousCallArg(CallInst *CI, unsigned argIdx) {//只对如下函数对应危险参数做空指针危险调用参数的判定
    if (!CI) return false;
    std::string callee = getCalledFuncName(CI).str();

    if (callee.find("llvm.memcpy") == 0 || callee == "memcpy" ||
        callee.find("llvm.memmove") == 0 || callee == "memmove") {
        return argIdx == 0 || argIdx == 1;
    }

    if (callee.find("llvm.memset") == 0 || callee == "memset") {
        return argIdx == 0;
    }

    if (callee == "strlen") return argIdx == 0;
    if (callee == "strcpy" || callee == "strcmp") return argIdx <= 1;
    if (callee == "strncpy" || callee == "strncmp") return argIdx <= 1;
    if (callee == "strcat" || callee == "strncat") return argIdx <= 1;

    // 对用户自定义函数：若形参是指针，传入可空实参也视为风险（函数体内可能解引用）。
    if (Function *CF = resolveDirectCallee(CI)) {
        if (!CF->isDeclaration() && argIdx < CF->arg_size()) {
            auto it = CF->arg_begin();
            std::advance(it, static_cast<long>(argIdx));
            if (it->getType()->isPointerTy()) return true;
        }
    }

    return false;
}

static bool maybeNullFallback(Value *V, int depth = 0) {//当主状态推理不够确定时，它再快速检查V的来源里是否可能包含null
    V = stripPtr(V);
    if (!V || depth > 4) return false;

    if (isa<ConstantPointerNull>(V)) return true;

    if (auto *PN = dyn_cast<PHINode>(V)) {
        for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
            if (maybeNullFallback(PN->getIncomingValue(i), depth + 1)) return true;
        }
    }

    if (auto *SI = dyn_cast<SelectInst>(V)) {
        if (maybeNullFallback(SI->getTrueValue(), depth + 1)) return true;
        if (maybeNullFallback(SI->getFalseValue(), depth + 1)) return true;
    }

    return false;
}

static NullState evalPtrState(Value *V, const Env &env, int depth = 0) {//输入一个指针值V和当前路径环境env，输出它在此路径下的状态
    V = stripPtr(V);
    if (!V || depth > 8) return NullState::Unknown;

    if (isa<ConstantPointerNull>(V)) return NullState::Null;
    if (isa<GlobalValue>(V)) return NullState::NonNull;
    if (isa<AllocaInst>(V)) return NullState::NonNull;
    if (isa<Argument>(V)) return NullState::Unknown;

    NullState inEnv = envGet(env, V);
    if (inEnv != NullState::Unknown) return inEnv;

    if (auto *BC = dyn_cast<BitCastInst>(V)) {//追溯
        return evalPtrState(BC->getOperand(0), env, depth + 1);
    }
    if (auto *ASC = dyn_cast<AddrSpaceCastInst>(V)) {//追溯
        return evalPtrState(ASC->getOperand(0), env, depth + 1);
    }
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
        return evalPtrState(GEP->getPointerOperand(), env, depth + 1);
    }
    if (auto *PN = dyn_cast<PHINode>(V)) {//逐incoming求值后joinState
        NullState acc = NullState::Unknown;
        bool first = true;
        for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
            NullState s = evalPtrState(PN->getIncomingValue(i), env, depth + 1);
            if (first) {
                acc = s;
                first = false;
            } else {
                acc = joinState(acc, s);
            }
        }
        return first ? NullState::Unknown : acc;
    }
    if (auto *SI = dyn_cast<SelectInst>(V)) {
        NullState t = evalPtrState(SI->getTrueValue(), env, depth + 1);
        NullState f = evalPtrState(SI->getFalseValue(), env, depth + 1);
        return joinState(t, f);
    }
    if (auto *LI = dyn_cast<LoadInst>(V)) {
        if (!LI->getType()->isPointerTy()) return NullState::Unknown;
        Value *slot = stripPtr(LI->getPointerOperand());
        NullState fromEnv = envGet(env, slot);
        if (fromEnv != NullState::Unknown) return fromEnv;
        return NullState::Unknown;
    }

    return NullState::Unknown;
}

static bool isPossiblyNullArg(CallInst *CI, unsigned argIdx, const Env &env) {//危险参数是否可能为null
    if (!CI || argIdx >= CI->arg_size()) return false;
    Value *arg = CI->getArgOperand(argIdx);
    NullState s = evalPtrState(arg, env);
    if (s == NullState::Null || s == NullState::Unknown) return true;
    return maybeNullFallback(arg);
}

static bool ptrDerefRisk(Value *ptr, const Env &env) {//是否可能解引用空指针
    if (!ptr) return false;
    NullState s = evalPtrState(ptr, env);
    if (s == NullState::Null || s == NullState::Unknown) return true;
    return maybeNullFallback(ptr);
}

static bool refineByNullCmp(Value *cond, Env &trueEnv, Env &falseEnv) {//是否能够通过条件表达式cond细化路径状态
    auto *IC = dyn_cast<ICmpInst>(cond);//指针比较
    if (!IC) return false;

    Value *op0 = stripPtr(IC->getOperand(0));
    Value *op1 = stripPtr(IC->getOperand(1));
    Value *target = nullptr;
    if (isa<ConstantPointerNull>(op0)) target = op1;
    else if (isa<ConstantPointerNull>(op1)) target = op0;
    if (!target || !target->getType()->isPointerTy()) return false;

    //如果比较指令中一个指针是与一个空指针进行比较，则分类讨论比较的谓词
    if (IC->getPredicate() == CmpInst::ICMP_EQ) {
        envSet(trueEnv, target, NullState::Null);
        envSet(falseEnv, target, NullState::NonNull);
        return true;
    }
    if (IC->getPredicate() == CmpInst::ICMP_NE) {
        envSet(trueEnv, target, NullState::NonNull);
        envSet(falseEnv, target, NullState::Null);
        return true;
    }
    return false;
}

static void processInstructionForState(Instruction &I, Env &env) {//状态转移函数
    //遇到一条指令I，就把Env更新到执行完这条指令后的状态
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
        Value *val = SI->getValueOperand();
        if (val->getType()->isPointerTy()) {
            envSet(env, stripPtr(SI->getPointerOperand()), evalPtrState(val, env));
        }
        return;
    }

    if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (LI->getType()->isPointerTy()) {
            Value *slot = stripPtr(LI->getPointerOperand());
            NullState s = envGet(env, slot);
            if (s == NullState::Unknown) s = evalPtrState(LI->getPointerOperand(), env);
            envSet(env, LI, s);
        }
        return;
    }

    if (auto *BC = dyn_cast<BitCastInst>(&I)) {
        if (BC->getType()->isPointerTy()) {
            envSet(env, BC, evalPtrState(BC->getOperand(0), env));
        }
        return;
    }

    if (auto *ASC = dyn_cast<AddrSpaceCastInst>(&I)) {
        if (ASC->getType()->isPointerTy()) {
            envSet(env, ASC, evalPtrState(ASC->getOperand(0), env));
        }
        return;
    }

    if (auto *SEL = dyn_cast<SelectInst>(&I)) {
        if (SEL->getType()->isPointerTy()) {
            NullState t = evalPtrState(SEL->getTrueValue(), env);
            NullState f = evalPtrState(SEL->getFalseValue(), env);
            envSet(env, SEL, joinState(t, f));
        }
        return;
    }

    if (auto *PHI = dyn_cast<PHINode>(&I)) {
        if (PHI->getType()->isPointerTy()) {
            NullState acc = NullState::Unknown;
            bool first = true;
            for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
                NullState s = evalPtrState(PHI->getIncomingValue(i), env);
                if (first) {
                    acc = s;
                    first = false;
                } else {
                    acc = joinState(acc, s);
                }
            }
            envSet(env, PHI, first ? NullState::Unknown : acc);
        }
        return;
    }

    if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (CI->getType()->isPointerTy()) {
            envSet(env, CI, NullState::Unknown);
        }
    }
}

} // namespace

void NullPointerDerefPass::run(ModuleMap &modules, std::vector<BugRecord> &out) {
    std::set<std::string> dedup;

    for (auto &p : modules) {
        Module *M = p.second;

        for (Function &F : *M) {
            if (F.isDeclaration()) continue;

            std::deque<PathItem> work;
            std::unordered_map<BasicBlock *, int> bbVisits;
            work.push_back({&F.getEntryBlock(), Env(), 0});

            while (!work.empty()) {
                PathItem cur = std::move(work.front());
                work.pop_front();

                if (!cur.BB) continue;
                if (cur.depth > kMaxExecDepth) continue;

                int &vis = bbVisits[cur.BB];
                if (vis >= kMaxBlockVisits) continue;
                ++vis;

                Env env = std::move(cur.env);

                for (Instruction &I : *cur.BB) {
                    Value *ptr = nullptr;
                    if (auto *LI = dyn_cast<LoadInst>(&I)) ptr = LI->getPointerOperand();
                    else if (auto *SI = dyn_cast<StoreInst>(&I)) ptr = SI->getPointerOperand();
                    else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) ptr = GEP->getPointerOperand();

                    if (ptr && ptrDerefRisk(ptr, env)) {
                        std::string key = F.getName().str() + "|" + locToString(&I) + "|NPD";
                        if (dedup.insert(key).second) {
                            addBug(out, "NullPointerDeref", &F, &I,
                                   "Path-sensitive symbolic execution found possible null dereference. Inst=" +
                                       instToString(&I));
                        }
                    }

                    if (auto *CI = dyn_cast<CallInst>(&I)) {
                        for (unsigned ai = 0; ai < CI->arg_size(); ++ai) {
                            if (!nullDangerousCallArg(CI, ai)) continue;
                            if (!isPossiblyNullArg(CI, ai, env)) continue;

                            std::string key = F.getName().str() + "|" + locToString(&I) + "|NPD_CALL|" + std::to_string(ai);
                            if (!dedup.insert(key).second) continue;

                            addBug(out, "NullPointerDeref", &F, &I,
                                   "Path-sensitive symbolic execution found possible null call argument (arg" +
                                       std::to_string(ai) + "). Inst=" + instToString(&I));
                        }
                    }

                    processInstructionForState(I, env);
                }

                auto *TI = dyn_cast<Instruction>(cur.BB->getTerminator());
                if (!TI) continue;

                if (auto *BI = dyn_cast<BranchInst>(TI)) {
                    if (!BI->isConditional()) {
                        work.push_back({BI->getSuccessor(0), env, cur.depth + 1});
                        continue;
                    }

                    Env trueEnv = env;
                    Env falseEnv = env;
                    refineByNullCmp(BI->getCondition(), trueEnv, falseEnv);
                    work.push_back({BI->getSuccessor(0), std::move(trueEnv), cur.depth + 1});
                    work.push_back({BI->getSuccessor(1), std::move(falseEnv), cur.depth + 1});
                    continue;
                }

                auto *term = cur.BB->getTerminator();
                for (unsigned si = 0; si < term->getNumSuccessors(); ++si) {
                    work.push_back({term->getSuccessor(si), env, cur.depth + 1});
                }
            }
        }
    }
}
