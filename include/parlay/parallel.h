
#ifndef PARLAY_PARALLEL_H_
#define PARLAY_PARALLEL_H_

#include <cassert>
#include <cstdlib>

#include <algorithm>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>  // IWYU pragma: keep
#include <utility>

// ----------------------------------------------------------------------------
// All scheduler plugins are required to implement the
// following four functions to integrate with parlay:
//  - num_workers :   return the max number of workers
//  - worker_id :     return the id of the current worker
//  - parallel_for :  execute a given loop body in parallel
//  - par_do :        execute two given functions in parallel
// ----------------------------------------------------------------------------

namespace parlay {

// number of threads available from OS
inline size_t num_workers();

// id of running thread, should be numbered from [0...num-workers)
inline size_t worker_id();

// parallel loop from start (inclusive) to end (exclusive) running
// function f.
//    f should map size_t to void.
//    granularity is the number of iterations to run sequentially
//      if 0 (default) then the scheduler will decide
//    conservative uses a safer scheduler
template <typename F>
inline void parallel_for(size_t start, size_t end, F&& f, long granularity = 0,
                         bool conservative = false);

// runs the thunks left and right in parallel.
//    both left and write should map void to void
//    conservative uses a safer scheduler
template <typename Lf, typename Rf>
inline void par_do(Lf&& left, Rf&& right, bool conservative = false);

// ----------------------------------------------------------------------------
//          Extra functions implemented on top of the four basic ones
//
//  - parallel_do : alias of par_do
//  - blocked_for : parallelize a loop over a sequence of blocks
// ----------------------------------------------------------------------------

// Given two functions left and right, both invocable with no arguments,
// invokes both of them potentially in parallel.
template <typename Lf, typename Rf>
inline void parallel_do(Lf&& left, Rf&& right, bool conservative = false) {
  static_assert(std::is_invocable_v<Lf&&>);
  static_assert(std::is_invocable_v<Rf&&>);
  par_do(std::forward<Lf>(left), std::forward<Rf>(right), conservative);
}

template<typename F>
inline void blocked_for(size_t start, size_t end, size_t block_size, F&& f, bool conservative = false) {
  static_assert(std::is_invocable_v<F&, size_t, size_t, size_t>);
  assert(block_size > 0);
  if (start < end) {
    size_t n_blocks = (end - start + block_size - 1) / block_size;
    parallel_for(0, n_blocks, [&](size_t i) {
      size_t s = start + i * block_size;
      size_t e = (std::min)(s + block_size, end);
      f(i, s, e);
    }, 0, conservative);
  }
}

// ----------------------------------------------------------------------------
//                              Some more extras
//
//  - par_do_if :   execute in parallel if do_parallel is true, else sequential
//  - par_do3 :     parallelize three jobs
//  - par_do3_if :  guess from the two above
// ----------------------------------------------------------------------------

template <typename Lf, typename Rf>
static void par_do_if(bool do_parallel, Lf&& left, Rf&& right, bool cons = false) {
  if (do_parallel)
    par_do(std::forward<Lf>(left), std::forward<Rf>(right), cons);
  else {
    std::forward<Lf>(left)();
    std::forward<Rf>(right)();
  }
}

template <typename Lf, typename Mf, typename Rf>
inline void par_do3(Lf&& left, Mf&& mid, Rf&& right) {
  auto left_mid = [&]() { par_do(std::forward<Lf>(left), std::forward<Mf>(mid)); };
  par_do(left_mid, std::forward<Rf>(right));
}

template <typename Lf, typename Mf, typename Rf>
static void par_do3_if(bool do_parallel, Lf&& left, Mf&& mid, Rf&& right) {
  if (do_parallel)
    par_do3(std::forward<Lf>(left), std::forward<Mf>(mid), std::forward<Rf>(right));
  else {
    std::forward<Lf>(left)();
    std::forward<Mf>(mid)();
    std::forward<Rf>(right)();
  }
}

}  // namespace parlay

//****************************************************
//                Scheduler selection
//****************************************************

// Cilk Scheduler
#if defined(PARLAY_CILKPLUS)
#include "internal/scheduler_plugins/cilkplus.h"    // IWYU pragma: keep, export

#elif defined(PARLAY_OPENCILK)
#include "internal/scheduler_plugins/opencilk.h"    // IWYU pragma: keep, export

// OpenMP Scheduler
#elif defined(PARLAY_OPENMP)
#include "internal/scheduler_plugins/omp.h"         // IWYU pragma: keep, export

// Intel's Thread Building Blocks Scheduler
#elif defined(PARLAY_TBB)
#include "internal/scheduler_plugins/tbb.h"         // IWYU pragma: keep, export

// No Parallelism
#elif defined(PARLAY_SEQUENTIAL)
#include "internal/scheduler_plugins/sequential.h"  // IWYU pragma: keep, export

// Parlay's Homegrown Scheduler
#else

#define PARLAY_USING_PARLAY_SCHEDULER

#include "scheduler.h"

#include "internal/work_stealing_job.h"


namespace parlay {
  
namespace internal {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // 'getenv': This function or variable may be unsafe.
#endif

// Determine the number of workers to spawn
inline unsigned int init_num_workers() {
  if (const auto env_p = std::getenv("PARLAY_NUM_THREADS")) {
    return std::stoi(env_p);
  } else {
    return std::thread::hardware_concurrency();
  }
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

using scheduler_type = scheduler<WorkStealingJob>;

// Set once the scheduler a thread started for itself has been destroyed,
// which happens when the thread ends. Objects that outlive it can still reach
// parallel code: a static object of the main thread is destroyed after that
// thread's thread-local objects, and its destructor may well call
// parallel_for. Such calls run sequentially. A bool has no destructor, so it
// stays readable for as long as the thread's storage does.
inline thread_local bool local_scheduler_destroyed = false;

// The scheduler a thread starts for itself the first time it runs parallel code
struct local_scheduler_holder {
  explicit local_scheduler_holder(size_t num_workers) : scheduler(num_workers) {}
  ~local_scheduler_holder() { local_scheduler_destroyed = true; }
  scheduler_type scheduler;
};

// The scheduler the calling thread runs parallel code on, started first if
// the thread has none; null once the scheduler it started has been destroyed.
//
// A thread that already has a scheduler is answered from the same single
// thread-local read as before; the flag is only looked at when there is none.
extern inline scheduler_type* find_current_scheduler() {
  auto current_scheduler = scheduler_type::get_current_scheduler();
  if (current_scheduler != nullptr) return current_scheduler;
  if (local_scheduler_destroyed) return nullptr;
  static thread_local local_scheduler_holder local_scheduler(init_num_workers());
  return std::addressof(local_scheduler.scheduler);
}

// Requires that the thread's own scheduler has not been destroyed
extern inline scheduler_type& get_current_scheduler() {
  auto current_scheduler = find_current_scheduler();
  assert(current_scheduler != nullptr);
  return *current_scheduler;
}

}  // namespace internal

inline size_t num_workers() {
  auto scheduler = internal::find_current_scheduler();
  return scheduler ? scheduler->num_workers() : 1;
}

inline size_t worker_id() {
  auto scheduler = internal::find_current_scheduler();
  return scheduler ? scheduler->worker_id() : 0;
}

template <typename F>
inline void parallel_for(size_t start, size_t end, F&& f, long granularity, bool conservative) {
  static_assert(std::is_invocable_v<F&, size_t>);

  if (start + 1 == end) {
    f(start);
  }
  else if ((end - start) <= static_cast<size_t>(granularity)) {
    for (size_t i = start; i < end; i++) f(i);
  }
  else if (end > start) {
    if (auto scheduler = internal::find_current_scheduler()) {
      fork_join_scheduler::parfor(*scheduler, start, end,
        std::forward<F>(f), static_cast<size_t>(granularity), conservative);
    }
    else {
      for (size_t i = start; i < end; i++) f(i);
    }
  }
}

template <typename Lf, typename Rf>
inline void par_do(Lf&& left, Rf&& right, bool conservative) {
  static_assert(std::is_invocable_v<Lf&&>);
  static_assert(std::is_invocable_v<Rf&&>);
  if (auto scheduler = internal::find_current_scheduler()) {
    fork_join_scheduler::pardo(*scheduler, std::forward<Lf>(left), std::forward<Rf>(right), conservative);
  }
  else {
    std::forward<Lf>(left)();
    std::forward<Rf>(right)();
  }
}

namespace internal {

// The scheduler behind execute_shared: one for the process, started by the
// first call and owned by no thread.
//
// It is never destroyed. A static object made before the first call is
// destroyed after anything that call made, and its destructor may still call
// execute_shared; the workers have to be there for it. They end with the
// process.
extern inline scheduler_type& get_shared_scheduler() {
  static scheduler_type* const shared_scheduler =
      new scheduler_type(init_num_workers(), scheduler_type::ownerless_t{});
  return *shared_scheduler;
}

}  // namespace internal

// Execute the given function f() on the scheduler that the whole process shares,
// and return once it has finished.
//
// A thread that calls parallel_for or par_do without being a worker gets a
// scheduler of its own, with its own threads. That suits a program whose main
// thread does the parallel work. A program that reaches parallel code from many
// threads (the threads of a server's pool, say) would end up with one scheduler
// per thread; such threads call execute_shared instead, and everything f forks
// runs on the same fixed set of workers whichever thread asked.
//
// The calling thread sleeps until f returns. Called from a thread of the shared
// scheduler, f runs on that thread, as the shared worker it is. That holds
// while the thread is working for a scheduler of its own inside
// execute_with_scheduler too: handing f over would make the thread wait for a
// scheduler it is part of, so it takes its place among the shared workers back
// for the length of the call. f must not throw.
template <typename F>
void execute_shared(F&& f) {
  auto& scheduler = internal::get_shared_scheduler();
  if (scheduler.started_this_thread()) {
    scheduler.run_as_own_worker(std::forward<F>(f));
  }
  else {
    scheduler.run_from_outside(std::forward<F>(f));
  }
}

// Execute the given function f() on p threads inside its own private scheduler instance
//
// The scheduler instance is destroyed upon completion and can not be re-used. Creating a
// scheduler is expensive, so it's not a good idea to use this for very cheap functions f.
template <typename F>
void execute_with_scheduler(unsigned int p, F&& f) {
  internal::scheduler_type scheduler(p);
  std::invoke(std::forward<F>(f));
}

}  // namespace parlay

#endif

#if !defined(PARLAY_USING_PARLAY_SCHEDULER)

#include <functional>
#include <utility>

namespace parlay {

// The other schedulers already serve every thread from one pool, so there is
// nothing to share that is not shared: f runs where it is called.
template <typename F>
void execute_shared(F&& f) {
  std::invoke(std::forward<F>(f));
}

}  // namespace parlay

#endif

#endif  // PARLAY_PARALELL_H_
