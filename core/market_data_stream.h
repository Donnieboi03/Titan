#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

// TODO: Future implementation can integrate an L3 parser (e.g. L3Stream) in this module for L3 event stream read/write.

namespace stream
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

    enum class StreamMode { Read, Write };

    // L2 stream: read (replay) or write (record) L2 updates in Titan canonical format (.bin, .csv, .csv.gz)
    class L2Stream
    {
    private:
        enum class Format { BINARY, CSV, CSV_GZ, UNKNOWN };

        StreamMode mode_;
        Format format_;

        // Read path state
        int fd_;
        const char* mapped_data_;
        size_t file_size_;
        uint64_t total_records_;
        uint64_t current_record_;
        static constexpr size_t SIMD_BATCH = 4;
        L2Update prefetch_buffer_[SIMD_BATCH];
        size_t buffer_index_;
        size_t buffer_valid_;

        bool use_streaming_;
        static constexpr size_t STREAM_BUFFER_SIZE = 64 * 1024 * 1024; // 64MB buffer
        std::vector<L2UpdateBinary> stream_buffer_;
        size_t stream_buffer_pos_;
        size_t stream_buffer_valid_;
        off_t file_position_;

        gzFile gz_file_;
        char line_buffer_[1024];
        bool header_parsed_;

        // Write path state
        FILE* write_file_;
        gzFile write_gz_file_;
        bool write_binary_;
        static constexpr size_t WRITE_BUFFER_SIZE = 1024;
        std::vector<L2UpdateBinary> write_buffer_;
        bool csv_header_written_;

    public:
        // Read mode: open existing file for replay
        explicit L2Stream(const std::string& filepath, bool streaming = true);

        // Write mode: create file for recording (format from extension: .bin -> binary, .csv/.csv.gz -> CSV)
        L2Stream(const std::string& filepath, StreamMode mode);

        ~L2Stream();

        L2Stream(const L2Stream&) = delete;
        L2Stream& operator=(const L2Stream&) = delete;
        L2Stream(L2Stream&&) = delete;
        L2Stream& operator=(L2Stream&&) = delete;

        // Read path (valid only when opened for read)
        bool parse_next(L2Update& update);
        uint64_t get_total_records() const { return total_records_; }
        bool is_open() const;

        // Write path (valid only when opened for write)
        bool write(const L2Update& update);
        void flush();

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

        void init_write(const std::string& filepath);
        void flush_write_buffer();
    };
}
