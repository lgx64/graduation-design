#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from datetime import datetime


def run_cmd(cmd, cwd=None, env=None):
    p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def collect_log_dirs(log_root: Path):
    if not log_root.exists():
        return set()
    return {p.resolve() for p in log_root.iterdir() if p.is_dir()}


def find_new_log_dir(log_root: Path, before):
    after = collect_log_dirs(log_root)
    created = sorted([p for p in (after - before)], key=lambda x: x.stat().st_mtime)
    if created:
        return created[-1]
    return None


def sanitize_rel_name(rel_path: Path):
    s = str(rel_path).replace('/', '__')
    if s.endswith('.c'):
        s = s[:-2]
    return s


def main():
    repo_root = Path(__file__).resolve().parent

    parser = argparse.ArgumentParser(description='Batch compile C files to .bc and analyze per file.')
    parser.add_argument('--input-dir', default=str(repo_root / 'inputfile'))
    parser.add_argument('--diagnose-dir', default=str(repo_root / 'diagnose'))
    parser.add_argument('--checker', default=str(repo_root / 'analyzer/build/academic_checker'))
    parser.add_argument('--checker-log-root', default='',
                        help='Directory for checker internal logs (ANALYZER_LOG_ROOT). '
                             'Default: <diagnose_run_dir>/_checker_logs')
    parser.add_argument('--clang', default='/usr/local/bin/clang')
    parser.add_argument('--keep-bc', action='store_true', help='Keep generated .bc files in diagnose/bc')
    args = parser.parse_args()

    input_dir = Path(args.input_dir).resolve()
    diagnose_root = Path(args.diagnose_dir).resolve()
    checker = Path(args.checker).resolve()
    clang = args.clang

    if not input_dir.exists():
        print(f'[ERROR] input dir not found: {input_dir}')
        return 1
    if not checker.exists():
        print(f'[ERROR] checker not found: {checker}')
        return 1

    c_files = sorted(input_dir.rglob('*.c'))
    if not c_files:
        print(f'[ERROR] no .c file found in {input_dir}')
        return 1

    run_tag = datetime.now().strftime('%Y-%m-%d^%H:%M:%S')
    diagnose_dir = diagnose_root / run_tag
    diagnose_dir.mkdir(parents=True, exist_ok=True)

    bc_dir = diagnose_dir / 'bc'
    bc_dir.mkdir(parents=True, exist_ok=True)

    if args.checker_log_root:
        checker_log_root = Path(args.checker_log_root)
        if not checker_log_root.is_absolute():
            checker_log_root = repo_root / checker_log_root
        checker_log_root = checker_log_root.resolve()
    else:
        checker_log_root = (diagnose_dir / '_checker_logs').resolve()
    checker_log_root.mkdir(parents=True, exist_ok=True)

    checker_env = os.environ.copy()
    checker_env['ANALYZER_LOG_ROOT'] = str(checker_log_root)

    global_log = diagnose_dir / 'global.log'
    with open(global_log, 'w', encoding='utf-8') as gf:
        gf.write(f'Batch diagnose started at {datetime.now().isoformat()}\n')
        gf.write(f'Checker log root: {checker_log_root}\n')

    log_root = checker_log_root

    ok_count = 0
    fail_count = 0

    for src in c_files:
        rel = src.relative_to(input_dir)
        key = sanitize_rel_name(rel)
        out_bc = bc_dir / f'{key}.bc'
        out_bc.parent.mkdir(parents=True, exist_ok=True)
        report_path = diagnose_dir / f'Bug_Report_{key}.txt'

        with open(global_log, 'a', encoding='utf-8') as gf:
            gf.write('\n' + '=' * 80 + '\n')
            gf.write(f'[FILE] {rel}\n')

        cc, out = run_cmd([clang, '-emit-llvm', '-c', '-g', '-O0', str(src), '-o', str(out_bc)])
        with open(global_log, 'a', encoding='utf-8') as gf:
            gf.write('[CLANG]\n')
            gf.write(out)
            gf.write('\n')

        if cc != 0:
            fail_count += 1
            with open(global_log, 'a', encoding='utf-8') as gf:
                gf.write(f'[RESULT] compile failed for {rel}\n')
            continue

        before = collect_log_dirs(log_root)
        rc, run_out = run_cmd([str(checker), str(out_bc)], cwd=str(repo_root), env=checker_env)
        with open(global_log, 'a', encoding='utf-8') as gf:
            gf.write('[CHECKER STDOUT/STDERR]\n')
            gf.write(run_out)
            gf.write('\n')

        log_dir = find_new_log_dir(log_root, before)
        if rc != 0 or log_dir is None:
            fail_count += 1
            with open(global_log, 'a', encoding='utf-8') as gf:
                gf.write(f'[RESULT] checker failed for {rel}, rc={rc}\n')
            continue

        src_bug_report = log_dir / 'Bug_Report.txt'
        src_global_log = log_dir / 'global.log'

        if src_bug_report.exists():
            shutil.copyfile(src_bug_report, report_path)
        else:
            with open(report_path, 'w', encoding='utf-8') as rf:
                rf.write(f'No Bug_Report produced for {rel}\n')

        if src_global_log.exists():
            with open(global_log, 'a', encoding='utf-8') as gf:
                gf.write('[CHECKER GLOBAL LOG]\n')
                gf.write(src_global_log.read_text(encoding='utf-8', errors='ignore'))
                gf.write('\n')

        with open(global_log, 'a', encoding='utf-8') as gf:
            gf.write(f'[RESULT] done: {rel}, report={report_path.name}\n')

        ok_count += 1

    if not args.keep_bc:
        shutil.rmtree(bc_dir, ignore_errors=True)

    print(f'[DONE] success={ok_count}, failed={fail_count}')
    print(f'[OUTPUT] reports in: {diagnose_dir}')
    print(f'[OUTPUT] global log: {global_log}')
    return 0 if fail_count == 0 else 2


if __name__ == '__main__':
    sys.exit(main())
