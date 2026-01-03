#include "../job_scheduler.cpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <atomic>

// Test job arguments
struct TestArgs
{
    std::string text;
    int value;
    std::atomic<int>* counter;
};

using ArgsPool = Arena<TestArgs>;

// Test job functions
void increment_counter_job(void* args)
{
    auto* test_args = static_cast<TestArgs*>(args);
    test_args->counter->fetch_add(1, std::memory_order_relaxed);
}

void print_job(void* args)
{
    auto* test_args = static_cast<TestArgs*>(args);
    std::cout << test_args->text << " " << test_args->value << std::endl;
}

void compute_job(void* args)
{
    auto* test_args = static_cast<TestArgs*>(args);
    volatile int result = 0;
    for (int i = 0; i < 1000; ++i)  // Less work: 1k iterations
    {
        result += i * test_args->value;
    }
    test_args->counter->fetch_add(1, std::memory_order_relaxed);
}

void test_basic_job_submission()
{
    std::cout << "=== Testing Basic Job Submission ===\n";
    
    JobScheduler scheduler(4);
    ArgsPool args_pool(10);  // Capacity for 10 args
    std::atomic<int> counter{0};
    
    // Use Arena allocation
    auto idx1 = args_pool.emplace("Job 1", 42, &counter);
    auto idx2 = args_pool.emplace("Job 2", 100, &counter);
    auto idx3 = args_pool.emplace("Job 3", 200, &counter);
    
    Job job1{increment_counter_job, &args_pool[idx1], 0};
    Job job2{increment_counter_job, &args_pool[idx2], 1};
    Job job3{increment_counter_job, &args_pool[idx3], 2};
    
    // Stage 1: Submit jobs to different workers
    scheduler.submit_job(std::move(job1));
    scheduler.submit_job(std::move(job2));
    scheduler.submit_job(std::move(job3));
    
    // Stage 2: Process all jobs
    scheduler.process_jobs();
    
    assert(counter.load() == 3 && "All 3 jobs should have executed");
    
    std::cout << "✓ Basic Job Submission test PASSED!\n\n";
}

void test_multiple_jobs_same_worker()
{
    std::cout << "=== Testing Multiple Jobs on Same Worker ===\n";
    
    JobScheduler scheduler(4);
    ArgsPool args_pool(150);
    std::atomic<int> counter{0};
    
    const int NUM_JOBS = 100;
    std::vector<Job> jobs;
    
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        auto idx = args_pool.emplace("Job", i, &counter);
        jobs.push_back(Job{increment_counter_job, &args_pool[idx], 0});
    }
    
    for (auto& job : jobs)
    {
        scheduler.submit_job(std::move(job));
    }
    
    scheduler.process_jobs();
    
    assert(counter.load() == NUM_JOBS && "All jobs should have executed");
    
    std::cout << "✓ Multiple Jobs Same Worker test PASSED!\n\n";
}

void test_round_robin_distribution()
{
    std::cout << "=== Testing Round-Robin Distribution ===\n";
    
    const int NUM_WORKERS = 4;
    JobScheduler scheduler(NUM_WORKERS);
    ArgsPool args_pool(1200);  // Capacity for 1200 args
    std::atomic<int> counter{0};
    
    const int NUM_JOBS = 1000;
    std::vector<Job> jobs;
    
    // Use Arena allocation for stable addresses
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        auto idx = args_pool.emplace("Job", i, &counter);
        jobs.push_back(Job{increment_counter_job, &args_pool[idx], static_cast<std::size_t>(i % NUM_WORKERS)});
    }
    
    // Distribute jobs round-robin across workers
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        scheduler.submit_job(std::move(jobs[i]));
    }
    
    scheduler.process_jobs();
    
    assert(counter.load() == NUM_JOBS && "All jobs should have executed");
    
    std::cout << "✓ Round-Robin Distribution test PASSED!\n\n";
}

void test_computational_jobs()
{
    std::cout << "=== Testing Computational Jobs ===\n";
    
    JobScheduler scheduler(4);
    ArgsPool args_pool(150);  // Capacity for 150 args
    std::atomic<int> counter{0};
    
    const int NUM_JOBS = 100;
    std::vector<Job> jobs;
    
    // Use Arena allocation for stable addresses
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        auto idx = args_pool.emplace("Compute", i, &counter);
        jobs.push_back(Job{compute_job, &args_pool[idx], static_cast<std::size_t>(i % 4)});
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Submit jobs across all workers
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        scheduler.submit_job(std::move(jobs[i]));
    }
    
    scheduler.process_jobs();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    assert(counter.load() == NUM_JOBS && "All computational jobs should have executed");
    
    std::cout << "  Processed " << NUM_JOBS << " computational jobs in " << duration.count() << "ms\n";
    std::cout << "✓ Computational Jobs test PASSED!\n\n";
}

void test_stress_submission()
{
    std::cout << "=== Testing Stress Submission ===\n";
    
    JobScheduler scheduler(8);
    const int NUM_JOBS = 100000;
    ArgsPool args_pool(NUM_JOBS + 200);  // Capacity for 12000 args
    std::atomic<int> counter{0};
    
    std::vector<Job> jobs;
    
    jobs.reserve(NUM_JOBS);
    
    // Use Arena allocation for stable addresses
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        auto idx = args_pool.emplace("Stress", i, &counter);
        jobs.push_back(Job{increment_counter_job, &args_pool[idx], static_cast<std::size_t>(i % 8)});
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Rapid-fire job submission
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        scheduler.submit_job(std::move(jobs[i]));
    }
    
    scheduler.process_jobs();
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    assert(counter.load() == NUM_JOBS && "All stress test jobs should have executed");
    
    std::cout << "  Processed " << NUM_JOBS << " jobs in " << duration.count() << "ms\n";
    std::cout << "  Throughput: " << (NUM_JOBS * 1000.0 / duration.count()) << " jobs/sec\n";
    std::cout << "✓ Stress Submission test PASSED!\n\n";
}

void test_empty_check()
{
    std::cout << "=== Testing Empty Check ===\n";
    
    JobScheduler scheduler(4);
    ArgsPool args_pool(10);  // Capacity for 10 args
    
    // Initially should be empty
    assert(scheduler.all_empty() && "Scheduler should be empty initially");
    
    std::atomic<int> counter{0};
    auto idx = args_pool.emplace("Test", 1, &counter);
    Job job{increment_counter_job, &args_pool[idx], 0};
    
    scheduler.submit_job(std::move(job));
    
    // Wait for job to complete
    scheduler.process_jobs();
    
    // Should be empty again
    assert(scheduler.all_empty() && "Scheduler should be empty after completion");
    assert(counter.load() == 1 && "Job should have executed");
    
    std::cout << "✓ Empty Check test PASSED!\n\n";
}

void test_sequential_vs_parallel()
{
    std::cout << "=== Testing Sequential vs Parallel Performance ===\n";
    
    const int NUM_JOBS = 10000;
    std::atomic<int> counter{0};
    ArgsPool args_pool(NUM_JOBS + 200);  // Capacity for 1200 args
    
    std::vector<size_t> indices;
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        indices.push_back(args_pool.emplace("Compute", i, &counter));
    }
    
    // Sequential execution
    auto seq_start = std::chrono::high_resolution_clock::now();
    for (auto idx : indices)
    {
        compute_job(&args_pool[idx]);
    }
    auto seq_end = std::chrono::high_resolution_clock::now();
    auto seq_duration = std::chrono::duration_cast<std::chrono::milliseconds>(seq_end - seq_start);
    
    std::cout << "  Sequential: " << seq_duration.count() << "ms\n";
    
    // Parallel execution - distribute jobs across all 4 workers
    counter.store(0);
    auto NUM_WORKERS = 4;
    JobScheduler scheduler(NUM_WORKERS);
    std::vector<Job> jobs;
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        jobs.push_back(Job{compute_job, &args_pool[indices[i]], static_cast<std::size_t>(i % NUM_WORKERS)});
    }
    
    auto par_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        scheduler.submit_job(std::move(jobs[i]));
    }
    scheduler.process_jobs();
    auto par_end = std::chrono::high_resolution_clock::now();
    auto par_duration = std::chrono::duration_cast<std::chrono::milliseconds>(par_end - par_start);
    
    std::cout << "  Parallel (4 threads): " << par_duration.count() << "ms\n";
    
    double speedup = static_cast<double>(seq_duration.count()) / par_duration.count();
    std::cout << "  Speedup: " << speedup << "x\n";
    
    assert(counter.load() == NUM_JOBS && "All parallel jobs should have executed");
    
    std::cout << "✓ Sequential vs Parallel test PASSED!\n\n";
}

int main()
{
    std::cout << "========================================\n";
    std::cout << "  Job Scheduler Tests\n";
    std::cout << "========================================\n\n";
    
    test_basic_job_submission();
    test_multiple_jobs_same_worker();
    test_round_robin_distribution();
    test_computational_jobs();
    test_empty_check();
    test_stress_submission();
    test_sequential_vs_parallel();
    
    std::cout << "========================================\n";
    std::cout << "  All Scheduler Tests PASSED! ✓\n";
    std::cout << "========================================\n";
    
    return 0;
}
