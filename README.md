# cpp-concurrency-lab

A small C++23 library of thread-handoff building blocks: a mutex queue, a condition-variable bounded queue, a thread pool,
three lock-free single-producer / single-consumer (SPSC) ring buffers, and a three-thread pipeline (feed, book, strategy) that
runs on any of them. Every piece has tests under sanitizers, and the queues are compared with one measurement method that avoids the
usual latency-benchmark mistakes. v1 is the baseline built from standard tools; v2 replaces the handoff with lock-free queues
and measures the difference.

## The 4 Ws

**Who**
* The author, a C++ developer building toward low-latency / quant developer roles, and anyone who reviews the code
  (for example an interviewer reading the repo).
* Other C++ learners who want small, tested reference implementations of a queue, a pool, a lock-free ring buffer and a pipeline.
* Not a production library.

**What**
* **Problem:** passing work from one thread to another is a core cost in low-latency systems (market data to book to strategy).
  The choice of mechanism (lock, condition variable, lock-free atomics) and its details (memory order, cache-line layout, cached
  indices) change both correctness risk and latency. The honest way to compare them is to build each one, test it, and measure
  it the same way.
* **Built:** header-only library `cclab`: `MutexQueue`, `BoundedBlockingQueue`, `ThreadPool`, `SpscQueueSeqCst`,
  `SpscQueueAcqRel` (padded or unpadded), `SpscQueueCached`, `LatencyRecorder`, and a `pipeline` that runs on any queue.
  Programs: `race` (data-race demo), `pipeline_demo`, `bench_queues`, `false_sharing`. GoogleTest suites, CMake with sanitizer
  builds, a mutation check, GitHub Actions CI.
* **Tech:** C++23, CMake + Ninja, GoogleTest 1.14, AddressSanitizer / UBSan / ThreadSanitizer, bash, GitHub Actions.
  Standard library only (`<thread>`, `<mutex>`, `<condition_variable>`, `<future>`, `<atomic>`).

**Where**
* Local Linux processes. No network, no cloud deployment.
* Developed on an Apple Silicon Mac inside an OrbStack Ubuntu VM (aarch64). Numbers from it are labelled **VM numbers**.
* CI: GitHub Actions `ubuntu-latest` (x86): tests under three builds and a Release smoke run. CI timings are never used as results.

**Why**
* Concurrency and the memory model are a standard interview topic and a real source of bugs.
* Every primitive has stated behaviour, tests that try to break it, and a mutation check that proves the tests notice when the
  code (including a memory order) is broken. Every memory order has a written justification in `docs/MEMORY_ORDERS.md`.
* The measurable value is latency percentiles and throughput per mechanism, with machine and method recorded; see Results.

## Domain primer

* **Thread handoff:** one thread produces items, another consumes them, through a queue.
* **Mutex:** a lock; only one thread holds it at a time. **Condition variable:** lets a thread sleep until another thread
  signals that a condition (queue not empty, not full) may have changed.
* **Data race:** two threads touch the same memory, at least one writes, with no synchronisation. Undefined behaviour in C++.
* **Spin:** repeatedly checking in a loop instead of sleeping. Fast to react, burns a core.
* **Lock-free SPSC queue:** a ring buffer with exactly one producer thread and one consumer thread, synchronised only with
  atomic head and tail counters, no lock.
* **Memory order:** how much ordering an atomic operation guarantees. *Release* store + *acquire* load: everything the writer
  did before the store is visible to the reader after the load. *Relaxed*: atomicity only. *seq_cst*: the strongest, one global
  order, the default.
* **Cache line:** the unit (64 or 128 bytes) in which CPU cores exchange memory. **False sharing:** two threads write different
  variables that sit on the same cache line, so the line bounces between their cores.
* **Tick:** one market-data update (a price and a quantity). **VWAP:** volume-weighted average price,
  sum(price x qty) / sum(qty). The pipeline's book stage keeps a running VWAP; it is a toy workload, chosen so the work per tick
  is tiny and the handoff cost dominates.
* **Latency percentiles:** p50 is the typical case, p99 means 1 in 100 items was slower, p99.9 means 1 in 1000. The tail is what
  hurts, so no mean is reported.
* **Coordinated omission:** if a benchmark waits for a slow item before sending the next one, it silently skips the delays the
  stall would have caused for the items that should have been sent meanwhile. Here the producer follows a fixed schedule and each
  item is stamped with its *scheduled* send time, so a stall still shows up in the latency.

## API

All in namespace `cclab`, headers under `include/cclab/`.

| Item | What it does |
|---|---|
| `MutexQueue<T>(cap)` | bounded ring behind one mutex; `try_push(const T&)`, `try_pop(T&)`; any number of producers/consumers |
| `BoundedBlockingQueue<T>(cap)` | mutex + 2 condition variables; `push(T)` blocks when full, `pop()` blocks when empty; `close()` wakes everyone; `pop()` returns `nullopt` once closed and drained; also `try_push`, `try_pop`, `size`, `closed` |
| `SpscQueueSeqCst<T>(cap)` | lock-free SPSC ring, all atomics `seq_cst`; `try_push(const T&)`, `try_pop(T&)`; capacity must be a power of two |
| `SpscQueueAcqRel<T, Padded>(cap)` | same API, relaxed/acquire/release; `Padded=false` puts head and tail on one cache line |
| `SpscQueueCached<T>(cap)` | same API, acquire/release plus a private cached copy of the other side's index |
| `ThreadPool(n)` | `submit(f, args...)` returns `std::future<R>`; task exceptions arrive through the future; `shutdown()` / destructor run all queued tasks, then join |
| `LatencyRecorder(cap)` | `record(ns)` (never allocates), `summarize()` returns min / p50 / p99 / p99.9 / max (exact, nearest rank) |
| `pipeline::run<Queue>(cfg)` | threaded feed -> book -> strategy run; returns counts, a checksum and latency summaries |
| `pipeline::reference(cfg)` | the same stages on one thread, used by tests as ground truth |
| `now_ns()`, `clock_resolution_ns()` | monotonic clock and its measured resolution |
| `cpu_relax()`, `pin_this_thread(cpu)` | spin hint; Linux thread pinning |
| `build_warning()` | non-null if the build is not valid for benchmarking (not Release, or sanitized) |

SPSC means exactly one thread calls `try_push` and exactly one thread calls `try_pop`; the type system does not enforce it.

## Versions

### v1 (baseline: simple, standard tools)

| Part | Choice | Why |
|---|---|---|
| Queue storage | preallocated ring (`std::vector`, head index + count) | no allocation while running; trivially checkable against a `std::deque` model |
| `MutexQueue` | one `std::mutex`, non-blocking `try_*`, callers spin | the simplest correct lock-based queue |
| `BoundedBlockingQueue` | one mutex, two condition variables, predicate waits, `close()` | the textbook blocking queue; predicate waits avoid lost and spurious wakeups |
| `ThreadPool` | one mutex, one condition variable, `std::deque` of tasks, `packaged_task` in a `shared_ptr` | `std::function` needs a copyable callable; `packaged_task` is move-only |
| Pipeline | 3 threads, 2 queues, sentinel tick for shutdown | no flags or extra locks needed for a clean stop |
| Latency | scheduled-time stamps, exact percentiles over stored samples | avoids coordinated omission; no histogram error |

### v2 (lock-free handoff)

| Part | Choice | Why |
|---|---|---|
| `SpscQueueSeqCst` | free-running head/tail counters, power-of-two ring, `seq_cst` everywhere, each index on its own cache line | the simplest lock-free queue to reason about |
| `SpscQueueAcqRel` | the same, with the weakest correct orders (relaxed / acquire / release) | `seq_cst` is stronger than the algorithm needs; see `docs/MEMORY_ORDERS.md` |
| unpadded variant | head and tail share a cache line | exists only so the cost of false sharing can be measured |
| `SpscQueueCached` | each side keeps a private copy of the other side's index and refreshes it only when the queue looks full/empty | fewer reads of a cache line the other core keeps writing |
| `false_sharing` | two threads, adjacent vs padded counters | the same effect without a queue around it |

What changed, and what it cost or bought (filled in only from measurements; see Results):

| Cost in the previous step | Change | Measured effect |
|---|---|---|
| a lock taken on every push and pop; producer and consumer contend for it (v1) | lock-free SPSC with atomics | -- |
| sleeping and waking through the kernel (v1 cv queue) | spinning on atomics | -- |
| `seq_cst` ordering stronger than the algorithm needs | acquire/release orders | -- |
| head and tail on one cache line | each index padded to its own line | -- |
| every call reads the other core's index | cached private copies, refreshed when full/empty | -- |

## Data

None external. The pipeline's ticks come from a seeded xorshift generator (`Config::seed`), so the same configuration always sees
the same input. Queue benchmarks send sequence numbers. Nothing is downloaded.

## How correctness is checked

| Check | What it does |
|---|---|
| Reference model | every queue (v1 and v2) vs a `std::deque` with a size cap, 30 seeds x 20,000 random operations, three push biases |
| Differential | the same operation logs and the same pipeline run on all six queues; each must equal the single-threaded reference |
| Determinism | an identical operation log gives identical results; identical pipeline config gives an identical checksum |
| Known answers | hand-computed VWAP deviations and signal counts; percentile ranks on 1000 and 1001 samples |
| Edge cases | zero capacity, capacity 1 and 2, non-power-of-two (rejected by SPSC), wrap-around, full, empty, close with a blocked producer and consumer, submit after shutdown, task exceptions, 0 and 1 pipeline items |
| Stress | 4 producers x 4 consumers on the blocking queue; 2-thread stress (capacity 1024 and 2) on every SPSC queue; fill/drain cycles that force stale cached indices |
| Sanitizers | all tests under ASan+UBSan, under TSan, and in Release |
| Mutation check | `scripts/mutation_check.sh` breaks one thing at a time in a copy of the repo (including each memory order in the SPSC queue); the tests must fail |
| CI | the same three builds on GitHub's x86 runners, plus a Release smoke run |

## Build, test, run

Inside the Linux VM:

```bash
./scripts/build.sh all          # ASan+UBSan, TSan, Release: build and run every test
./scripts/build.sh asan         # or one at a time: asan | tsan | release
./scripts/mutation_check.sh     # every mutation must be reported "caught"

./build-tsan/race race          # data race: TSan reports it
./build-tsan/race mutex         # fixed with std::mutex: TSan silent
./build-tsan/race atomic        # fixed with std::atomic: TSan silent

./build-release/pipeline_demo --queue acqrel --items 200000
./build-release/false_sharing

# reproducible benchmark run, saved to results/ (choose CPUs that exist: see `nproc`)
BENCH_ARGS="--pin 2,3" FS_ARGS="--pin 2,3" PIPE_ARGS="--pin 2,3,4" ./scripts/run_bench.sh vm-v2
```

To measure v1 as it was, use a worktree of its tag (the v1 tools have no `false_sharing`, `--queue` or SPSC rows):

```bash
git worktree add ../cpp-concurrency-lab-v1 v1.0.0
cd ../cpp-concurrency-lab-v1 && ./scripts/run_bench.sh vm-v1
cp results/vm-v1-*.txt ../cpp-concurrency-lab/results/
```

## Results

### Method

Release build (`-O3 -DNDEBUG`), no sanitizers; the programs print a warning otherwise. `scripts/run_bench.sh` records machine,
kernel, CPU, governor, compiler, flags, git commit and the exact commands in `results/`. Each benchmark repeats its
measurement 3 times and each cell is a **range across the 3 runs**, never the best run. Producer and consumer start together behind a
flag; the first messages of every latency run are discarded as warm-up. Throughput: unthrottled producer, items / wall time,
order verified. Latency: paced producer, each message stamped with its scheduled send time, latency = consumer receive time minus
that stamp; p50 / p99 / p99.9 / max only. Every queue runs the same operations, with the same pinning. The clock's measured
resolution is printed with the results; per-message times close to it are not meaningful. Pinning fixes which virtual CPU a thread
uses; it does not isolate cores from the host.

### v1 (VM numbers)

<!-- v1-results:begin -->
Machine: OrbStack Ubuntu VM on an Apple Silicon Mac (aarch64, 8 vCPUs, kernel 7.2.12-orbstack), GCC 15.2.0,
`-O3 -DNDEBUG`, no sanitizers. Clock resolution: 41 ns. Raw output: `results/vm-v1-20261025-044554.txt`.

Queues: 1 producer pinned to CPU 2, 1 consumer pinned to CPU 3, capacity 1024, 3 runs. Throughput: 2,000,000 items,
unthrottled. Latency: 500,000 items paced every 2,000 ns, first 20,000 discarded. Seed: n/a (fixed schedule).

| Queue | Throughput (Mops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|---|
| MutexQueue (lock + spin) | 4.87-5.78 | 303-351 | 13,776-215,309 | 70,994-1,624,070 | 195,146-2,090,153 |
| BoundedBlockingQueue (cv) | 4.83-5.42 | 4,752-5,423 | 48,432-55,122 | 66,833-188,918 | 219,965-830,087 |

Pipeline (`pipeline_demo`, cv queue): 1,000,000 ticks paced every 1,000 ns, first 10,000 discarded,
seed 88172645463325252, capacity 4096, feed/book/strategy pinned to CPUs 2/3/4, **one run** (not three).
Output matched the single-threaded reference (1,000,000 received, 0 order violations).

| Stage | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|
| feed->book | 6,921 | 7,005,470 | 10,748,415 | 11,249,162 |
| feed->strategy | 16,478 | 7,066,825 | 10,812,372 | 11,292,288 |
<!-- v1-results:end -->

### v2 (VM numbers)

<!-- v2-results:begin -->
Machine, compiler, flags, workload, seed, pinning: --

| Queue | Throughput (Mops/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|---|
| MutexQueue (lock + spin) | -- | -- | -- | -- | -- |
| BoundedBlockingQueue (cv) | -- | -- | -- | -- | -- |
| SpscQueueSeqCst | -- | -- | -- | -- | -- |
| SpscQueueAcqRel (padded) | -- | -- | -- | -- | -- |
| SpscQueueAcqRel (unpadded) | -- | -- | -- | -- | -- |
| SpscQueueCached | -- | -- | -- | -- | -- |

Pipeline (`pipeline_demo`, feed->strategy, ns), 3 runs per queue with identical arguments, ranges across runs:

| Queue | p50 | p99 | p99.9 | max |
|---|---|---|---|---|
| cv | -- | -- | -- | -- |
| mutex | -- | -- | -- | -- |
| seqcst | -- | -- | -- | -- |
| acqrel | -- | -- | -- | -- |
| acqrel-unpadded | -- | -- | -- | -- |
| cached | -- | -- | -- | -- |

False sharing (`false_sharing`, ms, range across runs): adjacent -- / padded --

What did not help or surprised us: --
<!-- v2-results:end -->

The v1 table above and the v2 table come from different harness versions only if v1 was measured from its own tag. To compare
v1 and v2 queues use the rows of the **same** v2 run (`MutexQueue` and `BoundedBlockingQueue` against the SPSC rows).

## What the numbers show

<!-- v1-shown:begin -->
### v1
- **Shown (measured):** both queues move about 5 million messages per second between two pinned threads
  (4.83-5.78 Mops/s across all runs). Under paced load the cv queue's median latency is about 14-18 times the mutex
  queue's (4,752-5,423 ns vs 303-351 ns). The mutex queue's tail swings widely between runs (p99.9 from 71 us to
  1.6 ms); the cv queue's p99 is steady (48-55 us). The cv pipeline delivers every tick correctly, but at one tick per
  microsecond its p99 is about 7 ms.
- **Likely (hypothesis, not yet shown):** the cv queue's median is mostly the cost of waking a sleeping consumer
  through the kernel on each item, which the spinning mutex queue avoids. The mutex queue's unstable tail comes from the
  spinning consumer and the producer competing for the lock, sometimes while the lock holder is descheduled by the host.
  The pipeline's millisecond p99 is backlog: at one tick per microsecond the cv handoff sometimes falls behind, the
  4,096-slot queues fill, and because latency counts from each tick's scheduled time, that waiting shows up (as it should).
- **To measure next:** the pipeline repeated 3 times; the pipeline at slower tick rates (2,000 and 5,000 ns) to find
  where the backlog disappears; the same workloads on lock-free SPSC queues (v2).
<!-- v1-shown:end -->

<!-- v2-shown:begin -->
### v2
- **Shown (measured):** --
- **Likely (hypothesis, not yet shown):** --
- **To measure next:** --
<!-- v2-shown:end -->

## Repository layout

```
include/cclab/     platform.hpp, latency_recorder.hpp, mutex_queue.hpp, bounded_queue.hpp, thread_pool.hpp,
                   spsc_queue.hpp, pipeline.hpp
tools/             race.cpp (data-race demo), pipeline_demo.cpp
bench/             bench_queues.cpp, false_sharing.cpp
tests/             GoogleTest suites; queue_types.hpp lists the queue types the typed tests run over
scripts/           build.sh, run_bench.sh, mutation_check.sh
docs/              design.md, MEMORY_ORDERS.md
results/           raw benchmark output (one file per run)
.github/workflows/ ci.yml
```
