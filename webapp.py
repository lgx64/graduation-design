#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any

from flask import Flask, jsonify, render_template, request

REPO_ROOT = Path(__file__).resolve().parent
CHECKER_PATH = REPO_ROOT / "analyzer" / "build" / "academic_checker"
DEFAULT_CLANG_CANDIDATES = (
    Path("/usr/local/bin/clang"),
    Path("/usr/bin/clang"),
)
RUNS_DIR = REPO_ROOT / ".web_runs"
RUNS_DIR.mkdir(parents=True, exist_ok=True)
MAX_UPLOAD_SIZE = 20 * 1024 * 1024
REPORT_LINE_RE = re.compile(
    r"^\[(?P<pass_name>[^\]]+)\]\s+Function=(?P<function>.*?)\s+\|\s+"
    r"Loc=(?P<source_path>.*?):(?P<line>\d+)\s+\|\s+"
    r"(?P<message>.*?)\s+Inst=\s*(?P<inst>.*)$"
)

app = Flask(__name__, template_folder="webui/templates", static_folder="webui/static")
app.config["JSON_AS_ASCII"] = False
app.config["MAX_CONTENT_LENGTH"] = MAX_UPLOAD_SIZE

def resolve_clang() -> str:
    env_clang = os.environ.get("CLANG_PATH")
    if env_clang:
        return env_clang
    for candidate in DEFAULT_CLANG_CANDIDATES:
        if candidate.exists():
            return str(candidate)
    discovered = shutil.which("clang")
    if discovered:
        return discovered
    raise FileNotFoundError("未找到 clang，请设置环境变量 CLANG_PATH。")

def run_cmd(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> tuple[int, str]:
    completed = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return completed.returncode, completed.stdout
def sanitize_filename(filename: str) -> str:
    cleaned = Path(filename or "snippet.c").name
    suffix = Path(cleaned).suffix
    if suffix not in {".c", ".zip"}:
        cleaned = f"{cleaned}.c"
    return re.sub(r"[^A-Za-z0-9_.-]", "_", cleaned)

def ensure_safe_zip(zip_path: Path) -> None:
    with zipfile.ZipFile(zip_path) as archive:
        for member in archive.infolist():
            member_path = Path(member.filename)
            if member_path.is_absolute() or ".." in member_path.parts:
                raise ValueError("zip 中包含不安全路径，请重新打包后再上传。")

def normalize_source_path(raw_source_path: str, project_root: Path) -> str:
    raw_path = Path(raw_source_path)
    try:
        return str(raw_path.resolve().relative_to(project_root.resolve())).replace("\\", "/")
    except Exception:
        if raw_path.is_absolute():
            return raw_path.name
    return str(raw_path).replace("\\", "/")

def parse_report(report_text: str, project_root: Path) -> list[dict[str, Any]]:
    diagnostics: list[dict[str, Any]] = []
    for raw_line in report_text.splitlines():
        line = raw_line.strip()
        match = REPORT_LINE_RE.match(line)
        if not match:
            continue
        diagnostics.append(
            {
                "pass_name": match.group("pass_name"),
                "function": match.group("function"),
                "source_path": normalize_source_path(match.group("source_path"), project_root),
                "line": int(match.group("line")),
                "message": match.group("message"),
                "inst": match.group("inst"),
            }
        )
    return diagnostics

def read_checker_report(checker_log_root: Path) -> tuple[str, str]:
    created_log_dirs = sorted(
        [path for path in checker_log_root.iterdir() if path.is_dir()],
        key=lambda path: path.stat().st_mtime,
    )
    if not created_log_dirs:
        return "", ""
    latest = created_log_dirs[-1]
    report_path = latest / "Bug_Report.txt"
    report_text = report_path.read_text(encoding="utf-8", errors="ignore") if report_path.exists() else ""
    return report_text, str(latest)

def compile_to_bitcode(source_path: Path, project_root: Path, clang: str, output_dir: Path) -> tuple[bool, str, Path | None]:
    relative = source_path.relative_to(project_root)
    bc_path = output_dir / relative.with_suffix(".bc")
    bc_path.parent.mkdir(parents=True, exist_ok=True)
    compile_cmd = [
        clang,
        "-emit-llvm",
        "-c",
        "-g",
        "-O0",
        "-I",
        str(project_root),
        str(source_path),
        "-o",
        str(bc_path),
    ]
    compile_rc, compile_output = run_cmd(compile_cmd, cwd=project_root)
    return compile_rc == 0, compile_output, bc_path if compile_rc == 0 else None

def analyze_bitcode(bc_path: Path, checker_log_root: Path) -> tuple[bool, str, str]:
    checker_env = os.environ.copy()
    checker_env["ANALYZER_LOG_ROOT"] = str(checker_log_root)
    run_rc, run_output = run_cmd([str(CHECKER_PATH), str(bc_path)], cwd=REPO_ROOT, env=checker_env)
    report_text, _ = read_checker_report(checker_log_root)
    return run_rc == 0, run_output, report_text

def build_file_entry(
    *,
    relative_path: str,
    source_code: str,
    diagnostics: list[dict[str, Any]],
    compile_output: str,
    run_output: str,
    report_text: str,
    status: str,
    error: str | None = None,
) -> dict[str, Any]:
    return {
        "path": relative_path,
        "source": source_code,
        "diagnostics": diagnostics,
        "compile_output": compile_output,
        "run_output": run_output,
        "report_text": report_text,
        "status": status,
        "error": error,
        "bug_count": len(diagnostics),
    }

def build_project_payload(files: list[dict[str, Any]], mode: str) -> dict[str, Any]:
    all_diagnostics: list[dict[str, Any]] = []
    pass_counter: Counter[str] = Counter()
    analyzed_files = 0
    failed_files = 0

    for file_entry in files:
        if file_entry["status"] == "ok":
            analyzed_files += 1
        else:
            failed_files += 1
        for diagnostic in file_entry["diagnostics"]:
            all_diagnostics.append(diagnostic)
            pass_counter[diagnostic["pass_name"]] += 1

    return {
        "ok": True,
        "mode": mode,
        "files": sorted(files, key=lambda item: item["path"]),
        "diagnostics": all_diagnostics,
        "summary": {
            "bug_count": len(all_diagnostics),
            "file_count": len(files),
            "analyzed_files": analyzed_files,
            "failed_files": failed_files,
            "generated_at": datetime.now().isoformat(timespec="seconds"),
            "pass_counts": dict(sorted(pass_counter.items())),
        },
    }

def analyze_single_source(source_code: str, filename: str) -> dict[str, Any]:
    safe_name = sanitize_filename(filename)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    with tempfile.TemporaryDirectory(prefix=f"web_run_{timestamp}_", dir=RUNS_DIR) as tmp_dir:
        tmp_root = Path(tmp_dir)
        project_root = tmp_root / "project"
        project_root.mkdir(parents=True, exist_ok=True)
        source_path = project_root / safe_name
        source_path.write_text(source_code, encoding="utf-8")
        return analyze_project_tree(project_root, mode="single")

def analyze_project_tree(project_root: Path, mode: str) -> dict[str, Any]:
    if not CHECKER_PATH.exists():
        return {"ok": False, "error": f"未找到分析器: {CHECKER_PATH}"}

    try:
        clang = resolve_clang()
    except FileNotFoundError as exc:
        return {"ok": False, "error": str(exc)}

    c_files = sorted(project_root.rglob("*.c"))
    if not c_files:
        return {"ok": False, "error": "未在上传内容中找到任何 .c 文件。"}

    bc_root = project_root / "_bc"
    bc_root.mkdir(parents=True, exist_ok=True)
    files: list[dict[str, Any]] = []

    for source_path in c_files:
        relative_path = str(source_path.relative_to(project_root)).replace("\\", "/")
        source_code = source_path.read_text(encoding="utf-8", errors="ignore")
        checker_log_root = project_root / "_checker_logs" / relative_path.replace("/", "__")
        checker_log_root.mkdir(parents=True, exist_ok=True)

        compile_ok, compile_output, bc_path = compile_to_bitcode(source_path, project_root, clang, bc_root)
        if not compile_ok or bc_path is None:
            files.append(
                build_file_entry(
                    relative_path=relative_path,
                    source_code=source_code,
                    diagnostics=[],
                    compile_output=compile_output,
                    run_output="",
                    report_text="",
                    status="compile_error",
                    error="编译失败",
                )
            )
            continue

        run_ok, run_output, report_text = analyze_bitcode(bc_path, checker_log_root)
        if not run_ok and not report_text:
            files.append(
                build_file_entry(
                    relative_path=relative_path,
                    source_code=source_code,
                    diagnostics=[],
                    compile_output=compile_output,
                    run_output=run_output,
                    report_text="",
                    status="analyze_error",
                    error="分析器执行失败",
                )
            )
            continue

        diagnostics = [
            {**diagnostic, "file_path": diagnostic["source_path"]}
            for diagnostic in parse_report(report_text, project_root)
        ]
        files.append(
            build_file_entry(
                relative_path=relative_path,
                source_code=source_code,
                diagnostics=diagnostics,
                compile_output=compile_output,
                run_output=run_output,
                report_text=report_text,
                status="ok",
            )
        )

    return build_project_payload(files, mode)

def analyze_uploaded_zip(uploaded_name: str, uploaded_bytes: bytes) -> dict[str, Any]:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    with tempfile.TemporaryDirectory(prefix=f"web_zip_{timestamp}_", dir=RUNS_DIR) as tmp_dir:
        tmp_root = Path(tmp_dir)
        zip_path = tmp_root / sanitize_filename(Path(uploaded_name).stem + ".zip")
        zip_path.write_bytes(uploaded_bytes)
        try:
            ensure_safe_zip(zip_path)
        except ValueError as exc:
            return {"ok": False, "error": str(exc)}

        project_root = tmp_root / "project"
        project_root.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(zip_path) as archive:
            archive.extractall(project_root)

        extracted_entries = [path for path in project_root.iterdir() if path.name != "__MACOSX"]
        if len(extracted_entries) == 1 and extracted_entries[0].is_dir():
            project_root = extracted_entries[0]

        return analyze_project_tree(project_root, mode="project")

@app.get("/")
def index() -> str:
    return render_template("index.html")

@app.get("/api/health")
def health() -> Any:
    return jsonify(
        {
            "ok": True,
            "checker_exists": CHECKER_PATH.exists(),
            "checker": str(CHECKER_PATH),
        }
    )

@app.post("/api/analyze")
def analyze() -> Any:
    uploaded_file = request.files.get("file")
    uploaded_zip = request.files.get("project_zip")
    form_code = request.form.get("code", "")
    form_filename = request.form.get("filename", "snippet.c")

    if uploaded_zip and uploaded_zip.filename:
        result = analyze_uploaded_zip(uploaded_zip.filename, uploaded_zip.read())
    elif uploaded_file and uploaded_file.filename:
        source_code = uploaded_file.read().decode("utf-8", errors="ignore")
        result = analyze_single_source(source_code, uploaded_file.filename)
    else:
        if not form_code.strip():
            return jsonify({"ok": False, "error": "请先上传 C 文件、zip 工程，或粘贴源码。"}), 400
        result = analyze_single_source(form_code, form_filename)

    status = 200 if result.get("ok") else 400
    return jsonify(result), status

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000, debug=False)
