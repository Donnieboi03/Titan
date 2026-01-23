#ifndef JOB_SCHEDULER_H
#define JOB_SCHEDULER_H

#include <cstdint>
#include <cstddef>

// Forward declaration
template<typename T>
class DoubleBuffer;

namespace scheduler
{
    // Job structure - type-erased callable with inline storage
    struct Job {
        using InvokeFn = void (*)(void*);

        uint32_t owner_id;
        InvokeFn invoke;
        alignas(16) std::byte storage[48]; // Inline storage for lambda captures

        Job();
        
        // Move-only
        Job(Job&&) = default;
        // Job(const Job&) = delete;
        // Job& operator=(const Job&) = delete;
    };

    // Helper function to create jobs from callables
    template <typename F>
    Job make_job(F&& f, uint32_t owner_id);

    using JobQueue = DoubleBuffer<Job>;
    using WorkerId = std::size_t;

    // JobScheduler class declaration
    class JobScheduler
    {
    public:
        JobScheduler(std::size_t num_workers = 1, std::size_t batch_capacity = 1048576);
        ~JobScheduler();

        // Submit a job to a specific worker
        void submit_job_on(WorkerId worker_id, Job&& job);

        // Process all pending jobs
        void process_jobs();

        // Process jobs for a specific worker
        void process_jobs_on(WorkerId worker_id);

        // Check if all jobs are complete
        bool is_complete() const;

        // Get number of workers
        std::size_t get_num_workers() const;

    private:
        class Impl;
        Impl* pimpl_;
        
        std::size_t num_workers_;
        std::size_t batch_capacity_;
    };
}

#endif // JOB_SCHEDULER_H
