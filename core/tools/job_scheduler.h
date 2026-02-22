#pragma once

#include "double_buffer.h"

namespace scheduler
{
    struct Job 
    {
        using InvokeFn = void (*)(void*);

        unsigned char storage[48]; // tuneable
        InvokeFn invoke;
        uint32_t owner_id;

        Job() : owner_id(0), invoke(nullptr) {}

        // Move-only
        Job(Job&&) = default;
        Job& operator=(Job&&) = default;
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
    };

    // make_job adapter for converting functions to jobs
    template <typename F>
    Job make_job(F&& f, uint32_t owner_id) 
    {
        Job job;
        job.owner_id = owner_id;

        new (job.storage) F(std::forward<F>(f));

        job.invoke = [](void* p) 
        {
            auto* fn = static_cast<F*>(p);
            (*fn)();
            fn->~F();
        };

        return job;
    }

    using JobQueue = DoubleBuffer<Job>;
    using WorkerId = std::size_t;

    class JobScheduler
    {
    public:
        JobScheduler(std::size_t num_workers = 1, std::size_t batch_capacity = 1048576);

        JobScheduler(const JobScheduler&) = delete;
        JobScheduler& operator=(const JobScheduler&) = delete;
        JobScheduler(JobScheduler&&) = delete;
        JobScheduler& operator=(JobScheduler&&) = delete;

        ~JobScheduler();

        WorkerId submit_job(Job&& job) noexcept;

        void submit_job_on(WorkerId worker_id, Job&& job) noexcept;

        void process_jobs() noexcept;
        void process_jobs_on(WorkerId worker_id) noexcept;

        void process_jobs_async() noexcept;
        void process_jobs_on_async(WorkerId worker_id) noexcept;

        bool is_complete() const noexcept;
        bool is_complete_on(WorkerId worker_id) const noexcept;

        std::size_t pending_jobs() const noexcept;
        std::size_t pending_jobs_on(WorkerId worker_id) const noexcept;

        bool is_full_on(WorkerId worker_id) const noexcept;
        bool is_empty_on(WorkerId worker_id) const noexcept;
        std::size_t get_worker_count() const noexcept;
        std::size_t get_batch_capacity() const noexcept;

    private:
        void worker_loop(std::size_t worker_id);

        void execute_batch() noexcept;
        void execute_batch_on(WorkerId worker_id) noexcept;

        void wait_for_completion() noexcept;
        void wait_for_completion_on(WorkerId worker_id) noexcept;

        bool all_queues_finished() const noexcept;

        struct alignas(engine::CACHE_LINE) AlignedAtomicBool 
        { 
            std::atomic<bool> v; 
            AlignedAtomicBool() noexcept : v{true} {} 
        };

        const std::size_t batch_capacity_; // Capacity of Batch
        const std::size_t num_workers_; // Number of Workers
        std::vector<std::thread> workers_; // Worker Threads

        // Producer / Consumer atomic's
        alignas(engine::CACHE_LINE) std::atomic<bool> running_{true};
        std::unique_ptr<AlignedAtomicBool[]> finished_last_job_;

        std::vector<JobQueue> job_queues_; // Queue for jobs
    };

    
    inline JobScheduler::JobScheduler(std::size_t num_workers, std::size_t batch_capacity)
    : num_workers_(num_workers), batch_capacity_(batch_capacity)
    {
        
        // allocate finished flags at runtime
        finished_last_job_ = std::make_unique<AlignedAtomicBool[]>(num_workers_);
        
        // Construct queue object
        for (std::size_t i = 0; i < num_workers_; ++i)
            job_queues_.emplace_back(batch_capacity_);

        // Start workers
        for (WorkerId i = 0; i < num_workers_; ++i)
            workers_.emplace_back([this, i]() { worker_loop(i); });
    }

    
    inline JobScheduler::~JobScheduler()
    {
        execute_batch(); // Execute all pending jobs
        wait_for_completion(); // Wait for jobs workers to finish
        running_.store(false, std::memory_order_release); // Stop loop

        // Join all workers
        for (auto& worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    
    inline WorkerId JobScheduler::submit_job(Job&& job) noexcept
    {
        const WorkerId worker_id = job.owner_id % num_workers_;
        // push current job; if push fails, flush batches then retry until successful
        while (!job_queues_[worker_id].try_emplace(std::forward<Job>(job)))
        {
            execute_batch();
            std::this_thread::yield();
        }

        return worker_id;
    }

    
    inline void JobScheduler::submit_job_on(WorkerId worker_id, Job&& job) noexcept
    {
        // push current job; if push fails, flush this worker's batch then retry until successful
        while (!job_queues_[worker_id].try_emplace(std::forward<Job>(job)))
        {
            execute_batch_on(worker_id);
            std::this_thread::yield();
        }
    }

    
    inline void JobScheduler::process_jobs() noexcept
    {
        execute_batch();
        wait_for_completion();
    }

    
    inline void JobScheduler::process_jobs_on(WorkerId worker_id) noexcept
    {
        execute_batch_on(worker_id);
        wait_for_completion_on(worker_id);
    }

    
    inline void JobScheduler::process_jobs_async() noexcept { execute_batch(); }
    
    inline void JobScheduler::process_jobs_on_async(WorkerId worker_id) noexcept { execute_batch_on(worker_id); }

    
    inline bool JobScheduler::is_complete() const noexcept { return all_queues_finished(); }

    
    inline bool JobScheduler::is_complete_on(WorkerId worker_id) const noexcept
    {
        return job_queues_[worker_id].empty() && finished_last_job_[worker_id].v.load(std::memory_order_acquire);
    }

    
    inline std::size_t JobScheduler::pending_jobs() const noexcept 
    {
        std::size_t total = 0;
        for (const auto& q : job_queues_)
            total += q.pending_writes() + q.pending_reads();
        return total;
    }

    
    inline std::size_t JobScheduler::pending_jobs_on(WorkerId worker_id) const noexcept 
    {
        if (worker_id >= num_workers_) return 0;
        return job_queues_[worker_id].pending_writes() + job_queues_[worker_id].pending_reads();
    }

    
    inline bool JobScheduler::is_full_on(WorkerId worker_id) const noexcept { return job_queues_[worker_id].full(); }
    
    inline bool JobScheduler::is_empty_on(WorkerId worker_id) const noexcept { return job_queues_[worker_id].empty(); }
    
    inline std::size_t JobScheduler::get_worker_count() const noexcept { return num_workers_; }
    
    inline std::size_t JobScheduler::get_batch_capacity() const noexcept { return batch_capacity_; }

    
    inline void JobScheduler::worker_loop(std::size_t worker_id)
    {
        Job job;
        auto& queue = job_queues_[worker_id];
        auto& finished_flag = finished_last_job_[worker_id].v;

        while (running_.load(std::memory_order_acquire))
        {
            // 1. HOT PATH: Try to get a job
            if (queue.try_pop(job)) 
            {
                if (job.invoke) job.invoke(job.storage);
                continue; // Go back and try to pop again immediately
            }

            // 2. COLD PATH: Queue is empty
            finished_flag.store(true, std::memory_order_release);
            std::this_thread::yield();
        }
    }

    
    inline void JobScheduler::execute_batch() noexcept
    {
        
        // Notify All Workers
        for (std::size_t i = 0; i < num_workers_; ++i)
        {
            if (job_queues_[i].pending_writes() > 0)
            {
                job_queues_[i].try_flush();
                finished_last_job_[i].v.store(false, std::memory_order_release);
            }
        }

    }

    
    inline void JobScheduler::execute_batch_on(WorkerId worker_id) noexcept
    {
        if (job_queues_[worker_id].pending_writes() > 0)
        {
            job_queues_[worker_id].try_flush();
            finished_last_job_[worker_id].v.store(false, std::memory_order_release);
        }
    }

    
    inline void JobScheduler::wait_for_completion() noexcept
    {
        while (!all_queues_finished())
        {
            execute_batch();
            std::this_thread::yield();
        }
    }

    
    inline void JobScheduler::wait_for_completion_on(WorkerId worker_id) noexcept
    {
        while (!job_queues_[worker_id].empty() || !finished_last_job_[worker_id].v.load(std::memory_order_acquire))
        {
            execute_batch_on(worker_id);
            std::this_thread::yield();
        }
    }

    
    inline bool JobScheduler::all_queues_finished() const noexcept
    {
        for (std::size_t i = 0; i < num_workers_; ++i)
        {
            if (!job_queues_[i].empty()) return false;
            if (!finished_last_job_[i].v.load(std::memory_order_acquire)) return false;
        }
        return true;
    }
}
