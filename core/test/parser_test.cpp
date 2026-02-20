#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
#include "../market_data_stream.h"

using namespace stream;

void print_update(const L2Update& update, int count)
{
    std::cout << "#" << count << " - "
              << "Timestamp: " << update.timestamp
              << ", Side: " << update.side
              << ", Price: $" << std::fixed << std::setprecision(2) << update.price
              << ", Amount: " << update.amount
              << ", Snapshot: " << (update.is_snapshot ? "true" : "false")
              << "\n";
}

void benchmark_parser(const std::string& filepath)
{
    std::cout << "\n=== Benchmarking: " << filepath << " ===\n";
    
    try {
        L2Stream parser(filepath);
        
        if (!parser.is_open()) {
            std::cerr << "ERROR: Failed to open file\n";
            return;
        }
        
        std::cout << "Total records: " << parser.get_total_records() << "\n";
        
        // Print first 5 updates
        std::cout << "\nFirst 5 updates:\n";
        L2Update update;
        int count = 0;
        while (count < 5 && parser.parse_next(update)) {
            print_update(update, count + 1);
            count++;
        }
        
        // Benchmark full parse
        L2Stream benchmark_parser(filepath);
        uint64_t total_parsed = 0;
        
        auto start = std::chrono::high_resolution_clock::now();
        
        while (benchmark_parser.parse_next(update)) {
            total_parsed++;
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        double duration_sec = duration.count() / 1000.0;
        double throughput = total_parsed / duration_sec;
        
        std::cout << "\n=== Performance ===\n";
        std::cout << "Total parsed: " << total_parsed << " updates\n";
        std::cout << "Duration: " << duration_sec << " seconds\n";
        std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
                  << throughput << " updates/sec\n";
        std::cout << "Throughput: " << std::fixed << std::setprecision(2) 
                  << (throughput / 1000000.0) << "M updates/sec\n";
        
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
    }
}

void test_format_detection()
{
    std::cout << "\n=== Testing Format Detection ===\n";
    
    std::vector<std::pair<std::string, std::string>> test_files = {
        {"test.bin", "Binary"},
        {"test.csv", "CSV"},
        {"test.csv.gz", "CSV.GZ"},
        {"test.txt", "Unknown"}
    };
    
    for (const auto& [file, expected] : test_files) {
        try {
            L2Stream parser(file);
            std::cout << "✓ " << file << " detected as valid format\n";
        } catch (const std::exception& e) {
            std::cout << "✗ " << file << " - " << e.what() << "\n";
        }
    }
}

int main(int argc, char* argv[])
{
    std::cout << "=== Market Data Parser Test ===\n";
    
    if (argc < 2) {
        std::cerr << "\nUsage: " << argv[0] << " <data_file>\n";
        std::cerr << "Supported formats: .bin (binary), .csv, .csv.gz\n";
        std::cerr << "\nExamples:\n";
        std::cerr << "  " << argv[0] << " data/BTCUSDT.bin\n";
        std::cerr << "  " << argv[0] << " data/BTCUSDT.csv\n";
        std::cerr << "  " << argv[0] << " data/BTCUSDT.csv.gz\n";
        
        // Run format detection test
        test_format_detection();
        
        return 1;
    }
    
    std::string filepath = argv[1];
    benchmark_parser(filepath);
    
    return 0;
}
