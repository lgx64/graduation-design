<<<<<<< HEAD
# graduation-design 项目使用指南（新环境从零上手）

本项目是一个基于 LLVM 的 C 代码静态分析器，当前支持检测：

- `NullPointerDeref`（空指针解引用）
- `UseAfterFree`（释放后使用）
- `DoubleFree`（重复释放）
- `IntegerOverflow`（整数溢出影响 size/index）
- `BufferOverflow`（缓冲区越界）

并提供两种入口：

1. 命令行批量分析：`batch_diagnose.py`
2. Web 前端分析：`webapp.py`（支持单文件与 zip 工程上传、行级高亮）

---

## 1) 新环境准备

建议 Linux 环境（Ubuntu 20.04/22.04）。

请先安装：

- `git`
- `cmake`（建议 >= 3.5）
- `make`
- `g++`（支持 C++17）
- `python3`（建议 3.10+）
- `clang`（建议 clang/LLVM 15Init）

> 注意：如果系统没有 `python` 命令，只用 `python3` 即可。

---

## 2) 拉取代码

```bash
git clone <your-repo-url>
cd graduation-design
```

---

## 3) 配置 Python 运行环境（推荐）

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -U pip
pip install -r requirements.txt
```

如果你看到 `python: command not found`，改用：

```bash
python3 webapp.py
```

或直接：

```bash
./.venv/bin/python webapp.py
```

---

## 4) 构建分析器

```bash
cd analyzer
make -j"$(nproc)"
cd ..
```

构建成功后应存在：

- `analyzer/build/academic_checker`

---

## 5) 命令行方式运行（批量）

项目默认从 `inputfile/` 读取 `.c` 文件并分析：

```bash
python3 batch_diagnose.py
```

输出目录：

- `diagnose/<时间戳>/`

常见输出文件：

- `global.log`
- `Bug_Report_*.txt`

可选参数示例：

```bash
python3 batch_diagnose.py \
  --input-dir ./inputfile \
  --diagnose-dir ./diagnose \
  --checker ./analyzer/build/academic_checker \
  --clang /usr/bin/clang
```

---

## 6) Web 前端方式运行（推荐演示）

启动服务：

```bash
python3 webapp.py
```

打开浏览器：

- `http://127.0.0.1:8000`

支持能力：

- 上传单个 `.c` 文件
- 粘贴源码直接分析
- 上传 zip 工程（递归分析其中 `.c`）
- 按文件切换查看结果
- 按 bug 类型筛选
- 行级高亮问题位置

---

## 7) 常见问题排查

### Q1: 运行 `python webapp.py` 报错（Exit Code 127）

原因：系统没有 `python` 命令别名。

解决：

```bash
python3 webapp.py
```

### Q2: 提示找不到 `clang`

先检查：

```bash
which clang
```

若不在默认路径，设置：

```bash
export CLANG_PATH=/path/to/clang
```

### Q3: 提示找不到 `analyzer/build/academic_checker`

说明分析器未构建成功，重新执行：

```bash
cd analyzer
make -j"$(nproc)"
cd ..
```

并检查：

```bash
ls -l analyzer/build/academic_checker
```

### Q4: Web 页面打开了但没有分析结果

请按顺序检查：

1. `clang` 可用
2. `academic_checker` 存在
3. 上传内容确实是 C 文件（或 zip 内含 `.c`）

---

## 8) 目录说明

- `analyzer/`：分析器源码与构建产物
- `inputfile/`：示例输入 C 文件
- `diagnose/`：批量分析输出
- `batch_diagnose.py`：命令行批处理入口
- `webapp.py`：Web 服务入口
- `webui/`：前端页面和静态资源

---

## 9) 一键式最小流程（复制可用）

```bash
git clone <your-repo-url>
cd graduation-design

python3 -m venv .venv
source .venv/bin/activate
pip install -U pip
pip install -r requirements.txt

cd analyzer
make -j"$(nproc)"
cd ..

python3 batch_diagnose.py
python3 webapp.py
```

然后访问：`http://127.0.0.1:8000`
=======
# graduation-design
My first static analyzer based on LLVM
>>>>>>> b6f4abd0834dc7ad11df7e9cc31dba6ff4820bd3
