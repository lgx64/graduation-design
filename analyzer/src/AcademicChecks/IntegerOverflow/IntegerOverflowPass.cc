#include "IntegerOverflowPass.h"
//找出那些“可能发生整数溢出，并且这个结果后续被用作大小或索引”的整数运算
IntegerOverflowPass::IntegerOverflowPass(const std::set<std::string> &allocFuncs)
    : AllocFuncs(allocFuncs) {}

std::string IntegerOverflowPass::name() const {
    return "IntegerOverflow";
}

static bool hasOverflowCheck(Instruction *I) {//是否出现名字含overflow的函数调用
    for (User *U : I->users()) {
        if (auto *CI = dyn_cast<CallInst>(U)) {
            if (Function *CF = CI->getCalledFunction()) {//只看直接调用
                std::string n = CF->getName().str();
                if (n.find("overflow") != std::string::npos) return true;//函数名里查找子串overflow
            }
        }
    }
    return false;
}

static bool isPassThroughIntNode(User *U) {//类型转换、PHI节点、Select指令和某些二元运算
    return isa<CastInst>(U) || isa<PHINode>(U) || isa<SelectInst>(U) || isa<BinaryOperator>(U);
}

static bool usedAsSizeOrIndexRec(Value *V,//当前要追踪的数据流起点（通常是某条整数算术指令的结果）
                                 const std::set<std::string> &allocFuncs,//分配内存的函数名称集合
                                 std::set<Value *> &visited,//是否访问过该Value对象，避免递归过程中出现死循环
                                 int depth = 0) {
    if (!V || depth > 8) return false;
    if (!visited.insert(V).second) return false;//visited.insert(V)返回一个pair<set<Value *>::iterator, bool>

    for (User *U : V->users()) {
        if (isa<GetElementPtrInst>(U)) return true;//参与了地址计算，可能作为索引
        if (auto *AI = dyn_cast<AllocaInst>(U)) {
            if (AI->isArrayAllocation()) return true;
        }
        if (auto *CI = dyn_cast<CallInst>(U)) {
            std::string callee = getCalledFuncName(CI).str();
            if (allocFuncs.count(callee)) return true;
            if (callee == "memcpy" || callee == "memmove" || callee == "memset") return true;
        }

        if (isPassThroughIntNode(U) && usedAsSizeOrIndexRec(U, allocFuncs, visited, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool usedAsSizeOrIndex(Instruction *I, const std::set<std::string> &allocFuncs) {
    std::set<Value *> visited;
    return usedAsSizeOrIndexRec(I, allocFuncs, visited);
}

void IntegerOverflowPass::run(ModuleMap &modules, std::vector<BugRecord> &out) {
    std::set<std::string> dedup;

    for (auto &p : modules) {
        Module *M = p.second;
        for (Function &F : *M) {
            if (F.isDeclaration()) continue;

            for (Instruction &I : instructions(&F)) {
                auto *BO = dyn_cast<BinaryOperator>(&I);
                if (!BO) continue;
                if (!BO->getType()->isIntegerTy()) continue;//只考虑整数类型的二元运算

                unsigned op = BO->getOpcode();
                if (!(op == Instruction::Add || op == Instruction::Sub ||
                      op == Instruction::Mul || op == Instruction::Shl)) {
                    continue;
                }

                if (op == Instruction::Mul) {
                    auto *C0 = dyn_cast<ConstantInt>(BO->getOperand(0));
                    auto *C1 = dyn_cast<ConstantInt>(BO->getOperand(1));
                    // 常见的 sizeof(T) * n 分配模式：小常量因子乘法，误报率较高，先保守跳过。
                    if ((C0 && C0->getZExtValue() <= 1024) ||
                        (C1 && C1->getZExtValue() <= 1024)) {
                        continue;
                    }
                }

                if (isa<ConstantInt>(BO->getOperand(0)) && isa<ConstantInt>(BO->getOperand(1))) {
                    continue;
                }

                if (auto *OBO = dyn_cast<OverflowingBinaryOperator>(BO)) {//尝试将二元运算指令转换为可带溢出语义的二元运算，如果成功则说明该指令支持无符号或有符号溢出检查
                    if (OBO->hasNoSignedWrap() || OBO->hasNoUnsignedWrap()) continue;
                }

                if (hasOverflowCheck(BO)) continue;
                if (!usedAsSizeOrIndex(BO, AllocFuncs)) continue;

                std::string key = F.getName().str() + "|" + locToString(&I) + "|IO";
                if (!dedup.insert(key).second) continue;

                addBug(out, "IntegerOverflow", &F, &I,
                       "Potential integer overflow impacting size/index. Inst=" + instToString(&I));
            }
        }
    }
}
