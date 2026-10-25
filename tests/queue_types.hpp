#pragma once
// The lists of queue types that the typed tests run over. Adding a queue to a list below makes
// every typed test cover it.

#include <gtest/gtest.h>

#include <cstdint>

#include "cclab/bounded_queue.hpp"
#include "cclab/mutex_queue.hpp"
#include "cclab/pipeline.hpp"

// Queues with a capacity constructor and non-blocking try_push/try_pop, carrying uint64_t.
using AllTryQueues = ::testing::Types<cclab::MutexQueue<std::uint64_t>, cclab::BoundedBlockingQueue<std::uint64_t>>;

// Queues that can carry pipeline events.
using PipelineQueues = ::testing::Types<cclab::MutexQueue<cclab::pipeline::Event>, cclab::BoundedBlockingQueue<cclab::pipeline::Event>>;
