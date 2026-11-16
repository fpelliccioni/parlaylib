
#ifndef PARLAY_SCHEDULER_H_
#define PARLAY_SCHEDULER_H_

#include <cassert>
#include <cstdint>
#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <chrono>         // IWYU pragma: keep
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>    // IWYU pragma: keep
#include <utility>
#include <vector>

#include "internal/work_stealing_deque.h"         // IWYU pragma: keep
#include "internal/work_stealing_job.h"

// IWYU pragma: no_include <bits/chrono.h>
// IWYU pragma: no_include <bits/this_thread_sleep.h>



// True if the scheduler should scale the number of awake workers
// proportional to the amount of work to be done. This saves CPU
// time if there is not any parallel work available, but may cause
// some startup lag when more parallelism becomes available.
//
// Default: true
#ifndef PARLAY_ELASTIC_PARALLELISM
#define PARLAY_ELASTIC_PARALLELISM true
#endif


// PARLAY_ELASTIC_STEAL_TIMEOUT sets the number of microseconds
// that a worker will attempt to steal jobs, such that if no
// jobs are successfully stolen, it will go to sleep.
//
// Default: 10000 (10 milliseconds)
#ifndef PARLAY_ELASTIC_STEAL_TIMEOUT
#define PARLAY_ELASTIC_STEAL_TIMEOUT 10000
#endif


#if PARLAY_ELASTIC_PARALLELISM
#include "internal/atomic_wait.h"
#endif

namespace parlay {


template <typename Job>
struct scheduler {

  using worker_id_type = unsigned int;

 private:
  static_assert(std::is_invocable_r_v<void, Job&>);

  struct workerInfo {
    static constexpr worker_id_type UNINITIALIZED = std::numeric_limits<worker_id_type>::max();

    worker_id_type worker_id;
    scheduler* my_scheduler;

    workerInfo() : worker_id(UNINITIALIZED), my_scheduler(nullptr) {}
    workerInfo(std::size_t worker_id_, scheduler* s) : worker_id(worker_id_), my_scheduler(s) {}

    workerInfo& operator=(const workerInfo&) = delete;
    workerInfo(const workerInfo&) = delete;

    workerInfo& operator=(workerInfo&& w) noexcept {
      if (this != &w) {
        worker_id = std::exchange(w.worker_id, UNINITIALIZED);
        my_scheduler = std::exchange(w.my_scheduler, nullptr);
      }
      return *this;
    }

    workerInfo(workerInfo&& w) noexcept { *this = std::move(w); }
  };

  // After YIELD_FACTOR * P unsuccessful steal attempts, a
  // a worker will sleep briefly for SLEEP_FACTOR * P nanoseconds
  // to give other threads a chance to work and save some cycles.
  constexpr static size_t YIELD_FACTOR = 200;
  constexpr static size_t SLEEP_FACTOR = 200;

  // The length of time that a worker must fail to steal anything
  // before it goes to sleep to save CPU time.
  constexpr static std::chrono::microseconds STEAL_TIMEOUT{PARLAY_ELASTIC_STEAL_TIMEOUT};

  static inline thread_local workerInfo worker_info{};

  // The scheduler that started the calling thread, if one did
  static inline thread_local const scheduler* started_by = nullptr;

  // The worker id it was started with
  static inline thread_local worker_id_type started_as = 0;

 public:

  const worker_id_type num_threads;

  // If the current thread is a worker of an existing scheduler, or the thread that spawned
  // a scheduler, return the most recent such scheduler.  Otherwise, returns null.
  static scheduler* get_current_scheduler() {
    return worker_info.my_scheduler;
  }

  // Tag for a scheduler that no thread owns: every worker is a thread the
  // scheduler starts, and the thread that constructs it is not one of them.
  // Work reaches such a scheduler through run_from_outside.
  struct ownerless_t { explicit ownerless_t() = default; };

  explicit scheduler(size_t num_workers)
      : num_threads(num_workers),
        num_deques(num_threads),
        num_awake_workers(num_threads),
        first_spawned_worker(1),
        parent_worker_info(std::exchange(worker_info, workerInfo{0, this})),
        deques(num_deques),
        attempts(num_deques),
        spawned_threads(),
        finished_flag(false) {
    spawn_workers();
  }

  // At least one worker: with none, nobody would ever take what is handed over.
  scheduler(size_t num_workers, ownerless_t)
      : num_threads(std::max<size_t>(1, num_workers)),
        num_deques(num_threads),
        num_awake_workers(num_threads),
        first_spawned_worker(0),
        parent_worker_info(),
        deques(num_deques),
        attempts(num_deques),
        spawned_threads(),
        finished_flag(false) {
    spawn_workers();
  }

  ~scheduler() {
    shutdown();
    if (first_spawned_worker != 0) {
      worker_info = std::move(parent_worker_info);
    }
  }

  // True if the calling thread is one of this scheduler's workers.
  bool is_worker() const noexcept {
    return worker_info.my_scheduler == this;
  }

  // True if the calling thread is one that this scheduler started. Unlike
  // is_worker, this stays true while the thread works for another scheduler:
  // execute_with_scheduler gives the thread that calls it a scheduler of its
  // own for a while.
  bool started_this_thread() const noexcept {
    return started_by == this;
  }

  // Runs f() on the calling thread as the worker of this scheduler that it is.
  // For a thread this scheduler started: if it is working for another scheduler
  // just now, it takes its own place here back for the length of the call, so
  // what f forks goes to this scheduler's workers, and returns to the other
  // scheduler afterwards. Its deque here is as it left it, with the job it is
  // in the middle of below whatever f pushes and pops. f must not throw.
  template <typename F>
  void run_as_own_worker(F&& f) {
    assert(started_this_thread());
    if (is_worker()) {
      std::forward<F>(f)();
      return;
    }
    workerInfo elsewhere = std::exchange(worker_info, workerInfo{started_as, this});
    std::forward<F>(f)();
    worker_info = std::move(elsewhere);
  }

  // Runs f() on a worker of this scheduler and returns once it has finished.
  // For threads that are not workers of this scheduler: a worker has a deque
  // to push the jobs it forks onto, and such a thread has none. f is handed to
  // the workers as one job, the first idle one takes it, and everything f forks
  // is scheduled among them as usual. The calling thread sleeps meanwhile.
  //
  // Any number of threads may call this at the same time. f must not throw.
  template <typename F>
  void run_from_outside(F&& f) {
    assert(!is_worker());
    // A thread of this scheduler that waited here would be waiting for itself
    assert(!started_this_thread());
    // The completion is signalled under a mutex that lives on this stack, and
    // not through the job's own flag: the worker must be out of every object
    // of this frame before the frame goes away, which it is once it has
    // released the mutex.
    std::mutex mutex;
    std::condition_variable finished;
    bool done = false;
    auto run = [&]() {
      std::forward<F>(f)();
      std::lock_guard<std::mutex> lock(mutex);
      done = true;
      finished.notify_one();
    };
    auto job = make_job(run);
    {
      std::lock_guard<std::mutex> lock(outside_mutex);
      outside_jobs.push_back(&job);
      num_outside_jobs.fetch_add(1);
    }
#if PARLAY_ELASTIC_PARALLELISM
    wake_up_a_worker();
#endif
    {
      std::unique_lock<std::mutex> lock(mutex);
      finished.wait(lock, [&]() { return done; });
    }
    // The job marks itself finished after f returns, which is the last thing
    // the worker does with it.
    job.wait();
  }

  // Push onto local stack.
  void spawn(Job* job) {
    int id = worker_id();
    [[maybe_unused]] bool first = deques[id].push_bottom(job);
#if PARLAY_ELASTIC_PARALLELISM
    if (first) wake_up_a_worker();
#endif
  }

  // Wait until the given condition is true.
  //
  // If conservative, this thread will simply busy wait. Otherwise,
  // it will look for work to steal and keep itself occupied. This
  // can deadlock if the stolen work wants a lock held by the code
  // that is waiting, so avoid that.
  template <typename F>
  void wait_until(F&& done, bool conservative = false) {
    // Conservative avoids deadlock if scheduler is used in conjunction
    // with user locks enclosing a wait.
    if (conservative) {
      while (!done())
        std::this_thread::yield();
    }
    // If not conservative, schedule within the wait.
    // Can deadlock if a stolen job uses same lock as encloses the wait.
    else {
      do_work_until(std::forward<F>(done));
    }
  }

  // Pop from local stack.
  Job* get_own_job() {
    auto id = worker_id();
    return deques[id].pop_bottom();
  }

  worker_id_type num_workers() { return num_threads; }
  worker_id_type worker_id() { return worker_info.worker_id; }

  bool finished() const noexcept {
    return finished_flag.load(std::memory_order_acquire);
  }

 private:
  // Align to avoid false sharing.
  struct alignas(128) attempt {
    size_t val;
  };

  int num_deques;
  std::atomic<size_t> num_awake_workers;
  // 1 when the constructing thread is worker 0, 0 when every worker is spawned
  worker_id_type first_spawned_worker;
  workerInfo parent_worker_info;
  std::vector<internal::Deque<Job>> deques;
  std::vector<attempt> attempts;
  std::vector<std::thread> spawned_threads;
  std::atomic<int> finished_flag;

  std::atomic<size_t> wake_up_counter{0};
  std::atomic<size_t> num_finished_workers{0};

  // Jobs handed over by threads that are not workers, oldest first. The count
  // is what a worker reads to know whether the queue is worth locking.
  std::mutex outside_mutex;
  std::deque<Job*> outside_jobs;
  std::atomic<size_t> num_outside_jobs{0};

  void spawn_workers() {
    for (worker_id_type i = first_spawned_worker; i < num_threads; ++i) {
      spawned_threads.emplace_back([&, i]() {
        worker_info = {i, this};
        started_by = this;
        started_as = i;
        worker();
      });
    }
  }

  // The oldest job handed over from outside, if there is one.
  Job* get_outside_job() {
    if (num_outside_jobs.load(std::memory_order_acquire) == 0) return nullptr;
    std::lock_guard<std::mutex> lock(outside_mutex);
    if (outside_jobs.empty()) return nullptr;
    Job* job = outside_jobs.front();
    outside_jobs.pop_front();
    num_outside_jobs.fetch_sub(1);
    return job;
  }

  // Start an individual worker task, stealing work if no local
  // work is available. May go to sleep if no work is available
  // for a long time, until woken up again when notified that
  // new work is available.
  void worker() {
#if PARLAY_ELASTIC_PARALLELISM
    wait_for_work();
#endif
    while (!finished()) {
      // Jobs from outside are taken here only, by a worker that has nothing
      // of its own under way: a worker waiting at a join keeps to the jobs of
      // the computation it is in.
      Job* job = get_outside_job();
      bool called_outside = false;
      if (!job) {
        job = get_job([&]() {
          called_outside = has_outside_job();
          return called_outside || finished();
        }, PARLAY_ELASTIC_PARALLELISM);
      }
      if (job)(*job)();
      // Left the search for a job from outside: look for it, do not sleep.
      else if (called_outside) continue;
#if PARLAY_ELASTIC_PARALLELISM
      else if (!finished()) {
        // If no job was stolen, the worker should go to
        // sleep and wait until more work is available
        wait_for_work();
      }
#endif
    }
    assert(finished());
    num_finished_workers.fetch_add(1);
  }

  // Runs tasks until done(), stealing work if necessary.
  //
  // Does not sleep or time out since this can be called
  // by the main thread and by join points, for which sleeping
  // would cause deadlock, and timing out could cause a join
  // point to resume execution before the job it was waiting
  // on has completed.
  template <typename F>
  void do_work_until(F&& done) {
    while (true) {
      Job* job = get_job(done, false);  // timeout MUST BE false
      if (!job) return;
      (*job)();
    }
    assert(done());
  }

  bool has_outside_job() const noexcept {
    return num_outside_jobs.load(std::memory_order_acquire) != 0;
  }

  // Find a job, first trying local stack, then random steals.
  //
  // Returns nullptr if break_early() returns true before a job
  // is found, or, if timeout is true and it takes longer than
  // STEAL_TIMEOUT to find a job to steal.
  template <typename F>
  Job* get_job(F&& break_early, bool timeout) {
    if (break_early()) return nullptr;
    Job* job = get_own_job();
    if (job) return job;
    else job = steal_job(std::forward<F>(break_early), timeout);
    return job;
  }
  
  // Find a job with random steals.
  //
  // Returns nullptr if break_early() returns true before a job
  // is found, or, if timeout is true and it takes longer than
  // STEAL_TIMEOUT to find a job to steal.
  template<typename F>
  Job* steal_job(F&& break_early, bool timeout) {
    size_t id = worker_id();
    const auto start_time = std::chrono::steady_clock::now();
    do {
      // By coupon collector's problem, this should touch all.
      for (size_t i = 0; i <= YIELD_FACTOR * num_deques; i++) {
        if (break_early()) return nullptr;
        Job* job = try_steal(id);
        if (job) return job;
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(num_deques * 100));
    } while (!timeout || std::chrono::steady_clock::now() - start_time < STEAL_TIMEOUT);
    return nullptr;
  }

  Job* try_steal(size_t id) {
    // use hashing to get "random" target
    size_t target = (hash(id) + hash(attempts[id].val)) % num_deques;
    attempts[id].val++;
    auto [job, empty] = deques[target].pop_top();
#if PARLAY_ELASTIC_PARALLELISM
    if (!empty) wake_up_a_worker();
#endif
    return job;
  }

#if PARLAY_ELASTIC_PARALLELISM

  // Wakes up at least one sleeping worker (more than one
  // worker may be woken up depending on the implementation).
  void wake_up_a_worker() {
    if (num_awake_workers.load(std::memory_order_acquire) < num_threads) {
      wake_up_counter.fetch_add(1);
      parlay::atomic_notify_one(&wake_up_counter);
    }
  }
  
  // Wake up all sleeping workers
  void wake_up_all_workers() {
    if (num_awake_workers.load(std::memory_order_acquire) < num_threads) {
      wake_up_counter.fetch_add(1);
      parlay::atomic_notify_all(&wake_up_counter);
    }
  }
  
  // Wait until notified to wake up
  void wait_for_work() {
    num_awake_workers.fetch_sub(1);
    // A job handed over from outside has nobody else to run it, so it must not
    // be missed. The counter is read before the queue is looked at: a handover
    // that is too late to be seen in the queue has not yet moved the counter,
    // and moving it ends the wait below at once.
    auto const seen = wake_up_counter.load();
    if (!has_outside_job()) {
      parlay::atomic_wait(&wake_up_counter, seen);
    }
    num_awake_workers.fetch_add(1);
  }

#endif

  size_t hash(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return static_cast<size_t>(x);
  }
  
  void shutdown() {
    finished_flag.store(true, std::memory_order_release);
#if PARLAY_ELASTIC_PARALLELISM
    // We must spam wake all workers until they finish in
    // case any of them are just about to fall asleep, since
    // they might therefore miss the flag to finish
    while (num_finished_workers.load() < spawned_threads.size()) {
      wake_up_all_workers();
      std::this_thread::yield();
    }
#endif
    for (auto& thread : spawned_threads) {
      thread.join();
    }
  }
};


class fork_join_scheduler {
  using Job = WorkStealingJob;
  using scheduler_t = scheduler<Job>;

 public:

  // Fork two thunks and wait until they both finish.
  template <typename L, typename R>
  static void pardo(scheduler_t& scheduler, L&& left, R&& right, bool conservative = false) {
    auto execute_right = [&]() { std::forward<R>(right)(); };
    auto right_job = make_job(right);
    scheduler.spawn(&right_job);
    std::forward<L>(left)();
    if (const Job* job = scheduler.get_own_job(); job != nullptr) {
      assert(job == &right_job);
      execute_right();
    }
    else {
      auto done = [&]() { return right_job.finished(); };
      scheduler.wait_until(done, conservative);
      assert(right_job.finished());
    }
  }

  template <typename F>
  static void parfor(scheduler_t& scheduler, size_t start, size_t end, F&& f, size_t granularity = 0, bool conservative = false) {
    if (end <= start) return;
    if (granularity == 0) {
      size_t done = get_granularity(start, end, f);
      granularity = std::max(done, (end - start) / static_cast<size_t>(128 * scheduler.num_threads));
      start += done;
    }
    parfor_(scheduler, start, end, f, granularity, conservative);
  }

 private:
  template <typename F>
  static size_t get_granularity(size_t start, size_t end, F& f) {
    size_t done = 0;
    size_t sz = 1;
    unsigned long long int ticks = 0;
    do {
      sz = std::min(sz, end - (start + done));
      auto tstart = std::chrono::steady_clock::now();
      for (size_t i = 0; i < sz; i++) f(start + done + i);
      auto tstop = std::chrono::steady_clock::now();
      ticks = static_cast<unsigned long long int>(std::chrono::duration_cast<
                std::chrono::nanoseconds>(tstop - tstart).count());
      done += sz;
      sz *= 2;
    } while (ticks < 1000 && done < (end - start));
    return done;
  }

  template <typename F>
  static void parfor_(scheduler_t& scheduler, size_t start, size_t end, F& f, size_t granularity, bool conservative) {
    if ((end - start) <= granularity)
      for (size_t i = start; i < end; i++) f(i);
    else {
      size_t n = end - start;
      // Not in middle to avoid clashes on set-associative caches on powers of 2.
      size_t mid = (start + (9 * (n + 1)) / 16);
      pardo(scheduler,
            [&]() { parfor_(scheduler, start, mid, f, granularity, conservative); },
            [&]() { parfor_(scheduler, mid, end, f, granularity, conservative); },
            conservative);
    }
  }

};

}  // namespace parlay

#endif  // PARLAY_SCHEDULER_H_
