// thread_pool.h — a fixed set of worker threads that run "parallel for" jobs.
//
// Threads are started once and reused: creating OS threads costs tens of
// microseconds each, which would eat a real share of a ~5 ms frame if done per frame.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool {
public:
    // `threadCount` includes the calling thread, so this starts threadCount - 1 workers.
    explicit ThreadPool(int threadCount);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int size() const { return static_cast<int>(workers_.size()) + 1; }

    // Runs fn(index, thread) for every index in [0, count) and returns when all are
    // done. `thread` (0 = the caller) says which thread ran it, for per-thread data.
    // Threads grab the next index from a shared counter, so faster cores simply
    // take more of the work.
    void parallelFor(int count, const std::function<void(int index, int thread)>& fn);

private:
    void workerLoop(int thread);
    void runJob(int thread);

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;  // signals workers: new job, or quit
    std::condition_variable done_;  // signals the caller: last worker finished

    // The current job. Written under mutex_ before workers are woken.
    const std::function<void(int, int)>* job_ = nullptr;
    int jobCount_ = 0;
    std::atomic<int> next_{0};  // next index to hand out
    int running_ = 0;           // workers still inside the current job
    uint64_t generation_ = 0;   // bumped per job, so a worker can tell a new job from a spurious wakeup
    bool quit_ = false;
};
