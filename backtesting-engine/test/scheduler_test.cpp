#include "../job_scheduler.cpp"
#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <thread>

void test_basic_job_submission()
{
    std::cout << "=== Testing Basic Job Submission ===\n";
    std::atomic<int> counter{0};
    
    {
        scheduler::JobScheduler scheduler(3, 1000);
        
        auto job1 = scheduler::make_job(
            [&counter]() {
                std::cout << "Job 1 executing\n";
                counter.fetch_add(1, std::memory_order_relaxed);
            },
            0U
        );

        auto job2 = scheduler::make_job(
            [&counter]() {
                std::cout << "Job 2 executing\n";
                counter.fetch_add(1, std::memory_order_relaxed);
            },
            1U
        );

        auto job3 = scheduler::make_job(
            [&counter]() {
                std::cout << "Job 3 executing\n";
                counter.fetch_add(1, std::memory_order_relaxed);
            },
            2U
        );
        
        std::cout << "Submitting jobs...\n";
        scheduler.submit_job(std::move(job1));
        scheduler.submit_job(std::move(job2));
        scheduler.submit_job(std::move(job3));
        
            std::cout << "Calling process_jobs()...\n";
            scheduler.process_jobs();
    }
    
    std::cout << "Counter value: " << counter.load() << "\n";
    assert(counter.load() == 3 && "All 3 jobs should have executed");
    
    std::cout << "✓ Basic Job Submission test PASSED!\n\n";
}

void test_multiple_jobs_same_worker()
{
    std::cout << "=== Testing Multiple Jobs on Same Worker ===\n";
    std::atomic<int> counter{0};
    const int NUM_JOBS = 100;
    
    {
        scheduler::JobScheduler scheduler(4, 1000);
        
        for (int i = 0; i < NUM_JOBS; ++i)
        {
            auto job = scheduler::make_job(
                [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
                0U  // All to worker 0
            );
            scheduler.submit_job(std::move(job));
        }
        
            scheduler.process_jobs();
    }
    
    assert(counter.load() == NUM_JOBS && "All jobs should have executed");
    
    std::cout << "✓ Multiple Jobs Same Worker test PASSED!\n\n";
}

void test_round_robin_distribution()
{
    std::cout << "=== Testing Round-Robin Distribution ===\n";
    
    const int NUM_WORKERS = 4;
    const int NUM_JOBS = 1000;
    std::atomic<int> counter{0};
    
    {
        scheduler::JobScheduler scheduler(4, 1000);
    
    
        // Distribute jobs round-robin across workers
        for (int i = 0; i < NUM_JOBS; ++i)
        {
            auto job = scheduler::make_job(
                [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
                static_cast<uint32_t>(i % NUM_WORKERS)
            );
            scheduler.submit_job(std::move(job));
        }
        
            scheduler.process_jobs();
    }
    
    assert(counter.load() == NUM_JOBS && "All jobs should have executed");
    
    std::cout << "✓ Round-Robin Distribution test PASSED!\n\n";
}

void test_computational_jobs()
{
    std::cout << "=== Testing Computational Jobs ===\n";
    
    std::atomic<int> counter{0};
    const int NUM_JOBS = 100;
    auto start = std::chrono::high_resolution_clock::now();
    
    {
        scheduler::JobScheduler scheduler(4, 1000);
    
        // Submit computational jobs across all workers
        for (int i = 0; i < NUM_JOBS; ++i)
        {
            int value = i;
            auto job = scheduler::make_job(
                [&counter, value]() {
                    volatile int result = 0;
                    for (int j = 0; j < 1000; ++j)
                    {
                        result += j * value;
                    }
                    counter.fetch_add(1, std::memory_order_relaxed);
                },
                static_cast<uint32_t>(i % 4)
            );
            scheduler.submit_job(std::move(job));
        }
        
            scheduler.process_jobs();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    assert(counter.load() == NUM_JOBS && "All computational jobs should have executed");
    
    std::cout << "  Processed " << NUM_JOBS << " computational jobs in " << duration.count() << "ms\n";
    std::cout << "✓ Computational Jobs test PASSED!\n\n";
}

void test_stress_submission()
{
    std::cout << "=== Testing Raw Throughput: Multi-Batch vs Single-Batch (emplace only) ===\n";

    const int NUM_JOBS = 1024 * 1024;
    const int WORKERS = 2;
    const int NUM_BATCHES = 128;
    const int JOBS_PER_BATCH = NUM_JOBS / NUM_BATCHES;

    // Test 1: Multi-batch async processing (streaming jobs)
    {
        std::vector<int> worker_counters(WORKERS, 0);
        auto start = std::chrono::high_resolution_clock::now();

        {
            scheduler::JobScheduler scheduler(WORKERS, JOBS_PER_BATCH);

            for (int batch = 0; batch < NUM_BATCHES; ++batch)
            {
                // Submit jobs for this batch using emplace
                for (int i = 0; i < JOBS_PER_BATCH; ++i)
                {
                    int global_job_idx = batch * JOBS_PER_BATCH + i;
                    std::size_t wid = static_cast<std::size_t>(global_job_idx % WORKERS);
                    scheduler.submit_job_on(wid, scheduler::make_job(
                        [&worker_counters, wid]() {
                            int result = 0;
                            for (int j = 0; j < 100; ++j)
                            {
                                result += j * wid;
                            }
                            worker_counters[wid] += 1;
                        },
                        static_cast<uint32_t>(wid)
                    ));
                }

                // Process this batch asynchronously
                scheduler.process_jobs_async();
            }

            // // Wait for all async processing to complete
            // while (!scheduler.is_complete()) {
            //     std::this_thread::yield();
            // }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dur = end - start;
        double seconds = dur.count();
        double total_throughput = (seconds > 0.0) ? (double(NUM_JOBS) / seconds) : 0.0;

        std::cout << "  [MULTI-BATCH ASYNC]  Total throughput: " << std::fixed << std::setprecision(2) << total_throughput << " jobs/sec\n";
        std::cout << "  Streaming " << NUM_BATCHES << " batches asynchronously\n";
    }

    // Test 2: Sync processing with multiple batches
    {
        std::vector<int> worker_counters(WORKERS, 0);
        auto start = std::chrono::high_resolution_clock::now();

        {
            scheduler::JobScheduler scheduler(WORKERS, JOBS_PER_BATCH);

            for (int batch = 0; batch < NUM_BATCHES; ++batch)
            {
                // Submit jobs for this batch using emplace
                for (int i = 0; i < JOBS_PER_BATCH; ++i)
                {
                    int global_job_idx = batch * JOBS_PER_BATCH + i;
                    std::size_t wid = static_cast<std::size_t>(global_job_idx % WORKERS);
                    scheduler.submit_job_on(wid, scheduler::make_job(
                        [&worker_counters, wid]() {
                            int result = 0;
                            for (int j = 0; j < 100; ++j)
                            {
                                result += j * wid;
                            }
                            worker_counters[wid] += 1;
                        },
                        static_cast<uint32_t>(wid)
                    ));
                }

                // Process this batch synchronously
                scheduler.process_jobs();
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dur = end - start;
        double seconds = dur.count();
        double total_throughput = (seconds > 0.0) ? (double(NUM_JOBS) / seconds) : 0.0;

        std::cout << "  [MULTI-BATCH SYNC]   Total throughput: " << std::fixed << std::setprecision(2) << total_throughput << " jobs/sec\n";
        std::cout << "  Processing " << NUM_BATCHES << " batches synchronously\n";
    }

    // Test 3: Single batch processing (traditional approach)
    {
        std::vector<int> worker_counters(WORKERS, 0);
        auto start = std::chrono::high_resolution_clock::now();

        {
            scheduler::JobScheduler scheduler(WORKERS, JOBS_PER_BATCH);

            // Submit all jobs at once using emplace
            for (int i = 0; i < NUM_JOBS; ++i)
            {
                std::size_t wid = static_cast<std::size_t>(i % WORKERS);
                scheduler.submit_job_on(wid, scheduler::make_job(
                    [&worker_counters, wid]() {
                        int result = 0;
                        for (int j = 0; j < 100; ++j)
                        {
                            result += j * wid;
                        }
                        worker_counters[wid] += 1;
                    },
                    static_cast<uint32_t>(wid)
                ));
            }

            // Process all jobs asynchronously
            scheduler.process_jobs_async();
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dur = end - start;
        double seconds = dur.count();
        double total_throughput = (seconds > 0.0) ? (double(NUM_JOBS) / seconds) : 0.0;

        std::cout << "  [SINGLE-BATCH ASYNC] Total throughput: " << std::fixed << std::setprecision(2) << total_throughput << " jobs/sec\n";
        std::cout << "  Bulk submit single batch asynchronously\n";
    }

    // Test 4: Single-batch sync processing
    {
        std::vector<int> worker_counters(WORKERS, 0);
        auto start = std::chrono::high_resolution_clock::now();

        {
            scheduler::JobScheduler scheduler(WORKERS, JOBS_PER_BATCH);

            // Submit all jobs at once using emplace
            for (int i = 0; i < NUM_JOBS; ++i)
            {
                std::size_t wid = static_cast<std::size_t>(i % WORKERS);
                scheduler.submit_job_on(wid, scheduler::make_job(
                    [&worker_counters, wid]() {
                        int result = 0;
                        for (int j = 0; j < 100; ++j)
                        {
                            result += j * wid;
                        }
                        worker_counters[wid] += 1;
                    },
                    static_cast<uint32_t>(wid)
                ));
            }

            // Process all jobs synchronously
            scheduler.process_jobs();
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> dur = end - start;
        double seconds = dur.count();
        double total_throughput = (seconds > 0.0) ? (double(NUM_JOBS) / seconds) : 0.0;

        std::cout << "  [SINGLE-BATCH SYNC]  Total throughput: " << std::fixed << std::setprecision(2) << total_throughput << " jobs/sec\n";
        std::cout << "  Bulk submit single batch synchronously\n";
    }

    std::cout << "✓ Stress Submission test PASSED!\n\n";
}

void test_empty_check()
{
    std::cout << "=== Testing Empty Check ===\n";
    
    scheduler::JobScheduler scheduler(4, 1000);
    
    // Initially should be empty
    assert(scheduler.is_complete() && "Scheduler should be empty initially");
    
    std::atomic<int> counter{0};
    auto job = scheduler::make_job(
        [&counter]() { counter.fetch_add(1, std::memory_order_relaxed); },
        0U
    );
    
    scheduler.submit_job(std::move(job));
    
    // Execute and wait for job to complete
        scheduler.process_jobs();
    
    // Should be empty again
    assert(scheduler.is_complete() && "Scheduler should be empty after completion");
    assert(counter.load() == 1 && "Job should have executed");
    
    std::cout << "✓ Empty Check test PASSED!\n\n";
}

void test_sequential_vs_parallel()
{
    std::cout << "=== Testing Sequential vs Parallel Performance ===\n";
    
    const int NUM_JOBS = 100000;
    std::atomic<int> seq_counter{0};
    
    // Sequential execution
    auto seq_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_JOBS; ++i)
    {
        volatile int result = 0;
        for (int j = 0; j < 1000; ++j)
        {
            result += j * i;
        }
        seq_counter.fetch_add(1, std::memory_order_relaxed);
    }
    auto seq_end = std::chrono::high_resolution_clock::now();
    auto seq_duration = std::chrono::duration_cast<std::chrono::milliseconds>(seq_end - seq_start);
    
    std::cout << "  Sequential: " << seq_duration.count() << "ms\n";

    // Parallel execution - distribute jobs across all 4 workers
    std::atomic<int> par_counter{0};
    const int NUM_WORKERS = 4;
    
    auto par_start = std::chrono::high_resolution_clock::now();
    
    {
        scheduler::JobScheduler scheduler(NUM_WORKERS, NUM_JOBS / NUM_WORKERS);
        for (int i = 0; i < NUM_JOBS; ++i)
        {
            int value = i;
            auto job = scheduler::make_job(
                [&par_counter, value]() {
                    volatile int result = 0;
                    for (int j = 0; j < 1000; ++j)
                    {
                        result += j * value;
                    }
                    par_counter.fetch_add(1, std::memory_order_relaxed);
                },
                static_cast<uint32_t>(i % NUM_WORKERS)
            );
            scheduler.submit_job(std::move(job));
        }
            scheduler.process_jobs();
    }
    
    auto par_end = std::chrono::high_resolution_clock::now();
    auto par_duration = std::chrono::duration_cast<std::chrono::milliseconds>(par_end - par_start);
    
    std::cout << "  Parallel (4 threads): " << par_duration.count() << "ms\n";
    
    double speedup = static_cast<double>(seq_duration.count()) / par_duration.count();
    std::cout << "  Speedup: " << speedup << "x\n";
    
    assert(par_counter.load() == NUM_JOBS && "All parallel jobs should have executed");
    
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
