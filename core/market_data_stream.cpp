#include "market_data_stream.h"
#include <stdexcept>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>

// Portable SIMD for binary parsing
#include "hwy/highway.h"

namespace stream {

// Read mode constructor
L2Stream::L2Stream(const std::string& filepath, bool streaming)
    : mode_(StreamMode::Read), format_(Format::UNKNOWN), fd_(-1), mapped_data_(nullptr), file_size_(0),
      total_records_(0), current_record_(0), buffer_index_(0), buffer_valid_(0),
      use_streaming_(streaming), stream_buffer_pos_(0), stream_buffer_valid_(0), file_position_(0),
      gz_file_(nullptr), header_parsed_(false),
      write_file_(nullptr), write_gz_file_(nullptr), write_binary_(false), csv_header_written_(false)
{
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

// Write mode constructor
L2Stream::L2Stream(const std::string& filepath, StreamMode mode)
    : mode_(StreamMode::Write), format_(Format::UNKNOWN), fd_(-1), mapped_data_(nullptr), file_size_(0),
      total_records_(0), current_record_(0), buffer_index_(0), buffer_valid_(0),
      use_streaming_(false), stream_buffer_pos_(0), stream_buffer_valid_(0), file_position_(0),
      gz_file_(nullptr), header_parsed_(false),
      write_file_(nullptr), write_gz_file_(nullptr), write_binary_(false), csv_header_written_(false)
{
    std::memset(prefetch_buffer_, 0, sizeof(prefetch_buffer_));
    std::memset(line_buffer_, 0, sizeof(line_buffer_));
    (void)mode;
    init_write(filepath);
}

L2Stream::~L2Stream()
{
    if (mode_ == StreamMode::Write) {
        flush();
        if (write_file_) {
            std::fclose(write_file_);
            write_file_ = nullptr;
        }
        if (write_gz_file_) {
            gzclose(write_gz_file_);
            write_gz_file_ = nullptr;
        }
        return;
    }

    if (format_ == Format::BINARY) {
        if (use_streaming_) {
            if (fd_ != -1) {
                close(fd_);
                fd_ = -1;
            }
            stream_buffer_.clear();
        } else {
            if (mapped_data_ && mapped_data_ != MAP_FAILED) {
                munmap(const_cast<char*>(mapped_data_), file_size_);
                mapped_data_ = nullptr;
            }
            if (fd_ != -1) {
                close(fd_);
                fd_ = -1;
            }
        }
        file_size_ = 0;
        current_record_ = 0;
        total_records_ = 0;
        buffer_index_ = 0;
        buffer_valid_ = 0;
        stream_buffer_pos_ = 0;
        stream_buffer_valid_ = 0;
        file_position_ = 0;
    } else if (gz_file_) {
        gzclose(gz_file_);
        gz_file_ = nullptr;
        header_parsed_ = false;
    }
    format_ = Format::UNKNOWN;
}

bool L2Stream::parse_next(L2Update& update)
{
    if (mode_ != StreamMode::Read) return false;
    if (format_ == Format::BINARY) {
        return parse_next_binary(update);
    } else {
        return parse_next_csv(update);
    }
}

bool L2Stream::is_open() const
{
    if (mode_ == StreamMode::Write) {
        return write_file_ != nullptr || write_gz_file_ != nullptr;
    }
    return (format_ == Format::BINARY && (mapped_data_ != nullptr || fd_ != -1)) ||
           (format_ != Format::BINARY && gz_file_ != nullptr);
}

bool L2Stream::write(const L2Update& update)
{
    if (mode_ != StreamMode::Write) return false;
    if (write_binary_) {
        L2UpdateBinary bin;
        bin.timestamp = update.timestamp;
        bin.price = update.price;
        bin.amount = update.amount;
        bin.side = update.side;
        bin.is_snapshot = update.is_snapshot ? 1 : 0;
        std::memset(bin.padding, 0, sizeof(bin.padding));
        write_buffer_.push_back(bin);
        if (write_buffer_.size() >= WRITE_BUFFER_SIZE) {
            flush_write_buffer();
        }
        return true;
    } else {
        if (!csv_header_written_) {
            if (write_gz_file_) {
                gzprintf(write_gz_file_, "timestamp,is_snapshot,side,price,amount\n");
            } else if (write_file_) {
                std::fprintf(write_file_, "timestamp,is_snapshot,side,price,amount\n");
            }
            csv_header_written_ = true;
        }
        const char* snap_str = update.is_snapshot ? "true" : "false";
        if (write_gz_file_) {
            if (gzprintf(write_gz_file_, "%lld,%s,%c,%.*g,%.*g\n",
                         (long long)update.timestamp, snap_str, update.side,
                         (int)(sizeof(double) * 2 + 2), update.price,
                         (int)(sizeof(double) * 2 + 2), update.amount) <= 0)
                return false;
        } else if (write_file_) {
            if (std::fprintf(write_file_, "%lld,%s,%c,%.*g,%.*g\n",
                             (long long)update.timestamp, snap_str, update.side,
                             (int)(sizeof(double) * 2 + 2), update.price,
                             (int)(sizeof(double) * 2 + 2), update.amount) < 0)
                return false;
        }
        return true;
    }
}

void L2Stream::flush()
{
    if (mode_ != StreamMode::Write) return;
    flush_write_buffer();
    if (write_file_) std::fflush(write_file_);
    if (write_gz_file_) gzflush(write_gz_file_, Z_SYNC_FLUSH);
}

L2Stream::Format L2Stream::detect_format(const std::string& filepath)
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

void L2Stream::init_binary_mmap(const std::string& filepath)
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
    mapped_data_ = static_cast<const char*>(mmap(nullptr, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (mapped_data_ == MAP_FAILED) {
        close(fd_);
        throw std::runtime_error("Failed to mmap file");
    }
    madvise(const_cast<char*>(mapped_data_), file_size_, MADV_SEQUENTIAL);
}

void L2Stream::init_binary_streaming(const std::string& filepath)
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
    size_t buffer_records = STREAM_BUFFER_SIZE / sizeof(L2UpdateBinary);
    stream_buffer_.resize(buffer_records);
    refill_stream_buffer();
}

void L2Stream::init_csv(const std::string& filepath)
{
    gz_file_ = gzopen(filepath.c_str(), "rb");
    if (!gz_file_) {
        throw std::runtime_error("Failed to open CSV file: " + filepath);
    }
}

void L2Stream::init_write(const std::string& filepath)
{
    format_ = detect_format(filepath);
    if (format_ == Format::BINARY) {
        write_binary_ = true;
        write_file_ = std::fopen(filepath.c_str(), "wb");
        if (!write_file_) {
            throw std::runtime_error("Failed to open for write: " + filepath);
        }
        write_buffer_.reserve(WRITE_BUFFER_SIZE);
    } else if (format_ == Format::CSV_GZ) {
        write_binary_ = false;
        write_gz_file_ = gzopen(filepath.c_str(), "wb");
        if (!write_gz_file_) {
            throw std::runtime_error("Failed to open for write: " + filepath);
        }
    } else if (format_ == Format::CSV) {
        write_binary_ = false;
        write_file_ = std::fopen(filepath.c_str(), "w");
        if (!write_file_) {
            throw std::runtime_error("Failed to open for write: " + filepath);
        }
    } else {
        throw std::runtime_error("Unsupported write format: " + filepath);
    }
}

void L2Stream::flush_write_buffer()
{
    if (write_buffer_.empty()) return;
    if (write_file_) {
        std::fwrite(write_buffer_.data(), sizeof(L2UpdateBinary), write_buffer_.size(), write_file_);
    }
    write_buffer_.clear();
}

bool L2Stream::parse_next_binary(L2Update& update)
{
    if (use_streaming_) {
        return parse_next_binary_streaming(update);
    }
    if (buffer_index_ >= buffer_valid_) {
        if (!refill_buffer_simd()) {
            return false;
        }
    }
    update = prefetch_buffer_[buffer_index_++];
    return true;
}

bool L2Stream::refill_stream_buffer()
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

bool L2Stream::parse_next_binary_streaming(L2Update& update)
{
    if (stream_buffer_pos_ >= stream_buffer_valid_) {
        if (!refill_stream_buffer()) {
            return false;
        }
    }
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

bool L2Stream::refill_buffer_simd()
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

bool L2Stream::parse_next_csv(L2Update& update)
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

} // namespace stream
