# Memory-order justification (v2)

Every atomic operation in the SPSC queues, with the reason for its order. You must be able to defend each line
out loud; that is the point of the project.

## Vocabulary

A **release** store makes every write that came before it in this thread visible to any thread that later does an
**acquire** load that *reads the value that release store wrote* ("synchronizes-with"). **relaxed** gives atomicity
only: no ordering of other memory. **seq_cst** adds one global total order over all seq_cst operations; this algorithm
does not need it.

## SpscQueueAcqRel

| Operation | Order | Why it is enough (and not weaker) |
|---|---|---|
| producer: `tail_.load` | relaxed | Only the producer ever writes `tail_`, so it reads its own previous value. Nothing to synchronize with. |
| producer: `head_.load` | acquire | Pairs with the consumer's release store of `head_`. Seeing a new `head_` proves the consumer has *finished reading* the slot we are about to overwrite. With relaxed, overwriting that slot would race with the consumer's read (a data race, which is undefined behaviour). |
| producer: `buf_[t & mask_] = v` | plain | Protected by the two orders around it; no atomic needed. |
| producer: `tail_.store(t+1)` | release | **Publishes** the slot write above: any consumer that acquires this value sees the slot contents. With relaxed the consumer could see the new tail but read a stale or half-written slot. |
| consumer: `head_.load` | relaxed | Only the consumer writes `head_`. |
| consumer: `tail_.load` | acquire | Pairs with the producer's release store of `tail_`; makes the slot contents visible. |
| consumer: `out = buf_[h & mask_]` | plain | Ordered after the acquire above. |
| consumer: `head_.store(h+1)` | release | The hand-off in the other direction: "I am done reading this slot, you may reuse it." Pairs with the producer's acquire of `head_`. |

Two synchronizes-with edges exist: **tail** (producer to consumer: "data is ready") and **head** (consumer to producer:
"slot is free"). That is the whole algorithm.

`scripts/mutation_check.sh` weakens each of these orders in a copy of the code, one at a time, and requires the
ThreadSanitizer build of the tests to fail. A memory-order change that no test notices would mean the justification above
is untested.

## SpscQueueSeqCst

The same code with every operation left at the default `seq_cst`. Correct, because seq_cst is stronger than
acquire/release. It buys nothing here: the two indices are independent channels, so nobody needs a single global order
across them.

**Hypothesis to test (not a result):** on x86 a seq_cst store compiles to `xchg` or a fence (more expensive) while a release
store is a plain `mov`, so seq_cst should be slower there. On AArch64, seq_cst and acquire/release loads and stores
both compile to `ldar` / `stlr`, so the aarch64 VM may show little or no difference. The x86 run is where it could appear.
Only what is measured is claimed.

## SpscQueueCached

The same orders as `SpscQueueAcqRel`; what differs is *when* the shared index is read.

| Operation | Order | Why |
|---|---|---|
| producer: `tail.load` | relaxed | Only the producer writes `tail`. |
| producer: `head_cache` read/write | plain (non-atomic) | Producer-private variable; no other thread touches it. |
| producer: `cons_.head.load` (refresh only) | acquire | Same pairing as in `SpscQueueAcqRel`: seeing the new head proves the consumer finished reading the freed slots. |
| producer: `tail.store(t+1)` | release | Publishes the slot write. |
| consumer: `head.load` | relaxed | Only the consumer writes `head`. |
| consumer: `tail_cache` read/write | plain | Consumer-private. |
| consumer: `prod_.tail.load` (refresh only) | acquire | Makes the slot contents up to that tail visible. |
| consumer: `head.store(h+1)` | release | "Slot is free." |

**Why a stale cache is safe.** `head` and `tail` only increase. A stale `head_cache` is at most the real head, so it
can only make the producer think there is *less* free space than there is: it refreshes needlessly, but never overwrites a
slot still in use. A stale `tail_cache` is at most the real tail, and every slot below it was published by the release
store of the value we acquired when we cached it, so those slots are visible.

**What it is meant to buy (hypothesis, not a result):** fewer reads of a cache line that the other core keeps modifying.
Whether that shows up on a given machine is for the benchmark to say.

## Padding (false sharing)

`head_` is written by the consumer and `tail_` by the producer. If they share a cache line, each write by one core
invalidates that line in the other core's cache even though they touch different variables. Padded means each index sits
alone on its own `kCacheLine` (128 bytes on aarch64, 64 elsewhere). `SpscQueueAcqRel<T, false>` is the unpadded variant,
kept for the experiment; `bench/false_sharing.cpp` shows the same effect without a queue.
