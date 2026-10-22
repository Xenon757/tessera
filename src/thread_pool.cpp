#include "thread_pool.h"

ThreadPool::ThreadPool(int threadCount) {
    for (int t = 1; t < threadCount; ++t) workers_.emplace_back([this, t] { workerLoop(t); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    for (std::thread& w : workers_) w.join();
}

void ThreadPool::parallelFor(int count, const std::function<void(int, int)>& fn) {
    if (workers_.empty() || count <= 1) {  // nothing to share: skip the handoff cost
        for (int i = 0; i < count; ++i) fn(i, 0);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_ = &fn;
        jobCount_ = count;
        next_ = 0;
        running_ = static_cast<int>(workers_.size());
        ++generation_;
    }
    wake_.notify_all();
    runJob(0);  // the caller works too instead of just waiting

    // Wait for every worker to leave the job, so `fn` (which lives on the caller's
    // stack) is never used after we return.
    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this] { return running_ == 0; });
    job_ = nullptr;
}

void ThreadPool::runJob(int thread) {
    // fetch_add hands each index to exactly one thread, with no lock.
    for (int i = next_.fetch_add(1); i < jobCount_; i = next_.fetch_add(1)) (*job_)(i, thread);
}

void ThreadPool::workerLoop(int thread) {
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || generation_ != seen; });
            if (quit_) return;
            seen = generation_;
        }
        runJob(thread);
        std::lock_guard<std::mutex> lock(mutex_);
        if (--running_ == 0) done_.notify_one();
    }
}
