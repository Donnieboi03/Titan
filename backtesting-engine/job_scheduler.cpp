#include "../tools/double_buffer.cpp"
#include <thread>
#include <atomic>

namespace scheduler
{
    struct Job {
        using InvokeFn = void (*)(void*);

        uint32_t owner_id;
        InvokeFn invoke;
        alignas(16) std::byte storage[48]; // tuneable

        Job() : owner_id(0), invoke(nullptr) {}

        // Move-only
        Job(Job&&) = default;
        Job& operator=(Job&&) = default;
        Job(const Job&) = delete;
        Job& operator=(const Job&) = delete;
    };

    // make_job adapter for converting functions to jobs
    template <typename F>
    Job make_job(F&& f, uint32_t owner_id) {
        static_assert(sizeof(F) <= sizeof(Job::storage),
                    "Lambda capture too large for inline Job");

        Job job;
        job.owner_id = owner_id;

        new (job.storage) F(std::forward<F>(f));

        job.invoke = [](void* p) {
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
        JobScheduler(std::size_t num_workers = 1, std::size_t batch_capacity = 1048576)
        : num_workers_(num_workers), batch_capacity_(batch_capacity)
        {
            job_queues_.reserve(num_workers_);
            
            // Construct queue object
            for (int i = 0; i < num_workers_; ++i)
                job_queues_.emplace_back(batch_capacity_);
            
            // Start workers
            for (WorkerId i = 0; i < num_workers_; ++i)
                workers_.emplace_back([this, i]() { worker_loop(i); });
        }

        JobScheduler(const JobScheduler&) = delete;
        JobScheduler& operator=(const JobScheduler&) = delete;
        JobScheduler(JobScheduler&&) = delete;
        JobScheduler& operator=(JobScheduler&&) = delete;

        ~JobScheduler()
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

        WorkerId submit_job(Job&& job) noexcept
        {
            const WorkerId& worker_id = job.owner_id % num_workers_;
            auto& buffer = job_queues_[worker_id];

            // Try Loop for Push (atomic-only, no mutex overhead)
            while (!buffer.try_push(std::move(job)))
            {
                // If buffer is full try to flush
                if (!buffer.try_flush())
                    std::this_thread::yield();
            }

            return worker_id;
        }

        void submit_job_on(WorkerId worker_id, Job&& job) noexcept
        {
            auto& buffer = job_queues_[worker_id];

            // Try Loop for Push (atomic-only, no mutex overhead)
            while (!buffer.try_push(std::move(job)))
            {
                // If buffer is full try to flush
                if (!buffer.try_flush())
                    std::this_thread::yield();
            }
        }


        void process_jobs() noexcept
        {
            execute_batch();
            wait_for_completion();
        }
        
        void process_jobs_async() noexcept { execute_batch(); }

        void process_jobs_on(WorkerId worker_id) noexcept 
        { 
            execute_batch(worker_id);
            wait_for_completion(worker_id);

        }

        void process_jobs_on_async(WorkerId worker_id) noexcept
        {
            execute_batch(worker_id);
        }
        
        bool is_complete() const noexcept { return all_queues_empty(); }
        bool is_worker_complete(WorkerId worker_id) const noexcept { return job_queues_[worker_id].empty(); }
        std::size_t is_worker_full(WorkerId worker_id) const noexcept { return job_queues_[worker_id].full(); }
        
        std::size_t get_worker_count() const noexcept { return num_workers_; }
        std::size_t get_batch_capacity() const noexcept { return batch_capacity_; }
        
    private:
        void worker_loop(std::size_t worker_id)
        {
            Job job;
            // Event Loop for Worker
            while (running_.load(std::memory_order_acquire))
            {
                // If pop fails then yield and retry (atomic-only, no mutex overhead)
                if (!job_queues_[worker_id].try_pop(job))
                {
                    std::this_thread::yield();
                    continue;
                }

                // Execute Job
                if (job.invoke)
                    job.invoke(job.storage);
            }
        }

        void execute_batch() noexcept
        {
            for (auto& buffer : job_queues_)
                buffer.try_flush();
        }

        void execute_batch(WorkerId worker_id) noexcept { job_queues_[worker_id].try_flush(); }

        void wait_for_completion() noexcept
        {
            while (!all_queues_empty())
            {
                // Keep trying to flush pending writes while waiting
                for (auto& buffer : job_queues_)
                    buffer.try_flush();
                std::this_thread::yield();
            }
        }
        
        void wait_for_completion(WorkerId worker_id) noexcept
        {
            while (!job_queues_[worker_id].empty())
            {
                // Keep trying to flush pending writes while waiting
                job_queues_[worker_id].try_flush();
                std::this_thread::yield();
            }
        }

        bool all_queues_empty() const noexcept
        {
            for (const auto& buffer : job_queues_)
            {
                if (!buffer.empty())
                    return false;
            }
            return true;
        }

        std::vector<JobQueue> job_queues_;
        std::vector<std::thread> workers_;
        std::size_t num_workers_;
        std::size_t batch_capacity_;
        std::atomic<bool> running_{true};
    };
}