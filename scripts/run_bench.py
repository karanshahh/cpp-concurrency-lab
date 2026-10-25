#!/usr/bin/env python3
"""run_bench.py <label>

Builds the Release tree, runs the benchmarks, and saves everything needed to reproduce the run
(machine, kernel, CPU, governor, compiler, flags, git commit, command lines, raw output)
to results/<label>-<timestamp>.txt. Each benchmark repeats its measurement 3 times internally
and prints ranges across the runs.

Examples:
  python3 scripts/run_bench.py vm-v1
  BENCH_ARGS="--pin 2,3" PIPE_ARGS="--pin 2,3,4" python3 scripts/run_bench.py vm-v1

Numbers from the OrbStack VM are "VM numbers"; override MACHINE_LABEL for any other machine.
Run from the repo root.
"""
import os
import subprocess
import sys


def repo_root():
    """The repo root: the current directory if it is one, else the parent of scripts/."""
    if os.path.isfile("CMakeLists.txt"):
        return os.getcwd()
    return os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def project_python():
    """$PYTHON, else .venv/bin/python, else python3 (needs numpy and pandas)."""
    py = os.environ.get("PYTHON", "")
    if not py:
        py = ".venv/bin/python" if os.access(".venv/bin/python", os.X_OK) else "python3"
    return py


def run(cmd, **kw):
    """Run a command; stop with its exit status if it fails (like set -e)."""
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(r.returncode)
    return r


def out(cmd, default=""):
    """Stdout of a command, stripped; `default` if it cannot run or fails."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else default
    except OSError:
        return default

import datetime
import re
import shlex

os.chdir(repo_root())


def main(argv):
    if not argv:
        print("usage: run_bench.py <label>   e.g. vm-v1", file=sys.stderr)
        return 1
    label = argv[0]
    machine = os.environ.get("MACHINE_LABEL") or "VM numbers (OrbStack Ubuntu, aarch64, shared host)"
    bench_args = os.environ.get("BENCH_ARGS", "")
    pipe_args = os.environ.get("PIPE_ARGS", "")
    fs_args = os.environ.get("FS_ARGS", "")

    run(["cmake", "-S", ".", "-B", "build-release", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", "build-release"])

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = f"results/{label}-{stamp}.txt"
    os.makedirs("results", exist_ok=True)
    u = os.uname()
    ncpu = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else (os.cpu_count() or "unknown")
    try:
        with open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor") as g:
            governor = g.read().strip()
    except OSError:
        governor = "unavailable"
    has_fs = os.access("build-release/false_sharing", os.X_OK)

    with open(path, "w") as f:
        def line(s=""):
            print(s, flush=True)
            f.write(s + "\n")
            f.flush()

        def stream(cmd):
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors="replace")
            for chunk in p.stdout:
                print(chunk, end="", flush=True)
                f.write(chunk)
                f.flush()
            rc = p.wait()
            if rc:
                sys.exit(rc)

        line(f"# {machine}")
        line(f"label:        {label}")
        line(f"date:         {datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')} (UTC)")
        line(f"git:          {out(['git', 'describe', '--tags', '--always', '--dirty'], 'unknown')} "
             f"({out(['git', 'rev-parse', '--short', 'HEAD'], 'unknown')})")
        line(f"kernel:       {u.sysname} {u.release} {u.machine}")
        line(f"nproc:        {ncpu}")
        line(f"cpu governor: {governor}")
        lscpu = out(["lscpu"])
        if lscpu:
            line("--- lscpu (head) ---")
            for l in lscpu.splitlines()[:20]:
                line(l)
        line("--- cmake cache (compiler and flags) ---")
        rx = re.compile(r"^(CMAKE_BUILD_TYPE|CMAKE_CXX_COMPILER|CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE|SANITIZE):")
        with open("build-release/CMakeCache.txt") as c:
            for l in c.read().splitlines():
                if rx.match(l):
                    line(l)
        line("--- method ---")
        line("Release build, no sanitizers; each benchmark repeats its measurement 3 times; ranges across runs; see README 'Method'")
        line("commands:")
        line(f"  build-release/bench_queues {bench_args}")
        if has_fs:
            line(f"  build-release/false_sharing {fs_args}")
        line(f"  build-release/pipeline_demo --queue <cv|mutex|seqcst|acqrel|acqrel-unpadded|cached> {pipe_args}   (3 runs per queue)")
        line()
        line("=== bench_queues ===")
        stream(["build-release/bench_queues", *shlex.split(bench_args)])
        if has_fs:
            line()
            line("=== false_sharing ===")
            stream(["build-release/false_sharing", *shlex.split(fs_args)])
        # The pipeline measures one run per process, so run each queue 3 times.
        for q in ("cv", "mutex", "seqcst", "acqrel", "acqrel-unpadded", "cached"):
            for n in (1, 2, 3):
                line()
                line(f"=== pipeline_demo --queue {q} (run {n} of 3) ===")
                stream(["build-release/pipeline_demo", "--queue", q, *shlex.split(pipe_args)])
    print()
    print(f"saved: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
