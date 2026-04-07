#include "CheckCommon.h"
#include <fstream>

std::string instToString(Instruction *I) {
    std::string buf;
    raw_string_ostream os(buf);
    if (I) I->print(os);
    return os.str();
}

std::string locToString(Instruction *I) {
    if (!I) return "<no-inst>";
    DILocation *Loc = I->getDebugLoc();
    if (!Loc) return "<no-debug-loc>";
    return Loc->getFilename().str() + ":" + std::to_string(Loc->getLine());
}

Value *stripPtr(Value *V) {
    return V ? V->stripPointerCasts() : nullptr;
}

Value *canonicalPtr(Value *V, int maxDepth) {
    V = stripPtr(V);
    if (!V) return nullptr;

    for (int depth = 0; depth < maxDepth; ++depth) {
        auto *LI = dyn_cast<LoadInst>(V);
        if (!LI) break;
        Value *slot = stripPtr(LI->getPointerOperand());
        if (!slot) break;
        V = slot;
    }
    return V;
}

Value *PointerSlotUF::find(Value *v) {
    v = stripPtr(v);
    if (!v) return nullptr;
    auto it = parent.find(v);//parent.find(v)在parent中查找键为v的元素
    if (it == parent.end()) {//新元素，初始化并返回
        parent[v] = v;
        rank[v] = 0;
        return v;
    }
    if (it->second == v) return v;//已是根节点
    it->second = find(it->second);//路径压缩优化
    return it->second;
}

void PointerSlotUF::unite(Value *a, Value *b) {
    a = find(a);
    b = find(b);
    if (!a || !b || a == b) return;//无效元素或已在同一集合中，无需合并

    unsigned ra = rank[a];//rank[a]和rank[b]分别表示集合a和b的秩（近似树高），用于优化合并过程
    unsigned rb = rank[b];
    if (ra < rb) {
        parent[a] = b;
    } else if (ra > rb) {
        parent[b] = a;
    } else {
        parent[b] = a;
        rank[a] = ra + 1;
    }
}

PointerSlotUF buildPointerSlotUF(Function &F) {
    PointerSlotUF uf;

    for (Instruction &I : instructions(&F)) {
        if (auto *SI = dyn_cast<StoreInst>(&I)) {
            Value *stored = stripPtr(SI->getValueOperand());
            Value *slot = stripPtr(SI->getPointerOperand());
            if (stored && slot && stored->getType()->isPointerTy() && slot->getType()->isPointerTy()) {
                uf.unite(stored, slot);
            }
            continue;
        }

        if (auto *LI = dyn_cast<LoadInst>(&I)) {
            if (LI->getType()->isPointerTy()) {
                uf.unite(LI, LI->getPointerOperand());
            }
            continue;
        }

        if (auto *BC = dyn_cast<BitCastInst>(&I)) {
            if (BC->getType()->isPointerTy() && BC->getOperand(0)->getType()->isPointerTy()) {
                uf.unite(BC, BC->getOperand(0));
            }
            continue;
        }

        if (auto *ASC = dyn_cast<AddrSpaceCastInst>(&I)) {
            if (ASC->getType()->isPointerTy() && ASC->getOperand(0)->getType()->isPointerTy()) {
                uf.unite(ASC, ASC->getOperand(0));
            }
            continue;
        }

        if (auto *PHI = dyn_cast<PHINode>(&I)) {
            if (!PHI->getType()->isPointerTy()) continue;
            for (Value *inc : PHI->incoming_values()) {
                if (inc && inc->getType()->isPointerTy()) uf.unite(PHI, inc);
            }
            continue;
        }

        if (auto *SEL = dyn_cast<SelectInst>(&I)) {
            if (!SEL->getType()->isPointerTy()) continue;
            Value *tv = SEL->getTrueValue();
            Value *fv = SEL->getFalseValue();
            if (tv && tv->getType()->isPointerTy()) uf.unite(SEL, tv);
            if (fv && fv->getType()->isPointerTy()) uf.unite(SEL, fv);
            continue;
        }
    }

    return uf;
}

Value *finalPtrSlot(Value *V, PointerSlotUF &uf) {
    Value *base = canonicalPtr(V);
    return base ? uf.find(base) : nullptr;
}

Function *resolveDirectCallee(CallInst *CI) {//解析直接调用的函数，如果是间接调用则返回nullptr
    if (!CI) return nullptr;
    if (Function *CF = CI->getCalledFunction()) return CF;
    Value *called = CI->getCalledOperand();
    called = called ? called->stripPointerCasts() : nullptr;
    return dyn_cast_or_null<Function>(called);
}

std::map<Value *, unsigned> buildPointerArgRootIndex(Function &F, PointerSlotUF &uf) {
    std::map<Value *, unsigned> argRootToIdx;
    unsigned idx = 0;
    for (Argument &A : F.args()) {
        if (A.getType()->isPointerTy()) {
            Value *r = finalPtrSlot(&A, uf);
            if (r) argRootToIdx[r] = idx;
        }
        ++idx;
    }
    return argRootToIdx;
}

void addBug(std::vector<BugRecord> &out,
            const std::string &kind,
            Function *F,
            Instruction *I,
            const std::string &detail) {
    BugRecord r;
    r.kind = kind;
    r.function = F ? F->getName().str() : "<no-func>";
    r.location = locToString(I);
    r.detail = detail;
    out.push_back(r);
}

void writeBugReport(const std::string &path,
                    const std::vector<BugRecord> &bugs,
                    const std::map<std::string, long long> &passMs) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;

    ofs << "=========== Bug Report ===========\n";
    ofs << "Total bugs: " << bugs.size() << "\n\n";

    ofs << "Pass timings (ms):\n";
    for (auto &p : passMs) {
        ofs << "  - " << p.first << ": " << p.second << "\n";
    }
    ofs << "\n";

    for (const auto &b : bugs) {
        ofs << "[" << b.kind << "] "
            << "Function=" << b.function
            << " | Loc=" << b.location
            << " | " << b.detail << "\n";
    }
}

CheckPassBase::~CheckPassBase() = default;
