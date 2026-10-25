#pragma once
// The lists of queue types that the typed tests run over. Adding a queue to a list below makes
// every typed test cover it.

#include <gtest/gtest.h>

#include <cstdint>

#include "cclab/bounded_queue.hpp"
#include "cclab/mutex_queue.hpp"
#include "cclab/pipeline.hpp"
#include "cclab/spsc_queue.hpp"

// Queues with a capacity constructor and non-blocking try_push/try_pop, carrying uint64_t.
// (v1 queues first, then the v2 lock-free queues: the same operation logs must give the same results.)
using AllTryQueues =
    ::testing::Types<cclab::MutexQueue<std::uint64_t>, cclab::BoundedBlockingQueue<std::uint64_t>,
                     cclab::SpscQueueSeqCst<std::uint64_t>, cclab::SpscQueueAcqRel<std::uint64_t, true>,
                     cclab::SpscQueueAcqRel<std::uint64_t, false>, cclab::SpscQueueCached<std::uint64_t>>;

// Queues that can carry pipeline events.
using PipelineQueues =
    ::testing::Types<cclab::MutexQueue<cclab::pipeline::Event>, cclab::BoundedBlockingQueue<cclab::pipeline::Event>,
                     cclab::SpscQueueSeqCst<cclab::pipeline::Event>, cclab::SpscQueueAcqRel<cclab::pipeline::Event, true>,
                     cclab::SpscQueueAcqRel<cclab::pipeline::Event, false>, cclab::SpscQueueCached<cclab::pipeline::Event>>;
