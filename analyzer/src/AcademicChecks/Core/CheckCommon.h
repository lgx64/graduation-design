#ifndef ACADEMIC_CHECK_COMMON_H
#define ACADEMIC_CHECK_COMMON_H

#include "../../utils/utils.h"
#include "../../utils/Common.h"
#include "../../utils/typedef.h"
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

struct BugRecord {//记录bug的相关信息，包括bug类型、所在函数、位置和详细描述等
    std::string kind;
    std::string function;
    std::string location;
    std::string detail;
};

std::string instToString(Instruction *I);//将LLVM IR指令转换为字符串表示
std::string locToString(Instruction *I);//将LLVM IR指令的位置转换为字符串表示，格式为"文件名:行号"
Value *stripPtr(Value *V);//去除指针类型的Value对象的指针转换，返回原始的Value对象
Value *canonicalPtr(Value *V, int maxDepth = 8);//将指针类型的Value对象转换为其规范形式，最多进行maxDepth次加载指针操作

constexpr int kInterprocMaxIters = 4;//构建指针等价类的并查集数据结构，用于分析指针之间的关系

class PointerSlotUF {//并查集数据结构，用于构建指针等价类
public:
    Value *find(Value *v);
    void unite(Value *a, Value *b);

private:
    std::unordered_map<Value *, Value *> parent;//parent[v]表示Value对象v所在集合的代表元素,v.first是Value对象，v.second是该对象所在集合的代表元素
    std::unordered_map<Value *, unsigned> rank;//rank[v]表示Value对象v所在集合的秩（近似树高），用于优化合并过程
};

PointerSlotUF buildPointerSlotUF(Function &F);//构建指针等价类的并查集数据结构，分析函数中的指针关系
Value *finalPtrSlot(Value *V, PointerSlotUF &uf);//获取指针类型的Value对象在并查集中的最终代表元素，用于分析指针的根源
Function *resolveDirectCallee(CallInst *CI);//解析直接调用的函数，返回被调用的函数对象，如果无法解析则返回nullptr
std::map<Value *, unsigned> buildPointerArgRootIndex(Function &F, PointerSlotUF &uf);//构建函数参数中指针类型的Value对象与参数索引之间的映射关系，用于分析函数参数的指针根源

void addBug(std::vector<BugRecord> &out,
            const std::string &kind,
            Function *F,
            Instruction *I,
            const std::string &detail);//添加一个bug记录到输出的bug列表中，包含bug类型、所在函数、位置和详细描述等信息

void writeBugReport(const std::string &path,
                    const std::vector<BugRecord> &bugs,
                    const std::map<std::string, long long> &passMs);//将bug报告写入指定路径的文件中，包含bug的总数、每个检查Pass的执行时间以及每个bug的详细信息等内容

class CheckPassBase {//检查Pass的基类，定义了检查Pass的接口和基本功能
public:
    virtual ~CheckPassBase();
    virtual std::string name() const = 0;
    virtual void run(ModuleMap &modules, std::vector<BugRecord> &out) = 0;
};
#endif
