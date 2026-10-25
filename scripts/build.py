#!/usr/bin/env python3
"""build.py [release|asan|tsan|all]
  release : optimised build, builds tools + benchmarks, runs the tests in Release (build-release/)
  asan    : Debug + AddressSanitizer + UBSan, builds and runs the tests (build-asan/)
  tsan    : Debug + ThreadSanitizer, builds and runs the tests (build-tsan/)
  all     : asan, then tsan, then release
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


os.chdir(repo_root())


def build(name, *cmake_args):
    run(["cmake", "-S", ".", "-B", f"build-{name}", "-G", "Ninja", *cmake_args])
    run(["cmake", "--build", f"build-{name}"])
    run(["ctest", "--test-dir", f"build-{name}", "--output-on-failure"])


def do_release():
    build("release", "-DCMAKE_BUILD_TYPE=Release")


def do_asan():
    build("asan", "-DCMAKE_BUILD_TYPE=Debug", "-DSANITIZE=address,undefined")


def do_tsan():
    build("tsan", "-DCMAKE_BUILD_TYPE=Debug", "-DSANITIZE=thread")


def main(argv):
    what = argv[0] if argv else "release"
    if what == "release":
        do_release()
    elif what == "asan":
        do_asan()
    elif what == "tsan":
        do_tsan()
    elif what == "all":
        do_asan()
        do_tsan()
        do_release()
    else:
        print(f"usage: {sys.argv[0]} [release|asan|tsan|all]", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
