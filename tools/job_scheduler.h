#pragma once
#include "double_buffer.cpp"
#include <thread>
#include <atomic>
#include <array>
#include <algorithm>

namespace scheduler
{
    struct Job {
        using InvokeFn = void (*)(void*);

        uint32_t owner_id;
        InvokeFn invoke;
        alignas(16) unsigned char storage[48]; // tuneable

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

        bool process_jobs_async() noexcept;
        bool process_jobs_on_async(WorkerId worker_id) noexcept;

        bool is_complete() noexcept;
        bool is_complete_on(WorkerId worker_id) noexcept;

        std::size_t pending_jobs() const noexcept;
        std::size_t pending_jobs_on(WorkerId worker_id) const noexcept;

        bool is_full_on(WorkerId worker_id) const noexcept;
        bool is_empty_on(WorkerId worker_id) const noexcept;
        std::size_t get_worker_count() const noexcept;
        std::size_t get_batch_capacity() const noexcept;

    private:
        void worker_loop(std::size_t worker_id);

        bool execute_batch() noexcept;
        bool execute_batch_on(WorkerId worker_id) noexcept;

        void wait_for_completion() noexcept;
        void wait_for_completion_on(WorkerId worker_id) noexcept;

        bool all_queues_finished() const noexcept;

        std::vector<JobQueue> job_queues_;
        std::unique_ptr<std::atomic<bool>[]> finished_last_job_;
        std::vector<std::thread> workers_;
        std::size_t num_workers_;
        std::size_t batch_capacity_;
        std::atomic<bool> running_{true};
    };
}
