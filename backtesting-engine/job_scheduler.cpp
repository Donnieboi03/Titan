#include "../tools/ring_buffer.cpp"
#include "../tools/arena.cpp"
#include <thread>
#include <atomic>
#include <functional>

// Remove is_notification flag - separate schedulers handle this
struct Job
{
    std::function<void()> execute;
    std::function<void()> cleanup;
    std::size_t owner_id;  // For filtering/routing
    
    Job() : owner_id(0) {}
    
    Job(std::function<void()> exec, std::function<void()> clean, std::size_t id)
    : execute(std::move(exec)), cleanup(std::move(clean)), owner_id(id)
    {}
};

using JobQueue = RingBuffer<Job>;

class JobScheduler
{
public:
    JobScheduler(std::size_t num_workers = 1)
    : num_workers_(num_workers), running_(false), processing_(false)
    {
        job_queues_.resize(num_workers_);
        
        for (std::size_t i = 0; i < num_workers_; ++i)
        {
            workers_.emplace_back([this, i]() { worker_loop(i); });
        }
    }

    // Delete copy/move to prevent accidental duplication
    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;
    JobScheduler(JobScheduler&&) = delete;
    JobScheduler& operator=(JobScheduler&&) = delete;

    ~JobScheduler()
    {
        running_.store(false, std::memory_order_release);
        processing_.store(false, std::memory_order_release);
        
        for (auto& worker : workers_)
        {
            if (worker.joinable())
                worker.join();
        }
    }

    void submit_job(Job&& job) noexcept
    {
        // Route by owner_id (engine_id)
        auto& job_queue = job_queues_[job.owner_id % num_workers_];
        job_queue.push(std::forward<Job>(job));
    }

    void process_jobs() noexcept
    {
        processing_.store(true, std::memory_order_release);
        wait_for_completion();
        processing_.store(false, std::memory_order_release);
    }
    
    void process_jobs_async() noexcept
    {
        processing_.store(true, std::memory_order_release);
    }
    
    bool is_complete() const noexcept
    {
        return all_queues_empty() && active_jobs_.load(std::memory_order_acquire) == 0;
    }
    
    bool is_worker_complete(std::size_t worker_id) const noexcept
    {
        if (worker_id >= job_queues_.size())
            return false;
        return job_queues_[worker_id].empty();
    }
    
    void wait_for_completion() noexcept
    {
        while (!all_queues_empty() || active_jobs_.load(std::memory_order_acquire) > 0)
        {
            std::this_thread::yield();
        }
        processing_.store(false, std::memory_order_release);
    }
    
    bool all_queues_empty() const noexcept
    {
        for (const auto& queue : job_queues_)
        {
            if (!queue.empty())
                return false;
        }
        return true;
    }

private:
    void worker_loop(std::size_t worker_id)
    {
        running_.store(true, std::memory_order_release);
        
        while (running_.load(std::memory_order_acquire))
        {
            if (!processing_.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
                continue;
            }
            
            auto& job_queue = job_queues_[worker_id];
            
            if (job_queue.empty())
            {
                std::this_thread::yield();
                continue;
            }
            
            const Job& job = job_queue.front();
            active_jobs_.fetch_add(1, std::memory_order_acquire);
            
            if (job.execute) 
                job.execute();
            
            if (job.cleanup)
                job.cleanup();
            
            job_queue.pop();
            active_jobs_.fetch_sub(1, std::memory_order_release);
        }
    }

    std::vector<JobQueue> job_queues_;
    std::vector<std::thread> workers_;
    std::size_t num_workers_;
    std::atomic<bool> running_;
    std::atomic<bool> processing_;
    std::atomic<int> active_jobs_{0};
};