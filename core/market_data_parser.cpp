#include "market_data_parser.h"
#include <stdexcept>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Portable SIMD for binary parsing
#include "hwy/highway.h"

parser::MarketDataParser::MarketDataParser(const std::string& filepath, bool streaming)
    : format_(Format::UNKNOWN), fd_(-1), mapped_data_(nullptr), file_size_(0),
        current_record_(0), total_records_(0), buffer_index_(0), buffer_valid_(0),
        gz_file_(nullptr), header_parsed_(false),
        use_streaming_(streaming), stream_buffer_pos_(0), stream_buffer_valid_(0), file_position_(0)
{
    // Initialize prefetch buffer to safe defaults
    std::memset(prefetch_buffer_, 0, sizeof(prefetch_buffer_));
    std::memset(line_buffer_, 0, sizeof(line_buffer_));
    
    format_ = detect_format(filepath);
    
    if (format_ == Format::BINARY) {
        if (use_streaming_) {
            init_binary_streaming(filepath);
        } else {
            init_binary_mmap(filepath);
        }
    } else if (format_ == Format::CSV || format_ == Format::CSV_GZ) {
        init_csv(filepath);
    } else {
        throw std::runtime_error("Unsupported file format: " + filepath);
    }
}

parser::MarketDataParser::~MarketDataParser()
{
    if (format_ == Format::BINARY) {
        if (use_streaming_) {
            // Streaming mode: just close fd, no munmap needed
            if (fd_ != -1) {
                close(fd_);
                fd_ = -1;
            }
            stream_buffer_.clear();
        } else {
            // mmap mode: unmap and close
            if (mapped_data_ && mapped_data_ != MAP_FAILED) {
                int munmap_result = munmap(const_cast<char*>(mapped_data_), file_size_);
                if (munmap_result == -1) {
                    // Log error but don't throw in destructor
                    // std::cerr << "Warning: munmap failed in MarketDataParser destructor" << std::endl;
                }
                mapped_data_ = nullptr;
            }
            if (fd_ != -1) {
                int close_result = close(fd_);
                if (close_result == -1) {
                    // Log error but don't throw in destructor
                    // std::cerr << "Warning: close failed in MarketDataParser destructor" << std::endl;
                }
                fd_ = -1;
            }
        }
        // Reset all binary format state
        file_size_ = 0;
        current_record_ = 0;
        total_records_ = 0;
        buffer_index_ = 0;
        buffer_valid_ = 0;
        stream_buffer_pos_ = 0;
        stream_buffer_valid_ = 0;
        file_position_ = 0;
    } else if (gz_file_) {
        // Ensure gzip file is properly closed
        int gzclose_result = gzclose(gz_file_);
        if (gzclose_result != Z_OK) {
            // Log error but don't throw in destructor
            // std::cerr << "Warning: gzclose failed in MarketDataParser destructor" << std::endl;
        }
        gz_file_ = nullptr;  // Reset pointer to prevent double-close
        // Reset CSV format state
        header_parsed_ = false;
    }
    
    // Reset format to ensure clean state
    format_ = Format::UNKNOWN;
}

bool parser::MarketDataParser::parse_next(L2Update& update)
{
    if (format_ == Format::BINARY) {
        return parse_next_binary(update);
    } else {
        return parse_next_csv(update);
    }
}

bool parser::MarketDataParser::is_open() const
{
    return (format_ == Format::BINARY && mapped_data_ != nullptr) || 
            (format_ != Format::BINARY && gz_file_ != nullptr);
}

parser::MarketDataParser::Format parser::MarketDataParser::detect_format(const std::string& filepath)
{
    if (filepath.size() >= 4 && filepath.substr(filepath.size() - 4) == ".bin") {
        return Format::BINARY;
    } else if (filepath.size() >= 7 && filepath.substr(filepath.size() - 7) == ".csv.gz") {
        return Format::CSV_GZ;
    } else if (filepath.size() >= 4 && filepath.substr(filepath.size() - 4) == ".csv") {
        return Format::CSV;
    }
    return Format::UNKNOWN;
}

void parser::MarketDataParser::init_binary_mmap(const std::string& filepath)
{
    fd_ = open(filepath.c_str(), O_RDONLY);
    if (fd_ == -1) {
        throw std::runtime_error("Failed to open binary file: " + filepath);
    }

    struct stat sb;
    if (fstat(fd_, &sb) == -1) {
        close(fd_);
        throw std::runtime_error("Failed to get file size");
    }
    file_size_ = sb.st_size;

    if (file_size_ % sizeof(L2UpdateBinary) != 0) {
        close(fd_);
        throw std::runtime_error("Invalid binary file format");
    }

    total_records_ = file_size_ / sizeof(L2UpdateBinary);

    mapped_data_ = static_cast<const char*>(
        mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0)
    );

    if (mapped_data_ == MAP_FAILED) {
        close(fd_);
        throw std::runtime_error("Failed to mmap file");
    }

    madvise(const_cast<char*>(mapped_data_), file_size_, MADV_SEQUENTIAL);
}

void parser::MarketDataParser::init_binary_streaming(const std::string& filepath)
{
    fd_ = open(filepath.c_str(), O_RDONLY);
    if (fd_ == -1) {
        throw std::runtime_error("Failed to open binary file: " + filepath);
    }

    struct stat sb;
    if (fstat(fd_, &sb) == -1) {
        close(fd_);
        throw std::runtime_error("Failed to get file size");
    }
    file_size_ = sb.st_size;

    if (file_size_ % sizeof(L2UpdateBinary) != 0) {
        close(fd_);
        throw std::runtime_error("Invalid binary file format");
    }

    total_records_ = file_size_ / sizeof(L2UpdateBinary);
    
    // Allocate streaming buffer (much smaller than full file)
    size_t buffer_records = STREAM_BUFFER_SIZE / sizeof(L2UpdateBinary);
    stream_buffer_.resize(buffer_records);
    
    // Pre-load first buffer
    refill_stream_buffer();
}

void parser::MarketDataParser::init_csv(const std::string& filepath)
{
    gz_file_ = gzopen(filepath.c_str(), "rb");
    if (!gz_file_) {
        throw std::runtime_error("Failed to open CSV file: " + filepath);
    }
}

bool parser::MarketDataParser::parse_next_binary(L2Update& update)
{
    if (use_streaming_) {
        return parse_next_binary_streaming(update);
    }
    
    // mmap version
    if (buffer_index_ >= buffer_valid_) {
        if (!refill_buffer_simd()) {
            return false;
        }
    }
    update = prefetch_buffer_[buffer_index_++];
    return true;
}

bool parser::MarketDataParser::refill_stream_buffer()
{
    if (file_position_ >= static_cast<off_t>(file_size_)) {
        return false;
    }
    
    size_t bytes_remaining = file_size_ - file_position_;
    size_t bytes_to_read = (bytes_remaining < STREAM_BUFFER_SIZE) ? bytes_remaining : STREAM_BUFFER_SIZE;
    
    ssize_t bytes_read = pread(fd_, stream_buffer_.data(), bytes_to_read, file_position_);
    if (bytes_read <= 0) {
        return false;
    }
    
    file_position_ += bytes_read;
    stream_buffer_valid_ = bytes_read / sizeof(L2UpdateBinary);
    stream_buffer_pos_ = 0;
    
    return true;
}

bool parser::MarketDataParser::parse_next_binary_streaming(L2Update& update)
{
    // Refill buffer if needed
    if (stream_buffer_pos_ >= stream_buffer_valid_) {
        if (!refill_stream_buffer()) {
            return false;
        }
    }
    
    // SIMD prefetch from buffer (same as mmap version)
    if (buffer_index_ >= buffer_valid_) {
        using namespace hwy::HWY_NAMESPACE;
        
        buffer_index_ = 0;
        buffer_valid_ = 0;
        
        size_t remaining = stream_buffer_valid_ - stream_buffer_pos_;
        size_t batch_size = (remaining >= SIMD_BATCH) ? SIMD_BATCH : remaining;
        
        const L2UpdateBinary* base_ptr = &stream_buffer_[stream_buffer_pos_];
        
        if (batch_size == SIMD_BATCH) {
            const ScalableTag<double> d;
            const ScalableTag<int64_t> di64;
            
            auto prices_01 = LoadU(d, &base_ptr[0].price);
            auto prices_23 = LoadU(d, &base_ptr[2].price);
            StoreU(prices_01, d, &prefetch_buffer_[0].price);
            StoreU(prices_23, d, &prefetch_buffer_[2].price);
            
            auto amounts_01 = LoadU(d, &base_ptr[0].amount);
            auto amounts_23 = LoadU(d, &base_ptr[2].amount);
            StoreU(amounts_01, d, &prefetch_buffer_[0].amount);
            StoreU(amounts_23, d, &prefetch_buffer_[2].amount);
            
            auto ts_01 = LoadU(di64, &base_ptr[0].timestamp);
            auto ts_23 = LoadU(di64, &base_ptr[2].timestamp);
            StoreU(ts_01, di64, &prefetch_buffer_[0].timestamp);
            StoreU(ts_23, di64, &prefetch_buffer_[2].timestamp);
            
            for (size_t i = 0; i < SIMD_BATCH; ++i) {
                prefetch_buffer_[i].side = base_ptr[i].side;
                prefetch_buffer_[i].is_snapshot = (base_ptr[i].is_snapshot != 0);
            }
        } else {
            for (size_t i = 0; i < batch_size; ++i) {
                prefetch_buffer_[i].timestamp = base_ptr[i].timestamp;
                prefetch_buffer_[i].price = base_ptr[i].price;
                prefetch_buffer_[i].amount = base_ptr[i].amount;
                prefetch_buffer_[i].side = base_ptr[i].side;
                prefetch_buffer_[i].is_snapshot = (base_ptr[i].is_snapshot != 0);
            }
        }
        
        stream_buffer_pos_ += batch_size;
        buffer_valid_ = batch_size;
    }
    
    update = prefetch_buffer_[buffer_index_++];
    return true;
}

bool parser::MarketDataParser::refill_buffer_simd()
{
    using namespace hwy::HWY_NAMESPACE;
    
    buffer_index_ = 0;
    buffer_valid_ = 0;

    if (current_record_ >= total_records_) {
        return false;
    }

    size_t remaining = total_records_ - current_record_;
    size_t batch_size = (remaining >= SIMD_BATCH) ? SIMD_BATCH : remaining;

    const L2UpdateBinary* base_ptr = 
        reinterpret_cast<const L2UpdateBinary*>(mapped_data_ + current_record_ * sizeof(L2UpdateBinary));

    if (batch_size == SIMD_BATCH) {
        const ScalableTag<double> d;
        const ScalableTag<int64_t> di64;
        
        auto prices_01 = LoadU(d, &base_ptr[0].price);
        auto prices_23 = LoadU(d, &base_ptr[2].price);
        StoreU(prices_01, d, &prefetch_buffer_[0].price);
        StoreU(prices_23, d, &prefetch_buffer_[2].price);
        
        auto amounts_01 = LoadU(d, &base_ptr[0].amount);
        auto amounts_23 = LoadU(d, &base_ptr[2].amount);
        StoreU(amounts_01, d, &prefetch_buffer_[0].amount);
        StoreU(amounts_23, d, &prefetch_buffer_[2].amount);
        
        auto ts_01 = LoadU(di64, &base_ptr[0].timestamp);
        auto ts_23 = LoadU(di64, &base_ptr[2].timestamp);
        StoreU(ts_01, di64, &prefetch_buffer_[0].timestamp);
        StoreU(ts_23, di64, &prefetch_buffer_[2].timestamp);
        
        for (size_t i = 0; i < SIMD_BATCH; ++i) {
            prefetch_buffer_[i].side = base_ptr[i].side;
            prefetch_buffer_[i].is_snapshot = (base_ptr[i].is_snapshot != 0);
        }
    } else {
        for (size_t i = 0; i < batch_size; ++i) {
            prefetch_buffer_[i].timestamp = base_ptr[i].timestamp;
            prefetch_buffer_[i].price = base_ptr[i].price;
            prefetch_buffer_[i].amount = base_ptr[i].amount;
            prefetch_buffer_[i].side = base_ptr[i].side;
            prefetch_buffer_[i].is_snapshot = (base_ptr[i].is_snapshot != 0);
        }
    }

    current_record_ += batch_size;
    buffer_valid_ = batch_size;
    return true;
}

bool parser::MarketDataParser::parse_next_csv(L2Update& update)
{
    if (!header_parsed_) {
        if (!gzgets(gz_file_, line_buffer_, sizeof(line_buffer_))) {
            return false;
        }
        header_parsed_ = true;
    }

    if (!gzgets(gz_file_, line_buffer_, sizeof(line_buffer_))) {
        return false;
    }

    // Parse CSV: timestamp,is_snapshot,side,price,amount
    char* token = strtok(line_buffer_, ",");
    if (!token) return false;
    update.timestamp = std::stoll(token);

    token = strtok(nullptr, ",");
    if (!token) return false;
    update.is_snapshot = (std::string(token) == "true");

    token = strtok(nullptr, ",");
    if (!token) return false;
    update.side = token[0];

    token = strtok(nullptr, ",");
    if (!token) return false;
    update.price = std::stod(token);

    token = strtok(nullptr, ",\n");
    if (!token) return false;
    update.amount = std::stod(token);

    return true;
}
