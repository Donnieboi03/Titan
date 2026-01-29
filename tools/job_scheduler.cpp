#include "job_scheduler.h"

namespace scheduler
{
    JobScheduler::JobScheduler(std::size_t num_workers, std::size_t batch_capacity)
    : num_workers_(num_workers), batch_capacity_(batch_capacity)
    {
        job_queues_.reserve(num_workers_);
        finished_last_job_ = std::make_unique<std::atomic<bool>[]>(num_workers_);
        for (std::size_t i = 0; i < num_workers_; ++i)
            finished_last_job_[i].store(true, std::memory_order_relaxed);

        // Construct queue object
        for (std::size_t i = 0; i < num_workers_; ++i)
            job_queues_.emplace_back(batch_capacity_);

        // Start workers
        for (WorkerId i = 0; i < num_workers_; ++i)
            workers_.emplace_back([this, i]() { worker_loop(i); });
    }

    JobScheduler::~JobScheduler()
    {
        execute_batch(); // Execute all pending jobs
        wait_for_completion(); // Wait for jobs workers to finish
        running_.store(false, std::memory_order_seq_cst); // Stop loop

        // Join all workers
        for (auto& worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    WorkerId JobScheduler::submit_job(Job&& job) noexcept
    {
        const WorkerId worker_id = job.owner_id % num_workers_;
        auto& buffer = job_queues_[worker_id];
        // push current job; if push fails, flush batches then retry until successful
        while (!buffer.try_emplace(std::forward<Job>(job)))
        {
            execute_batch();
            std::this_thread::yield();
        }

        return worker_id;
    }

    void JobScheduler::submit_job_on(WorkerId worker_id, Job&& job) noexcept
    {
        auto& buffer = job_queues_[worker_id];

        // push current job; if push fails, flush this worker's batch then retry until successful
        while (!buffer.try_emplace(std::forward<Job>(job)))
        {
            execute_batch_on(worker_id);
            std::this_thread::yield();
        }
    }

    void JobScheduler::process_jobs() noexcept
    {
        execute_batch();
        wait_for_completion();
    }

    void JobScheduler::process_jobs_on(WorkerId worker_id) noexcept
    {
        execute_batch_on(worker_id);
        wait_for_completion_on(worker_id);
    }

    bool JobScheduler::process_jobs_async() noexcept { return execute_batch(); }
    bool JobScheduler::process_jobs_on_async(WorkerId worker_id) noexcept { return execute_batch_on(worker_id); }

    bool JobScheduler::is_complete() noexcept {
        // Complete when there are no pending writes and all workers reported finished
        bool queues_finished = all_queues_finished();

        if (queues_finished)
        {
            // If not complete try to batch; yield only if no progress
            if (!execute_batch())
                std::this_thread::yield();
        }

        return queues_finished;
    }

    bool JobScheduler::is_complete_on(WorkerId worker_id) noexcept
    {
        bool worker_empty = job_queues_[worker_id].empty();
        bool worker_finished = finished_last_job_[worker_id].load(std::memory_order_acquire);

        if (worker_empty && worker_finished)
        {
            // If not complete try to batch; yield only if no progress
            if (!execute_batch())
                std::this_thread::yield();
        }

        return worker_empty && worker_finished;
    }

    std::size_t JobScheduler::pending_jobs() const noexcept {
        std::size_t total = 0;
        for (const auto& q : job_queues_)
            total += q.pending_writes() + q.pending_reads();
        return total;
    }

    std::size_t JobScheduler::pending_jobs_on(WorkerId worker_id) const noexcept {
        if (worker_id >= num_workers_) return 0;
        return job_queues_[worker_id].pending_writes() + job_queues_[worker_id].pending_reads();
    }

    bool JobScheduler::is_full_on(WorkerId worker_id) const noexcept { return job_queues_[worker_id].full(); }
    bool JobScheduler::is_empty_on(WorkerId worker_id) const noexcept { return job_queues_[worker_id].empty(); }
    std::size_t JobScheduler::get_worker_count() const noexcept { return num_workers_; }
    std::size_t JobScheduler::get_batch_capacity() const noexcept { return batch_capacity_; }

    void JobScheduler::worker_loop(std::size_t worker_id)
    {
        Job job;
        // Event Loop for Worker
        while (running_.load(std::memory_order_acquire))
        {
            // If pop fails then yield and retry 
            if (!job_queues_[worker_id].try_pop(job))
            {
                // If pop fails due no pending reads, worker is idle/done
                if (!job_queues_[worker_id].pending_reads())
                    finished_last_job_[worker_id].store(true, std::memory_order_release);
                std::this_thread::yield();
                continue;
            }

            // Execute Job
            if (job.invoke)
                job.invoke(job.storage);
        }
    }

    bool JobScheduler::execute_batch() noexcept
    {
        for (std::size_t i = 0; i < num_workers_; ++i)
            finished_last_job_[i].store(false, std::memory_order_release);

            bool all_flushed = true;
            for (auto& buffer : job_queues_)
                all_flushed = buffer.try_flush() && all_flushed;

            return all_flushed;
    }

    bool JobScheduler::execute_batch_on(WorkerId worker_id) noexcept
    {
        finished_last_job_[worker_id].store(false, std::memory_order_relaxed);
        return job_queues_[worker_id].try_flush();
    }

    void JobScheduler::wait_for_completion() noexcept
    {
        while (true)
        {
            if (all_queues_finished()) break;

            // Keep trying to flush pending writes while waiting
            for (auto& buffer : job_queues_)
                buffer.try_flush();
            std::this_thread::yield();
        }
    }

    void JobScheduler::wait_for_completion_on(WorkerId worker_id) noexcept
    {
        while (job_queues_[worker_id].empty() && !finished_last_job_[worker_id].load(std::memory_order_acquire))
        {
            // Keep trying to flush pending writes while waiting
            job_queues_[worker_id].try_flush();
            std::this_thread::yield();
        }
    }

    bool JobScheduler::all_queues_finished() const noexcept
    {
        return std::all_of(job_queues_.begin(), job_queues_.end(), 
        [](const JobQueue& b){ return b.empty(); }) &&
        std::all_of(finished_last_job_.get(), finished_last_job_.get() + num_workers_, 
        [](const std::atomic<bool>& a){ return a.load(std::memory_order_acquire);});
    }
}
