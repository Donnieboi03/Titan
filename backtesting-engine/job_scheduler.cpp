#include "../tools/double_buffer.cpp"
#include <thread>
#include <atomic>
#include <functional>

struct Job
{
    std::function<void()> execute;
    std::size_t owner_id;
    
    Job() : owner_id(0) {}
    
    Job(std::function<void()> exec, std::size_t id)
    : execute(std::move(exec)), owner_id(id)
    {}
};

using JobQueue = DoubleBuffer<Job>;
using WorkerId = std::size_t;

class JobScheduler
{
public:
    JobScheduler(std::size_t num_workers = 1, std::size_t batch_capacity = 16384)
    : num_workers_(num_workers), batch_capacity_(batch_capacity)
    {
        job_queues_.reserve(num_workers_);
        
        // Construct queue object
        for (std::size_t i = 0; i < num_workers_; ++i)
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
        
        // If push fails, yeild and retry
        while (!buffer.try_push(std::move(job))) 
            std::this_thread::yield();
        
        return worker_id;
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
        while (running_.load(std::memory_order_acquire))
        {         
            // If pop fails then yeild and retry   
            if (!job_queues_[worker_id].try_pop(job))
            {
                std::this_thread::yield();
                continue;
            }
            
            // Execute
            if (job.execute) 
                job.execute();            
        }
    }

    void execute_batch() noexcept
    {
        for (auto& buffer : job_queues_)
            buffer.flush();
    }

    void execute_batch(WorkerId worker_id) noexcept { job_queues_[worker_id].flush(); }

    void wait_for_completion() noexcept
    {
        while (!all_queues_empty())
            std::this_thread::yield();
    }
    
    void wait_for_completion(WorkerId worker_id) noexcept
    {
        while (!job_queues_[worker_id].empty())
            std::this_thread::yield();
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