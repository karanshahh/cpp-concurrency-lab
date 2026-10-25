#!/usr/bin/env python3
"""mutation_check.py - "break one thing on purpose; the tests must fail".

For each mutation: copy the repo to a temp dir, change ONE thing there (your files are never touched),
build with a sanitizer, run the tests, and report whether they caught it.
  caught   = at least one test failed (good)
  SURVIVED = all tests passed although the code is broken (a gap in the tests)
Exit code 1 if any mutation survived; 2 if no mutation matched the filter (nothing was run).
Optional argument: only run mutations whose name contains it, e.g.  python3 scripts/mutation_check.py "cached"
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

import shutil
import tempfile

os.chdir(repo_root())

# (name, mode, file, exact text to replace, replacement); mode asan or tsan.
# Memory-order mutations need TSan: on x86 the tests cannot see them otherwise.
MUTATIONS = [
    ('mutex-queue: no full check', 'asan', 'include/cclab/mutex_queue.hpp',
     'if (count_ == buf_.size()) return false;  // full',
     'if (false) return false;  // full'),
    ('blocking-queue: pop skips draining after close', 'asan', 'include/cclab/bounded_queue.hpp',
     'if (count_ == 0) return std::nullopt;  // closed and drained',
     'if (closed_) return std::nullopt;  // closed and drained'),
    ('thread-pool: shutdown drops queued tasks', 'asan', 'include/cclab/thread_pool.hpp',
     'if (tasks_.empty()) return;  // stopping_ and nothing left: exit',
     'if (stopping_) return;  // stopping_ and nothing left: exit'),
    ('latency-recorder: percentile rank rounds down', 'asan', 'include/cclab/latency_recorder.hpp',
     'std::uint64_t rank = (n * pm + 999) / 1000;  // ceil(n * pm / 1000)',
     'std::uint64_t rank = (n * pm) / 1000;  // ceil(n * pm / 1000)'),
    ('pipeline: book forgets to update the VWAP volume', 'asan', 'include/cclab/pipeline.hpp',
     'e.qty = e.price - vwap_num_ / vol_;  // deviation from the running VWAP',
     'e.qty = e.price - vwap_num_ / (vol_ + 1);  // deviation from the running VWAP'),
    ('spsc: no full check (seq_cst and acq/rel)', 'asan', 'include/cclab/spsc_queue.hpp',
     'if (t - h == buf_.size()) return false;  // full',
     'if (false) return false;  // full'),
    ('spsc acq/rel: producer loads head with relaxed instead of acquire', 'tsan', 'include/cclab/spsc_queue.hpp',
     'const std::size_t h = head_.v.load(std::memory_order_acquire);',
     'const std::size_t h = head_.v.load(std::memory_order_relaxed);'),
    ('spsc acq/rel: producer publishes tail with relaxed instead of release', 'tsan', 'include/cclab/spsc_queue.hpp',
     'tail_.v.store(t + 1, std::memory_order_release);',
     'tail_.v.store(t + 1, std::memory_order_relaxed);'),
    ('spsc acq/rel: consumer frees the slot with relaxed instead of release', 'tsan', 'include/cclab/spsc_queue.hpp',
     'head_.v.store(h + 1, std::memory_order_release);\n    return true;\n  }\n\n  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }\n\n private:\n  const std::size_t mask_;\n  std::vector<T> buf_;\n  detail::IndexCell<Padded> head_;',
     'head_.v.store(h + 1, std::memory_order_relaxed);\n    return true;\n  }\n\n  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }\n\n private:\n  const std::size_t mask_;\n  std::vector<T> buf_;\n  detail::IndexCell<Padded> head_;'),
    ('spsc cached: producer never refreshes its cached head', 'asan', 'include/cclab/spsc_queue.hpp',
     'prod_.head_cache = cons_.head.load(std::memory_order_acquire);  // refresh (shared line, rare)',
     'prod_.head_cache = prod_.head_cache;  // refresh (shared line, rare)'),
    ('spsc cached: consumer never refreshes its cached tail', 'asan', 'include/cclab/spsc_queue.hpp',
     'cons_.tail_cache = prod_.tail.load(std::memory_order_acquire);  // refresh (shared line, rare)',
     'cons_.tail_cache = cons_.tail_cache;  // refresh (shared line, rare)'),
]


def copy_repo(dst):
    def ignore(directory, names):
        if os.path.abspath(directory) == os.path.abspath("."):
            return [n for n in names if n.startswith("build") or n in (".git", "results")]
        return []
    shutil.copytree(".", dst, ignore=ignore, dirs_exist_ok=True)


def quiet(cmd, log):
    with open(log, "w") as f:
        return subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode


def main(argv):
    root = os.getcwd()
    flt = argv[0] if argv else ""
    survived = 0
    ran = 0
    for name, mode, file, old, new in MUTATIONS:
        if flt and flt not in name:
            continue
        ran += 1
        san = "thread" if mode == "tsan" else "address,undefined"
        work = tempfile.mkdtemp()
        copy_repo(work)
        path = os.path.join(work, file)
        with open(path) as f:
            content = f.read()
        if old not in content:
            print(f"ERROR     {name}: text to mutate not found in {file}", flush=True)
            shutil.rmtree(work)
            survived = 1
            continue
        with open(path, "w") as f:
            f.write(content.replace(old, new))
        extra = []
        gt = os.path.join(root, "build-asan/_deps/googletest-src")
        if os.path.isdir(gt):
            extra.append(f"-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST={gt}")
        quiet(["cmake", "-S", work, "-B", f"{work}/build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug",
               f"-DSANITIZE={san}", "-DCCLAB_TEST_TIMEOUT=20", *extra], f"{work}/configure.log")
        if quiet(["cmake", "--build", f"{work}/build"], f"{work}/build.log") != 0:
            print(f"ERROR     {name}: mutated code did not build (see design of the mutation)", flush=True)
            shutil.rmtree(work)
            survived = 1
            continue
        if quiet(["ctest", "--test-dir", f"{work}/build", "--timeout", "120"], f"{work}/test.log") == 0:
            print(f"SURVIVED  {name}  (all tests passed on broken code)", flush=True)
            survived = 1
        else:
            print(f"caught    {name}", flush=True)
        shutil.rmtree(work)
    if ran == 0:
        print("No mutation matched the filter; nothing was run.")
        return 2
    if survived:
        print("At least one mutation survived or errored: fix the tests (or the mutation).")
        return 1
    print("All mutations were caught.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
