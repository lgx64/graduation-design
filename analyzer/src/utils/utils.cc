#include "utils.h"
#include "Errors.h"
#include "toml.hpp"
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <spdlog/common.h>
#include <iomanip>


void __appendTomlArr(set<string> &set, toml::array* str_arr) {
    for (auto it = str_arr->begin(); it != str_arr->end(); it++) {
        if (it->is_string()) {
            set.insert(it->value_or(""));
        } else {
            throw GeneralException("Non-string in config");
        }
    }
}

void __appendTomlArr(vector<string> &vec, toml::array* str_arr) {
    for (auto it = str_arr->begin(); it != str_arr->end(); it++) {
        if (it->is_string()) {
            vec.push_back(it->value_or(""));
        } else {
            throw GeneralException("Non-string in config");
        }
    }
}

void __appendPairTomlArr(vector<string> &firsts,vector<string> &seconds, toml::array* str_arr) {
    for (auto it = str_arr->begin(); it != str_arr->end(); it++) {
        if (it->is_array()) {
            toml::array* pair = it->as_array();
            if (pair->end() - pair->begin() == 2) {
                firsts.push_back(pair->begin()->value_or(""));
                seconds.push_back((++pair->begin())->value_or(""));
            } else {
                throw GeneralException("Pair length is not 2");
            }
        } else {
            throw GeneralException("Non-pair in pair list");
        }
    }
}


using namespace toml;

namespace load {
    Module* loadModule(string file_path) {
        SMDiagnostic Err;
        LLVMContext *LLVMCtx = new LLVMContext();
        unique_ptr<Module> M = parseIRFile(file_path, Err, *LLVMCtx);

        if (M == NULL) {
            cerr << __FILE_NAME__ << ": error loading file '"<< file_path << "'\n";
            return nullptr;
        }
        Module* module = M.release();
        return module;
    }

    int loadModules(vector<string>& inputFilenames, ModuleMap& module_map) {

#if _OPENMP
	OP<<"Openmp enabled\n";
#else
	OP<<"Openmp is not supported\n";
#endif

        int ret = inputFilenames.size();
        LLVMContext *LLVMCtx = new LLVMContext();
        #pragma omp parallel for num_threads(18) // 24 -> 9s; 12->10s
        for (unsigned i = 0; i < inputFilenames.size(); ++i) {
            Module* module = loadModule(inputFilenames[i]);
            if (!module) { ret--; continue; }
            module_map[module->getName().str()] = module;
        }
        return ret;
    }

    int loadModules(cl::list<string>& inputFilenames, ModuleMap& module_map) {
        vector<string> vec{begin(inputFilenames), end(inputFilenames)};
        return loadModules(vec, module_map);
    }

    int loadModules(string bclistFile, ModuleMap& module_map) {
        ifstream inf(bclistFile);
        vector<string>* paths = new vector<string>();
        string buf;
        while(!inf.eof()) {
            inf >> buf;
            paths->push_back(buf);
        }
        return loadModules(*paths, module_map);
    }
}

namespace config {
    parse_result loadConfig(string path) {
        // If a specific path is provided, use it directly
        if (!path.empty()) {
            return parse_file(path);
        }
        
        // Try multiple possible config file paths
        vector<string> possible_paths = CONFIG_FILE_PATHS;
        
        for (const string& config_path : possible_paths) {
            if (std::filesystem::exists(config_path)) {
                return parse_file(config_path);
            }
        }
        
        // If no config file found, throw the original error with the first path
        return parse_file(possible_paths[0]);
    }

    // don't use
    node_view<node> getConfig(vector<string> keys, parse_result config) {
        node_view<node> value;
        for (int i = 0; i < keys.size(); i++) {
            if (i == 0) { 
                value = config[keys[i]];
            } else {
                value = value[keys[i]];
            }
        }
        return value;
    }

    DatabaseCredential get_mysql_credential() {
        parse_result config = loadConfig();
        auto mysql_config = config["databases"]["mysql"];
        DatabaseCredential ret = {
            mysql_config["host"].value_or(""),
            mysql_config["port"].value_or(""),
            mysql_config["username"].value_or(""),
            mysql_config["password"].value_or(""),
            mysql_config["dbname"].value_or(""),
        };
        return ret;
    }

    void setAllocFuncs(set<string> &allocFuncs) {
        parse_result config = loadConfig();
        toml::array* arr = config["functions"]["alloc"]["fs"].as_array();
        __appendTomlArr(allocFuncs, arr);
    }

    void setFreeFuncs(set<string> &freeFuncs) {
        parse_result config = loadConfig();
        toml::array* arr = config["functions"]["free"]["fs"].as_array();
        __appendTomlArr(freeFuncs, arr);
    }

    void setDebugFuncs(set<string> &debugFuncs) {
        parse_result config = loadConfig();
        toml::array* arr = config["functions"]["llvmDebug"]["fs"].as_array();
        __appendTomlArr(debugFuncs, arr); 
    }

    void setHeapAllocFuncs(set<string> &heapAllocFuncs) {
        parse_result config = loadConfig();
        toml::array* arr = config["functions"]["heapAllocFuncs"]["fs"].as_array();
        __appendTomlArr(heapAllocFuncs, arr); 
    }

    void setIcallIgnoreList(vector<string> &IcallIgnoreFileLoc, vector<string> &IcallIgnoreLineNum) {
        parse_result config = loadConfig();
        toml::array* arrFileLoc = config["files"]["icall-ignore-list-fileloc"]["fs"].as_array();
        __appendTomlArr(IcallIgnoreFileLoc, arrFileLoc);
        toml::array* arrLineNum = config["files"]["icall-ignore-list-linenum"]["fs"].as_array();
        __appendTomlArr(IcallIgnoreLineNum, arrLineNum);
    }
}

namespace timer{
    string boot_time = utcTime();
    map<string, long int> recorder = map<string, long int>();
    long int now() { return std::time(NULL); }
    void record(string key) { recorder[key] = now(); }
    void record(string key, long int time) { recorder[key] = time; }
    void add(string key, long int time) { recorder[key] += time; }
    long int get(string key) { return recorder[key]; }
    long int diff(string key1, string key2) { return recorder[key2] - recorder[key1]; }

    string utcTime() {
        using namespace std::chrono;
        auto tp = system_clock::now();
        auto tt = system_clock::to_time_t(tp);
        auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;

        std::tm tm = *std::localtime(&tt);
        std::ostringstream oss;
        oss << std::put_time(&tm, "%F^%T") << "."
            << std::setw(3) << std::setfill('0') << ms.count();
        return oss.str();
    }
}

namespace logging {
    string log_prefix = "";
    shared_ptr<logger> global_log = nullptr;
    shared_ptr<logger> exc_log = nullptr;
    shared_ptr<logger> stdout_log = nullptr;
    shared_ptr<logger> stderr_log = nullptr;

    static string unique_logger_name(const string &base) {
        return base + "_" + timer::utcTime() + "_" + to_string(getpid());
    }

    static string resolve_log_root() {
        if (const char *envRoot = std::getenv("ANALYZER_LOG_ROOT")) {
            if (envRoot[0] != '\0') return string(envRoot);
        }

        try {
            parse_result config = config::loadConfig();
            if (auto dir = config["logging"]["dir"].value<string>()) {
                if (!dir->empty()) return *dir;
            }
        } catch (...) {
            // keep default root
        }

        return "analyzer/logs";
    }

    void setup_logger() {
        if (global_log && exc_log && stdout_log && stderr_log) return;

        timer::boot_time = timer::utcTime();
        string log_root = resolve_log_root();
        if (!log_root.empty() && log_root.back() == '/') {
            log_root.pop_back();
        }
        log_prefix = log_root + "/" + timer::boot_time + "/";
        std::filesystem::create_directories(log_prefix);

        global_log = basic_logger_mt(unique_logger_name("global"), log_prefix + "global.log");
        exc_log = basic_logger_mt(unique_logger_name("exception"), log_prefix + "exception.log");
        stdout_log = stdout_color_mt(unique_logger_name("out"), color_mode::always);
        stderr_log = stderr_color_mt(unique_logger_name("err"), color_mode::always);

        // set_pattern("[%m/%d %T.%2!e] [%l] %v");
        global_log->set_level(level::trace);
        global_log->flush_on(level::warn);
        exc_log->set_level(level::warn);
        exc_log->flush_on(level::warn);
        stdout_log->set_level(level::info);
        stdout_log->flush_on(level::warn);
        stderr_log->set_level(level::info);
        stderr_log->flush_on(level::warn);
    }

    shared_ptr<logger> register_logger(string name, level::level_enum level, string log_file) {
        setup_logger();

        auto logger = spdlog::get(name);
        if (logger) { return logger; }

        logger = basic_logger_mt(name, 
            log_file.empty() ? log_prefix + name + ".log" : log_file);
        logger->set_level(level);
        // only flush on warn to save time
        logger->flush_on(level::warn);
        return logger;
    }
}

