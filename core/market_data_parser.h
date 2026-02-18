#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

namespace parser
{
    // Universal L2 Update structure
    struct L2Update
    {
        int64_t timestamp;    // Microseconds since epoch
        double price;         // Price level
        double amount;        // Total amount at this price
        char side;           // 'b' or 'a' (bid/ask)
        bool is_snapshot;    // True if snapshot, false if incremental
    };

    // Binary format (32 bytes per record)
    struct __attribute__((packed)) L2UpdateBinary
    {
        int64_t timestamp;
        double price;
        double amount;
        char side;
        uint8_t is_snapshot;
        char padding[6];
    };
    
    static_assert(sizeof(L2UpdateBinary) == 32, "Binary record must be 32 bytes");

    // Unified parser supporting .bin, .csv, and .csv.gz
    class MarketDataParser
    {
    private:
        enum class Format { BINARY, CSV, CSV_GZ, UNKNOWN };
        
        Format format_;
        
        // Binary format (mmap + SIMD)
        int fd_;
        const char* mapped_data_;
        size_t file_size_;
        uint64_t total_records_;
        uint64_t current_record_;
        static constexpr size_t SIMD_BATCH = 4;
        L2Update prefetch_buffer_[SIMD_BATCH];
        size_t buffer_index_;
        size_t buffer_valid_;
        
        // Streaming mode (reduces memory from 2.6GB to ~64MB)
        bool use_streaming_;
        static constexpr size_t STREAM_BUFFER_SIZE = 64 * 1024 * 1024; // 64MB buffer
        std::vector<L2UpdateBinary> stream_buffer_;
        size_t stream_buffer_pos_;
        size_t stream_buffer_valid_;
        off_t file_position_;
        
        // CSV format (gzip support)
        gzFile gz_file_;
        char line_buffer_[1024];
        bool header_parsed_;

    public:
        explicit MarketDataParser(const std::string& filepath, bool streaming = true);
        ~MarketDataParser();

        // Delete copy/move constructors (resource management)
        MarketDataParser(const MarketDataParser&) = delete;
        MarketDataParser& operator=(const MarketDataParser&) = delete;
        MarketDataParser(MarketDataParser&&) = delete;
        MarketDataParser& operator=(MarketDataParser&&) = delete;

        bool parse_next(L2Update& update);
        uint64_t get_total_records() const { return total_records_; }
        bool is_open() const;

    private:
        Format detect_format(const std::string& filepath);
        void init_binary_mmap(const std::string& filepath);
        void init_binary_streaming(const std::string& filepath);
        void init_csv(const std::string& filepath);
        bool parse_next_binary(L2Update& update);
        bool parse_next_binary_streaming(L2Update& update);
        bool parse_next_csv(L2Update& update);
        bool refill_buffer_simd();
        bool refill_stream_buffer();
    };
}
