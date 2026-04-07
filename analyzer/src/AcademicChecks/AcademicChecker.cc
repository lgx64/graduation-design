#include "Core/CheckCommon.h"
#include "NullPointerDeref/NullPointerDerefPass.h"
#include "UAF/UAFPass.h"
#include "DoubleFree/DoubleFreePass.h"
#include "IntegerOverflow/IntegerOverflowPass.h"
#include "BufferOverflow/BufferOverflowPass.h"
#include <chrono>

using namespace llvm;
using namespace std;

cl::list<std::string> InputFilenames(
    cl::Positional, cl::OneOrMore, cl::desc("<input bitcode files>"));//记录输入文件列表

int main(int argc, char **argv) {
    sys::PrintStackTraceOnErrorSignal(argv[0]);
    PrettyStackTraceProgram X(argc, argv);
    logging::setup_logger();
    //前期准备工作：设置日志、解析命令行参数、加载模块、设置函数列表等
    cl::ParseCommandLineOptions(argc, argv, "academic multi-pass defect checker\n");//解析命令行参数，生成帮助信息

    ModuleMap modules;
    int loaded = load::loadModules(InputFilenames, modules);//使用utils中的loadModules函数加载输入的bitcode文件，并将结果存储在modules中
    GLOG->info("Total {0} files and {1} are loaded", InputFilenames.size(), loaded);

    set<string> allocFuncs;
    set<string> freeFuncs;
    config::setAllocFuncs(allocFuncs);//setAllocFuncs函数将预定义的内存分配函数名称添加到allocFuncs集合中
    config::setFreeFuncs(freeFuncs);//setFreeFuncs函数将预定义的内存释放函数名称添加到freeFuncs集合中

    vector<unique_ptr<CheckPassBase>> passes;
    passes.emplace_back(make_unique<NullPointerDerefPass>());//使用emplace_back将不同的检查Pass对象添加到passes向量
    passes.emplace_back(make_unique<UAFPass>(freeFuncs));
    passes.emplace_back(make_unique<DoubleFreePass>(freeFuncs));
    passes.emplace_back(make_unique<IntegerOverflowPass>(allocFuncs));
    passes.emplace_back(make_unique<BufferOverflowPass>());

    vector<BugRecord> bugs;//记录bug有关信息
    map<string, long long> passMs;//记录每个检查Pass的执行时间

    for (auto &pass : passes) {//遍历每个检查Pass并记录执行时间
        auto t0 = chrono::steady_clock::now();
        pass->run(modules, bugs);
        auto t1 = chrono::steady_clock::now();
        passMs[pass->name()] = chrono::duration_cast<chrono::milliseconds>(t1 - t0).count();//duration_cast返回一个milliseconds 对象，然后对它调用 .count()
    }

    string bugReportPath = logging::log_prefix + "Bug_Report.txt";//生成bug报告的路径，命名规则为logs/boot_time/Bug_Report.txt，其中boot_time是程序启动时的UTC时间
    writeBugReport(bugReportPath, bugs, passMs);

    GLOG->info("Academic checker finished. Total bugs: {0}", bugs.size());
    for (auto &p : passMs) {
        GLOG->info("Pass {0}: {1} ms", p.first, p.second);
    }
    GLOG->info("Bug report written to: {0}", bugReportPath);

    return 0;
}
