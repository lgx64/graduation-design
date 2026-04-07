#include "BufferOverflowPass.h"
#include <unordered_set>

std::string BufferOverflowPass::name() const {
    return "BufferOverflow";
}

namespace {

struct CopyEffect {//跨函数拷贝摘要的一条记录，记录函数对目标参数写了多少字节以及是什么类型
    unsigned dstArg = 0;//目标参数索引，表示函数参数中被复制数据写入的指针参数的索引位置
    uint64_t bytes = 0;//表示复制的数据字节数
    std::string kind;//表示复制操作的类型，如"memcpy"、"memmove"等
};

using CopySummary = std::vector<CopyEffect>;//复制摘要

static Value *basePtr(Value *V) {//指针溯源
    while (V) {
        if (isa<AllocaInst>(V) || isa<Argument>(V) || isa<GlobalVariable>(V)) return V;
        if (auto *GEP = dyn_cast<GetElementPtrInst>(V)) {
            V = stripPtr(GEP->getPointerOperand());
            continue;
        }
        if (auto *BC = dyn_cast<BitCastInst>(V)) {
            V = stripPtr(BC->getOperand(0));
            continue;
        }
        if (auto *LI = dyn_cast<LoadInst>(V)) {
            V = stripPtr(LI->getPointerOperand());
            continue;
        }
        break;
    }
    return V;
}

static uint64_t constByteOffsetFromBase(Value *V, const DataLayout &DL) {//计算指针V相对其基对象的“常量字节偏移”
    uint64_t offset = 0;
    Value *cur = stripPtr(V);

    while (cur) {
        if (auto *GEP = dyn_cast<GetElementPtrInst>(cur)) {
            APInt apOff(DL.getPointerTypeSizeInBits(GEP->getType()), 0, true);//DL.getPointerTypeSizeInBits(GEP->getType())获取指针类型的位数，0表示初始值为0，true表示有符号整数
            if (GEP->accumulateConstantOffset(DL, apOff)) {//如果可以静态求值则将偏移累加到apOff中
                offset += apOff.getZExtValue();
            }
            cur = stripPtr(GEP->getPointerOperand());//剥离这一层指针转换后继续向下追溯
            continue;
        }
        if (auto *BC = dyn_cast<BitCastInst>(cur)) {
            cur = stripPtr(BC->getOperand(0));
            continue;
        }
        if (auto *LI = dyn_cast<LoadInst>(cur)) {
            cur = stripPtr(LI->getPointerOperand());
            continue;
        }
        break;
    }

    return offset;
}

static uint64_t knownAllocaSize(AllocaInst *AI, const DataLayout &DL) {//获取alloca指令分配的内存大小
    uint64_t elem = DL.getTypeAllocSize(AI->getAllocatedType());
    if (!AI->isArrayAllocation()) return elem;
    if (auto *C = dyn_cast<ConstantInt>(AI->getArraySize())) return elem * C->getZExtValue();
    return 0;//长度非固定
}

static uint64_t knownGlobalObjSize(GlobalVariable *GV, const DataLayout &DL) {//获取全局变量大小
    if (!GV || !GV->hasInitializer()) return 0;
    Type *ty = GV->getValueType();
    if (!ty || !ty->isSized()) return 0;
    return DL.getTypeAllocSize(ty);
}

static uint64_t knownConstStringLength(Value *V) {//获取常量字符串长度
    V = stripPtr(V);
    if (auto *GV = dyn_cast<GlobalVariable>(V)) {
        if (!GV->hasInitializer()) return 0;
        if (auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer())) {
            if (CDA->isString()) return CDA->getAsString().size() + 1;
        }
    }
    return 0;
}

static bool isMemOpCall(const std::string &callee) {//是否是内存操作函数调用
    return (callee == "memcpy" || callee == "memmove" || callee == "memset" ||
            callee.find("llvm.memcpy") == 0 || callee.find("llvm.memmove") == 0 ||
            callee.find("llvm.memset") == 0);
}

static uint64_t accessBytesByInstruction(Instruction *I) {//估算一条内存访问指令实际读/写了多少字节
    if (auto *LI = dyn_cast<LoadInst>(I)) {
        Type *ty = LI->getType();
        return ty && ty->isSized() ? LI->getModule()->getDataLayout().getTypeStoreSize(ty) : 0;
    }
    if (auto *SI = dyn_cast<StoreInst>(I)) {
        Type *ty = SI->getValueOperand()->getType();
        return ty && ty->isSized() ? SI->getModule()->getDataLayout().getTypeStoreSize(ty) : 0;
    }
    return 0;
}

static int pointerArgIndex(Value *V,
                           PointerSlotUF &uf,
                           const std::map<Value *, unsigned> &argRootToIdx) {//获取指针类型的value对象在函数参数中对应的索引位置
    Value *r = finalPtrSlot(V, uf);
    auto it = argRootToIdx.find(r);
    if (it == argRootToIdx.end()) return -1;
    return static_cast<int>(it->second);
}

static std::string effectKey(const CopyEffect &e) {//将复制记录转字符串
    return std::to_string(e.dstArg) + "#" + std::to_string(e.bytes) + "#" + e.kind;
}

static CopySummary collectOneSummary(Function &F,
                                     const std::map<Function *, CopySummary> &known) {//给单个函数F提炼“它会向哪个参数写多少字节”的摘要
    CopySummary out;
    std::unordered_set<std::string> dedup;

    PointerSlotUF uf = buildPointerSlotUF(F);
    auto argRootToIdx = buildPointerArgRootIndex(F, uf);

    for (Instruction &I : instructions(&F)) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;

        std::string callee = getCalledFuncName(CI).str();
        if (isMemOpCall(callee) && CI->arg_size() >= 3) {//直接调用内存操作函数，且参数数量至少为3
            auto *lenC = dyn_cast<ConstantInt>(CI->getArgOperand(2));
            if (!lenC) continue;
            int idx = pointerArgIndex(CI->getArgOperand(0), uf, argRootToIdx);
            if (idx < 0) continue;

            CopyEffect e{static_cast<unsigned>(idx), lenC->getZExtValue(), "direct-memop"};
            if (dedup.insert(effectKey(e)).second) out.push_back(e);
            continue;
        }

        if (callee == "strcpy" && CI->arg_size() >= 2) {//字符串拷贝函数调用，且参数数量至少为2
            int idx = pointerArgIndex(CI->getArgOperand(0), uf, argRootToIdx);
            if (idx < 0) continue;
            uint64_t bytes = knownConstStringLength(CI->getArgOperand(1));
            if (!bytes) continue;
            CopyEffect e{static_cast<unsigned>(idx), bytes, "direct-strcpy"};
            if (dedup.insert(effectKey(e)).second) out.push_back(e);
            continue;
        }

        if (callee == "strncpy" && CI->arg_size() >= 3) {//字符串拷贝函数调用，且参数数量至少为3，第三个参数是常量，表示要拷贝的字节数
            int idx = pointerArgIndex(CI->getArgOperand(0), uf, argRootToIdx);
            if (idx < 0) continue;
            auto *n = dyn_cast<ConstantInt>(CI->getArgOperand(2));
            if (!n) continue;
            CopyEffect e{static_cast<unsigned>(idx), n->getZExtValue(), "direct-strncpy"};
            if (dedup.insert(effectKey(e)).second) out.push_back(e);
            continue;
        }
        
        {//跨函数传播调用的函数中对dst指针的写入效果
            Function *CF = resolveDirectCallee(CI);
            if (!CF) continue;
            auto sit = known.find(CF);
            if (sit == known.end()) continue;

            for (const auto &ce : sit->second) {
                if (ce.dstArg >= CI->arg_size()) continue;
                int idx = pointerArgIndex(CI->getArgOperand(ce.dstArg), uf, argRootToIdx);
                if (idx < 0) continue;
                CopyEffect e{static_cast<unsigned>(idx), ce.bytes, "ctx-" + ce.kind};
                if (dedup.insert(effectKey(e)).second) out.push_back(e);
            }
        }
    }

    return out;
}

static std::map<Function *, CopySummary> buildInterprocCopySummary(ModuleMap &modules) {//构建跨过程的复制摘要
    std::map<Function *, CopySummary> summary;
    for (auto &p : modules) {
        for (Function &F : *p.second) {
            if (!F.isDeclaration()) summary[&F] = CopySummary();
        }
    }

    for (int iter = 0; iter < kInterprocMaxIters; ++iter) {
        bool changed = false;
        auto old = summary;
        for (auto &it : summary) {
            CopySummary now = collectOneSummary(*it.first, old);
            std::unordered_set<std::string> a, b;
            for (const auto &x : now) a.insert(effectKey(x));
            for (const auto &x : it.second) b.insert(effectKey(x));
            if (a != b) {
                it.second = std::move(now);//使用move避免复制
                changed = true;
            }
        }
        if (!changed) break;
    }
    return summary;
}

static void propagateAliasBases(Function &F,
                                PointerSlotUF &uf,
                                std::map<Value *, std::set<Value *>> &rootBases) {//把“某个指针根可能指向哪些基对象（alloca）沿着指针赋值关系传播开
//rootBases[v]表示v可以追溯到哪些alloca对象。通过迭代得到最终的传播结果
                                    for (Instruction &I : instructions(&F)) {
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
            Value *r = finalPtrSlot(AI, uf);
            if (r) rootBases[r].insert(AI);
        }
    }

    for (int iter = 0; iter < 6; ++iter) {
        bool changed = false;
        for (Instruction &I : instructions(&F)) {
            auto *SI = dyn_cast<StoreInst>(&I);
            if (!SI || !SI->getValueOperand()->getType()->isPointerTy()) continue;

            Value *srcRoot = finalPtrSlot(SI->getValueOperand(), uf);
            Value *dstRoot = finalPtrSlot(SI->getPointerOperand(), uf);
            if (!srcRoot || !dstRoot) continue;

            auto sit = rootBases.find(srcRoot);
            if (sit == rootBases.end()) continue;

            auto &dstSet = rootBases[dstRoot];
            size_t old = dstSet.size();
            dstSet.insert(sit->second.begin(), sit->second.end());
            if (dstSet.size() != old) changed = true;
        }
        if (!changed) break;
    }
}

static Value *findSizedAliasBase(Value *ptr,
                                 PointerSlotUF &uf,
                                 std::map<Value *, std::set<Value *>> &rootBases,
                                 std::map<Value *, uint64_t> &allocBytes) {//给任意指针 ptr 找到一个“既是别名候选、又已知大小”的基对象
    Value *root = finalPtrSlot(ptr, uf);
    if (root) {
        auto it = rootBases.find(root);
        if (it != rootBases.end()) {
            for (Value *cand : it->second) {
                if (allocBytes.count(cand)) return cand;
            }
        }
    }

    Value *bp = basePtr(ptr);
    if (bp && allocBytes.count(bp)) return bp;
    return nullptr;
}

} // namespace

static void collectObjectSizes(Function &F,
                               Module *M,
                               const DataLayout &DL,
                               std::map<Value *, uint64_t> &allocBytes) {//收集当前函数后续分析会用到的对象大小表
    for (Instruction &I : instructions(&F)) {
        if (auto *AI = dyn_cast<AllocaInst>(&I)) {
            uint64_t sz = knownAllocaSize(AI, DL);
            if (sz) allocBytes[AI] = sz;
        }
    }
    for (GlobalVariable &GV : M->globals()) {
        uint64_t sz = knownGlobalObjSize(&GV, DL);
        if (sz) allocBytes[&GV] = sz;
    }
}

static void buildAliasBases(Function &F,
                            Module *M,
                            PointerSlotUF &uf,
                            std::map<Value *, std::set<Value *>> &rootBases) {//先把“全局对象”放进别名基集合，再调用传播函数把这些基信息扩散到函数里的各个指针根
    for (GlobalVariable &GV : M->globals()) {
        Value *r = finalPtrSlot(&GV, uf);
        if (r) rootBases[r].insert(&GV);
    }
    propagateAliasBases(F, uf, rootBases);
}

static void detectMemOpOverflows(Function &F,
                                 const DataLayout &DL,
                                 PointerSlotUF &uf,
                                 std::map<Value *, std::set<Value *>> &rootBases,
                                 std::map<Value *, uint64_t> &allocBytes,
                                 std::set<std::string> &dedup,
                                 std::vector<BugRecord> &out) {
    for (Instruction &I : instructions(&F)) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;
        std::string callee = getCalledFuncName(CI).str();
        if (!isMemOpCall(callee) || CI->arg_size() < 3) continue;

        auto *lenC = dyn_cast<ConstantInt>(CI->getArgOperand(2));
        if (!lenC) continue;
        Value *dst = stripPtr(CI->getArgOperand(0));

        Value *bp = findSizedAliasBase(dst, uf, rootBases, allocBytes);
        if (!bp) continue;

        uint64_t dstSize = allocBytes[bp];
        uint64_t off = constByteOffsetFromBase(dst, DL);
        if (off >= dstSize) {//起始偏移超过目标对象大小，说明完全越界了
            std::string key = F.getName().str() + "|" +locToString(CI) + "|BO_out_of_bounds";
            if (!dedup.insert(key).second) continue;
            addBug(out, "BufferOverflow", &F, CI,
                   "Memory operation starts at offset beyond destination object size (" + 
                   std::to_string(off) + " >= " + std::to_string(dstSize) + "). Inst=" + 
                   instToString(CI));
            continue;
        }
        uint64_t remain = dstSize - off;
        uint64_t req = lenC->getZExtValue();
        if (req <= remain) continue;

        std::string key = F.getName().str() + "|" + locToString(CI) + "|BO";
        if (!dedup.insert(key).second) continue;
        addBug(out, "BufferOverflow", &F, CI,
               "Copy/set size exceeds destination object (" + std::to_string(req) +
                   " > " + std::to_string(remain) + "). Inst=" + instToString(CI));
    }
}

static void detectStringOpOverflows(Function &F,
                                    const DataLayout &DL,
                                    PointerSlotUF &uf,
                                    std::map<Value *, std::set<Value *>> &rootBases,
                                    std::map<Value *, uint64_t> &allocBytes,
                                    std::set<std::string> &dedup,
                                    std::vector<BugRecord> &out) {
    for (Instruction &I : instructions(&F)) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;
        std::string callee = getCalledFuncName(CI).str();
        bool isStrcpyLike = (callee == "strcpy");
        bool isStrncpyLike = (callee == "strncpy");
        if (!isStrcpyLike && !isStrncpyLike) continue;

        Value *dst = stripPtr(CI->getArgOperand(0));
        Value *bp = findSizedAliasBase(dst, uf, rootBases, allocBytes);
        if (!bp) continue;

        uint64_t dstSize = allocBytes[bp];
        uint64_t off = constByteOffsetFromBase(dst, DL);
        if (off >= dstSize) {
            std::string key = F.getName().str() + "|" + locToString(CI) + "|BO_out_of_bounds";
            if (!dedup.insert(key).second) continue;
            addBug(out, "BufferOverflow", &F, CI,
                   "Memory operation starts at offset beyond destination object size (" + 
                   std::to_string(off) + " >= " + std::to_string(dstSize) + "). Inst=" + 
                   instToString(CI));
            continue;
        }
        uint64_t remain = dstSize - off;

        uint64_t req = 0;
        if (isStrcpyLike) {
            req = knownConstStringLength(CI->getArgOperand(1));
            if (!req) continue;
        } else {
            if (CI->arg_size() < 3) continue;
            auto *n = dyn_cast<ConstantInt>(CI->getArgOperand(2));
            if (!n) continue;
            req = n->getZExtValue();
        }
        if (req <= remain) continue;

        std::string key = F.getName().str() + "|" + locToString(CI) + "|BO_STR";
        if (!dedup.insert(key).second) continue;
        addBug(out, "BufferOverflow", &F, CI,
               "String copy may exceed destination object (" + std::to_string(req) +
                   " > " + std::to_string(remain) + "). Inst=" + instToString(CI));
    }
}

static void detectIndexedAccessOverflows(Function &F,
                                         const DataLayout &DL,
                                         PointerSlotUF &uf,
                                         std::map<Value *, std::set<Value *>> &rootBases,
                                         std::map<Value *, uint64_t> &allocBytes,
                                         std::set<std::string> &dedup,
                                         std::vector<BugRecord> &out) {//通过GEP计算出的索引指针，在load/store时越界访问对象。
    for (Instruction &I : instructions(&F)) {
        Value *ptr = nullptr;
        if (auto *LI = dyn_cast<LoadInst>(&I)) ptr = LI->getPointerOperand();
        else if (auto *SI = dyn_cast<StoreInst>(&I)) ptr = SI->getPointerOperand();
        else continue;

        auto *GEP = dyn_cast_or_null<GetElementPtrInst>(stripPtr(ptr));
        if (!GEP) continue;

        Value *bp = findSizedAliasBase(GEP, uf, rootBases, allocBytes);
        if (!bp) continue;

        uint64_t objSize = allocBytes[bp];
        uint64_t off = constByteOffsetFromBase(GEP, DL);
        uint64_t acc = accessBytesByInstruction(&I);
        if (!acc) continue;
        if (off + acc <= objSize) continue;

        std::string key = F.getName().str() + "|" + locToString(&I) + "|BO_GEP";
        if (!dedup.insert(key).second) continue;
        addBug(out, "BufferOverflow", &F, &I,
               "Out-of-bounds memory access by indexed pointer (offset " +
                   std::to_string(off) + ", access " + std::to_string(acc) +
                   ", object " + std::to_string(objSize) + "). Inst=" + instToString(&I));
    }
}

static void detectContextSummaryOverflows(Function &F,
                                          const DataLayout &DL,
                                          PointerSlotUF &uf,
                                          std::map<Value *, std::set<Value *>> &rootBases,
                                          std::map<Value *, uint64_t> &allocBytes,
                                          const std::map<Function *, CopySummary> &interprocSummary,
                                          std::set<std::string> &dedup,
                                          std::vector<BugRecord> &out) {//基于跨函数摘要检测函数调用导致的越界风险
    for (Instruction &I : instructions(&F)) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI) continue;
        Function *CF = resolveDirectCallee(CI);
        if (!CF) continue;
        auto sit = interprocSummary.find(CF);
        if (sit == interprocSummary.end()) continue;

        for (const auto &eff : sit->second) {
            if (eff.bytes == 0 || eff.dstArg >= CI->arg_size()) continue;
            Value *dst = stripPtr(CI->getArgOperand(eff.dstArg));
            Value *bp = findSizedAliasBase(dst, uf, rootBases, allocBytes);
            if (!bp) continue;

            uint64_t dstSize = allocBytes[bp];
            uint64_t off = constByteOffsetFromBase(dst, DL);
            if (off >= dstSize) {
                std::string key = F.getName().str() + "|" + locToString(CI) + "|BO_CTX_OUT_OF_BOUNDS|" +
                                  std::to_string(eff.dstArg) + "|" + std::to_string(eff.bytes);
                if (!dedup.insert(key).second) continue;
                addBug(out, "BufferOverflow", &F, CI,
                       "Context-sensitive wrapper copy starts at offset beyond destination object size (" +
                       std::to_string(off) + " >= " + std::to_string(dstSize) + "). Inst=" +
                       instToString(CI));
                continue;
            }
            uint64_t remain = dstSize - off;
            if (eff.bytes <= remain) continue;

            std::string key = F.getName().str() + "|" + locToString(CI) + "|BO_CTX|" +
                              std::to_string(eff.dstArg) + "|" + std::to_string(eff.bytes);
            if (!dedup.insert(key).second) continue;

            addBug(out, "BufferOverflow", &F, CI,
                   "Context-sensitive wrapper copy may exceed destination object (" +
                       std::to_string(eff.bytes) + " > " + std::to_string(remain) +
                       "). Inst=" + instToString(CI));
        }
    }
}

static void analyzeOneFunction(Function &F,
                               Module *M,//Module对象表示一个LLVM IR模块，包含了函数、全局变量和其他IR元素
                               const DataLayout &DL,//datalayout提供了目标平台的数据类型大小和对齐信息
                               const std::map<Function *, CopySummary> &interprocSummary,//interprocSummary表示跨函数传播的复制效应摘要信息，记录了函数参数之间的复制关系和复制大小等信息
                               std::set<std::string> &dedup,
                               std::vector<BugRecord> &out) {
    std::map<Value *, uint64_t> allocBytes;
    collectObjectSizes(F, M, DL, allocBytes);

    PointerSlotUF uf = buildPointerSlotUF(F);
    std::map<Value *, std::set<Value *>> rootBases;
    buildAliasBases(F, M, uf, rootBases);

    detectMemOpOverflows(F, DL, uf, rootBases, allocBytes, dedup, out);
    detectStringOpOverflows(F, DL, uf, rootBases, allocBytes, dedup, out);
    detectIndexedAccessOverflows(F, DL, uf, rootBases, allocBytes, dedup, out);
    detectContextSummaryOverflows(F, DL, uf, rootBases, allocBytes, interprocSummary, dedup, out);
}

void BufferOverflowPass::run(ModuleMap &modules, std::vector<BugRecord> &out) {
    std::set<std::string> dedup;
    auto interprocSummary = buildInterprocCopySummary(modules);

    for (auto &p : modules) {
        Module *M = p.second;
        const DataLayout &DL = M->getDataLayout();

        for (Function &F : *M) {
            if (F.isDeclaration()) continue;
            analyzeOneFunction(F, M, DL, interprocSummary, dedup, out);
        }
    }
}
