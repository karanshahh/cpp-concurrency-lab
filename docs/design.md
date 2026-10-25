# Design: cpp-concurrency-lab

Written at kickoff, before the code.

## What is being built, and which track

Hand-written C++ primitives for passing work between threads, each tested and compared by measurement:
a mutex queue, a condition-variable bounded queue, a thread pool, three lock-free single-producer /
single-consumer (SPSC) ring buffers, and a three-thread pipeline (feed -> book -> strategy) that uses them.

Track: **Dev** (a quant-developer project: systems that are correct, fast, and measured). It is the P9
concurrency project from the project writeups. Nothing here is a trading result.

## Repo

One repo: `cpp-concurrency-lab` (decided at kickoff: a new repo; the orderbook stays in its own repo).
Namespace and include directory: `cclab`.

## Version plan

v1 is simple and obviously correct, with standard tools. Each later version changes
something specific, and every change is measured against v1.

| Version | What it is | Why |
|---|---|---|
| v1 (baseline) | Everything built from standard tools: `std::mutex`, `std::condition_variable`, `std::thread`, `std::future`. `MutexQueue` (lock + spin), `BoundedBlockingQueue` (cv), `ThreadPool`, the pipeline on the cv queue, `LatencyRecorder`, benchmark harness, the data-race demo with its two fixes. | A correct baseline. The numbers every later version is compared with. |
| v2 | Replaces the handoff mechanism with lock-free SPSC ring buffers: `SpscQueueSeqCst`, `SpscQueueAcqRel` (padded and unpadded), `SpscQueueCached`. Adds the false-sharing experiment and the memory-order justification. The same benchmark and pipeline run on all queues. | Hypothesis: a mutex handoff costs lock contention plus kernel sleep/wake, and lock-free queues avoid both. Each sub-change (seq_cst vs acquire/release, padding, cached indices) is a separate row in the same table, so each is measured against the baseline and against its neighbour. |
| v3 | Not planned. Only if the v2 numbers show one clear remaining cost worth a change (for example batching). If added, it gets its own row in "what changed -> what it cost". | Avoids tuning without evidence. |

## Data

No external data. The pipeline's ticks come from a seeded xorshift generator (fixed seed in `Config::seed`), so every run
of the same configuration sees the same input. Queue benchmarks send counters. Nothing is fetched, nothing is committed
under `data/`. Replaying real ITCH data through these queues is left to the later capstone project.

## What will be measured, and what "done" means

Measured: Release only; machine, compiler, flags, workload, seed recorded by
`scripts/run_bench.py`; 3 runs, results reported as ranges; latency as p50 / p99 / p99.9 / max, never a mean;
throughput separately; the clock resolution reported; compared queues run the same operations; conclusions labelled
**shown** or **likely**.

- Throughput (Mops/s) and latency percentiles per queue, 1 producer / 1 consumer, paced producer for latency.
- Pipeline latency feed->book and feed->strategy per queue (v2).
- False-sharing cost: adjacent vs padded counters (v2).

Done for v1: all tests pass under ASan+UBSan, TSan and Release, CI green; mutation check catches every mutation;
results in the README; branch `v1` and tag `v1.0.0` pushed and protected
and can explain it; the repo is public and pinned after the read-through.
Done for v2: the same, plus TSan-clean stress tests for every SPSC queue, a written justification of every memory order
(`docs/MEMORY_ORDERS.md`), and the README names what did not help.

## Correctness plan

| Check | How |
|---|---|
| Reference implementation + randomized test | every queue vs a `std::deque` model under random operation sequences with three push biases |
| Differential test between versions | the typed tests run the v1 queues and (in v2) the lock-free queues through the same operation logs and the same threaded pipeline; all must equal the single-threaded reference |
| Determinism | same operation log -> same results; same pipeline seed -> same checksum, repeated runs |
| Edge cases | empty, full, capacity 1 and 2, non-power-of-two (rejected by SPSC), zero capacity, close with blocked producer/consumer, submit after shutdown, exceptions through futures, zero and one pipeline item |
| Sanitizers | all tests under ASan+UBSan, TSan, and Release |
| Mutation check | `scripts/mutation_check.py` breaks one thing at a time in a copy; the tests must fail |
| CI | GitHub Actions on x86 runners: the three builds plus a Release smoke run |

## Concepts exercised

| Concept | Where | Measured? |
|---|---|---|
| Ownership and RAII | `ThreadPool` joins its threads in the destructor; `lock_guard` / `unique_lock` everywhere | n/a |
| Move semantics, perfect forwarding | `ThreadPool::submit` (forwarding, `packaged_task` is move-only); `BoundedBlockingQueue::push(T)` moves | n/a |
| Templates and concepts | queue templates; `requires` expressions pick blocking vs spinning in `pipeline.hpp`; typed tests | n/a |
| `std::variant` / `std::expected` | not used (no message variants here; they appear in the ITCH parser project) | n/a |
| Memory layout and caches | `IndexCell` padding, `kCacheLine`, producer/consumer-side structs (v2) | v2: false-sharing experiment, padded vs unpadded queue |
| Allocation | rings and recorders preallocated; no allocation in push/pop | n/a (no allocator comparison here) |
| Threads, mutexes, condition variables | `MutexQueue`, `BoundedBlockingQueue`, `ThreadPool` | v1 baseline |
| Atomics and memory orders | `race.cpp` (relaxed atomic), SPSC queues (v2) | v2: seq_cst vs acquire/release |
| Lock-free structures | SPSC ring buffers (v2) | v2 vs v1 baseline |
| Latency measurement | `LatencyRecorder`, paced producers, scheduled-time stamps | method, not a result |

## Design decisions

- The README has a "4 Ws" section (Who / What / Where / Why) at the owner's request, in addition to the standard sections.
- Benchmarks use a small custom harness instead of Google Benchmark: the project needs cross-thread latency
  percentiles with scheduled-time stamps, which Google Benchmark does not provide. It is the same measurement
  standard.
- Header-only library (no `src/` directory): every primitive is a template or tiny inline code.
