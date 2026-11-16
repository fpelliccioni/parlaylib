#include "gtest/gtest.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <thread>
#include <vector>

#include <parlay/parallel.h>

namespace {

// Counts the threads that ever ran a piece of the work. Thread ids cannot tell,
// since the id of a joined thread is given to the next one.
//
// The mark is a plain bool on purpose. A thread-local object with a
// constructor would be built, with some compilers, as soon as the thread
// touches any other thread-local of this file, the scheduler's own included,
// which every thread that calls execute_shared does: it would count callers
// that ran nothing.
std::atomic<std::size_t> threads_seen{0};
thread_local bool counted_this_thread = false;

void count_this_thread() {
  if (!counted_this_thread) {
    counted_this_thread = true;
    threads_seen.fetch_add(1);
  }
}

// The sum of 0..n-1, each term added by whichever worker runs it.
std::size_t parallel_sum(std::size_t n) {
  std::atomic<std::size_t> sum{0};
  parlay::parallel_for(0, n, [&](std::size_t i) {
    count_this_thread();
    sum.fetch_add(i, std::memory_order_relaxed);
  }, 1);
  return sum.load();
}

constexpr std::size_t expected_sum(std::size_t n) { return n * (n - 1) / 2; }

// Made before the first call to execute_shared, so destroyed after whatever
// that call makes, and still calling it then. A wrong answer cannot be reported
// through the test framework that late; it ends the process instead.
struct CallsExecuteSharedAtExit {
  ~CallsExecuteSharedAtExit() {
    std::size_t result = 0;
    parlay::execute_shared([&]() { result = parallel_sum(1000); });
    if (result != expected_sum(1000)) std::abort();
  }
};
const CallsExecuteSharedAtExit calls_execute_shared_at_exit;

}  // namespace

TEST(TestExecuteShared, RunsTheFunctionAndWaitsForIt) {
  std::size_t result = 0;
  parlay::execute_shared([&]() { result = parallel_sum(10000); });
  ASSERT_EQ(result, expected_sum(10000));
}

TEST(TestExecuteShared, StillRunsWhileStaticObjectsAreDestroyed) {
  // The check is the destructor of calls_execute_shared_at_exit, after main.
  std::size_t result = 0;
  parlay::execute_shared([&]() { result = parallel_sum(1000); });
  ASSERT_EQ(result, expected_sum(1000));
}

// The rest is about the workers of Parlay's own scheduler. With the other back
// ends execute_shared calls the function where it is.
#if defined(PARLAY_USING_PARLAY_SCHEDULER)

TEST(TestExecuteShared, RunsOnAWorkerNotOnTheCaller) {
  const auto caller = std::this_thread::get_id();
  std::thread::id ran_on;
  parlay::execute_shared([&]() { ran_on = std::this_thread::get_id(); });
  ASSERT_NE(ran_on, caller);
}

TEST(TestExecuteShared, ThreadsFromOutsideShareOneSetOfWorkers) {
  const std::size_t outside_threads = 16;
  const std::size_t calls = 40;
  const std::size_t n = 2000;
  std::size_t workers = 0;
  parlay::execute_shared([&]() { workers = parlay::num_workers(); });
  ASSERT_GT(workers, 0u);

  threads_seen.store(0);
  std::atomic<std::size_t> wrong{0};
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < outside_threads; ++t) {
    threads.emplace_back([&]() {
      for (std::size_t c = 0; c < calls; ++c) {
        std::size_t result = 0;
        parlay::execute_shared([&]() { result = parallel_sum(n); });
        if (result != expected_sum(n)) wrong.fetch_add(1);
      }
    });
  }
  for (auto& thread : threads) thread.join();
  ASSERT_EQ(wrong.load(), 0u);
  // The work ran on the shared workers and on nothing else: not on the calling
  // threads, and not on a scheduler per calling thread.
  ASSERT_LE(threads_seen.load(), workers);
}

#endif  // defined(PARLAY_USING_PARLAY_SCHEDULER)

TEST(TestExecuteShared, RunsInPlaceWhenCalledFromAWorker) {
  std::thread::id outer, inner;
  std::size_t result = 0;
  parlay::execute_shared([&]() {
    outer = std::this_thread::get_id();
    parlay::execute_shared([&]() {
      inner = std::this_thread::get_id();
      result = parallel_sum(5000);
    });
  });
  ASSERT_EQ(outer, inner);
  ASSERT_EQ(result, expected_sum(5000));
}

TEST(TestExecuteShared, WakesWorkersThatWentToSleep) {
  // Longer apart than the time a worker looks for work before it sleeps, so
  // each call finds the workers asleep and has to wake one.
  for (int call = 0; call < 12; ++call) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    std::size_t result = 0;
    parlay::execute_shared([&]() { result = parallel_sum(3000); });
    ASSERT_EQ(result, expected_sum(3000));
  }
}

TEST(TestExecuteShared, ManyShortCallsFromManyThreads) {
  const std::size_t outside_threads = 8;
  const std::size_t calls = 2000;
  std::atomic<std::size_t> ran{0};
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < outside_threads; ++t) {
    threads.emplace_back([&]() {
      for (std::size_t c = 0; c < calls; ++c) {
        parlay::execute_shared([&]() { ran.fetch_add(1); });
      }
    });
  }
  for (auto& thread : threads) thread.join();
  ASSERT_EQ(ran.load(), outside_threads * calls);
}

#if defined(PARLAY_USING_PARLAY_SCHEDULER)

TEST(TestExecuteShared, RunsAsASharedWorkerFromAWorkerThatHasASchedulerOfItsOwn) {
  // execute_with_scheduler gives the worker another scheduler for a while. A
  // call to execute_shared from inside it must not be handed back to the shared
  // workers: with one worker that is the thread now waiting. It runs on the
  // same thread, among the shared workers again, and the thread is back with
  // its own scheduler afterwards.
  const std::size_t own_workers = 2;
  std::size_t shared_workers = 0;
  parlay::execute_shared([&]() { shared_workers = parlay::num_workers(); });

  std::thread::id outer, inner;
  std::size_t workers_inside = 0, workers_before = 0, workers_after = 0, result = 0;
  parlay::execute_shared([&]() {
    outer = std::this_thread::get_id();
    parlay::execute_with_scheduler(own_workers, [&]() {
      workers_before = parlay::num_workers();
      parlay::execute_shared([&]() {
        inner = std::this_thread::get_id();
        workers_inside = parlay::num_workers();
        result = parallel_sum(5000);
      });
      workers_after = parlay::num_workers();
    });
  });
  ASSERT_EQ(outer, inner);
  ASSERT_EQ(result, expected_sum(5000));
  ASSERT_EQ(workers_before, own_workers);
  ASSERT_EQ(workers_inside, shared_workers);
  ASSERT_EQ(workers_after, own_workers);
}

TEST(TestExecuteShared, AnOwnerlessSchedulerHasAtLeastOneWorker) {
  using scheduler_type = parlay::internal::scheduler_type;
  scheduler_type scheduler(0, scheduler_type::ownerless_t{});
  ASSERT_EQ(scheduler.num_workers(), 1u);
  bool ran = false;
  scheduler.run_from_outside([&]() { ran = true; });
  ASSERT_TRUE(ran);
}

#endif  // defined(PARLAY_USING_PARLAY_SCHEDULER)
