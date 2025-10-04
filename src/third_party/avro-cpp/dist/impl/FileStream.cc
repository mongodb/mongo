/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Stream.hh"
#include <fstream>
#ifndef _WIN32
#include "fcntl.h"
#include "unistd.h"
#include <cerrno>

#ifndef O_BINARY
#define O_BINARY 0
#endif
#else
#include "Windows.h"

#ifdef min
#undef min
#endif
#endif

using std::istream;
using std::ostream;
using std::unique_ptr;

namespace avro {
namespace {
struct BufferCopyIn {
    virtual ~BufferCopyIn() = default;
    virtual void seek(size_t len) = 0;
    virtual bool read(uint8_t *b, size_t toRead, size_t &actual) = 0;
};

struct FileBufferCopyIn : public BufferCopyIn {
#ifdef _WIN32
    HANDLE h_;
    explicit FileBufferCopyIn(const char *filename) : h_(::CreateFileA(filename, GENERIC_READ, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) {
        if (h_ == INVALID_HANDLE_VALUE) {
            throw Exception("Cannot open file: {}", ::GetLastError());
        }
    }

    ~FileBufferCopyIn() {
        ::CloseHandle(h_);
    }

    void seek(size_t len) override {
        if (::SetFilePointer(h_, len, NULL, FILE_CURRENT) == INVALID_SET_FILE_POINTER && ::GetLastError() != NO_ERROR) {
            throw Exception("Cannot skip file: {}", ::GetLastError());
        }
    }

    bool read(uint8_t *b, size_t toRead, size_t &actual) override {
        DWORD dw = 0;
        if (!::ReadFile(h_, b, toRead, &dw, NULL)) {
            throw Exception("Cannot read file: {}", ::GetLastError());
        }
        actual = static_cast<size_t>(dw);
        return actual != 0;
    }
#else
    const int fd_;

    explicit FileBufferCopyIn(const char *filename) : fd_(open(filename, O_RDONLY | O_BINARY)) {
        if (fd_ < 0) {
            throw Exception("Cannot open file: {}", strerror(errno));
        }
    }

    ~FileBufferCopyIn() override {
        ::close(fd_);
    }

    void seek(size_t len) final {
        off_t r = ::lseek(fd_, len, SEEK_CUR);
        if (r == static_cast<off_t>(-1)) {
            throw Exception("Cannot skip file: {}", strerror(errno));
        }
    }

    bool read(uint8_t *b, size_t toRead, size_t &actual) final {
        auto n = ::read(fd_, b, toRead);
        if (n > 0) {
            actual = n;
            return true;
        }
        return false;
    }
#endif
};

struct IStreamBufferCopyIn : public BufferCopyIn {
    istream &is_;

    explicit IStreamBufferCopyIn(istream &is) : is_(is) {
    }

    void seek(size_t len) override {
        if (!is_.seekg(len, std::ios_base::cur)) {
            throw Exception("Cannot skip stream");
        }
    }

    bool read(uint8_t *b, size_t toRead, size_t &actual) override {
        is_.read(reinterpret_cast<char *>(b), toRead);
        if (is_.bad()) {
            return false;
        }
        actual = static_cast<size_t>(is_.gcount());
        return (!is_.eof() || actual != 0);
    }
};

struct NonSeekableIStreamBufferCopyIn : public IStreamBufferCopyIn {
    explicit NonSeekableIStreamBufferCopyIn(istream &is) : IStreamBufferCopyIn(is) {}

    void seek(size_t len) final {
        const size_t bufSize = 4096;
        uint8_t buf[bufSize];
        while (len > 0) {
            size_t n = std::min(len, bufSize);
            is_.read(reinterpret_cast<char *>(buf), n);
            if (is_.bad()) {
                throw Exception("Cannot skip stream");
            }
            auto actual = static_cast<size_t>(is_.gcount());
            if (is_.eof() && actual == 0) {
                throw Exception("Cannot skip stream");
            }
            len -= n;
        }
    }
};

} // namespace

class BufferCopyInInputStream : public SeekableInputStream {
    const size_t bufferSize_;
    uint8_t *const buffer_;
    unique_ptr<BufferCopyIn> in_;
    size_t byteCount_;
    uint8_t *next_;
    size_t available_;

    bool next(const uint8_t **data, size_t *size) final {
        if (available_ == 0 && !fill()) {
            return false;
        }
        *data = next_;
        *size = available_;
        next_ += available_;
        byteCount_ += available_;
        available_ = 0;
        return true;
    }

    void backup(size_t len) final {
        next_ -= len;
        available_ += len;
        byteCount_ -= len;
    }

    void skip(size_t len) final {
        while (len > 0) {
            if (available_ == 0) {
                in_->seek(len);
                byteCount_ += len;
                return;
            }
            size_t n = std::min(available_, len);
            available_ -= n;
            next_ += n;
            len -= n;
            byteCount_ += n;
        }
    }

    size_t byteCount() const final { return byteCount_; }

    bool fill() {
        size_t n = 0;
        if (in_->read(buffer_, bufferSize_, n)) {
            next_ = buffer_;
            available_ = n;
            return true;
        }
        return false;
    }

    void seek(int64_t position) final {
        // BufferCopyIn::seek is relative to byteCount_, whereas position is
        // absolute.
        in_->seek(position - byteCount_ - available_);
        byteCount_ = position;
        available_ = 0;
    }

public:
    BufferCopyInInputStream(unique_ptr<BufferCopyIn> in, size_t bufferSize) : bufferSize_(bufferSize),
                                                                              buffer_(new uint8_t[bufferSize]),
                                                                              in_(std::move(in)),
                                                                              byteCount_(0),
                                                                              next_(buffer_),
                                                                              available_(0) {}

    ~BufferCopyInInputStream() override {
        delete[] buffer_;
    }
};

namespace {
struct BufferCopyOut {
    virtual ~BufferCopyOut() = default;
    virtual void write(const uint8_t *b, size_t len) = 0;
};

struct FileBufferCopyOut : public BufferCopyOut {
#ifdef _WIN32
    HANDLE h_;
    explicit FileBufferCopyOut(const char *filename) : h_(::CreateFileA(filename, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) {
        if (h_ == INVALID_HANDLE_VALUE) {
            throw Exception("Cannot open file: {}", ::GetLastError());
        }
    }

    ~FileBufferCopyOut() {
        ::CloseHandle(h_);
    }

    void write(const uint8_t *b, size_t len) override {
        while (len > 0) {
            DWORD dw = 0;
            if (!::WriteFile(h_, b, len, &dw, NULL)) {
                throw Exception("Cannot read file: {}", ::GetLastError());
            }
            b += dw;
            len -= dw;
        }
    }
#else
    const int fd_;

    explicit FileBufferCopyOut(const char *filename) : fd_(::open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644)) {

        if (fd_ < 0) {
            throw Exception("Cannot open file: {}", ::strerror(errno));
        }
    }

    ~FileBufferCopyOut() override {
        ::close(fd_);
    }

    void write(const uint8_t *b, size_t len) final {
        if (::write(fd_, b, len) < 0) {
            throw Exception("Cannot write file: {}", ::strerror(errno));
        }
    }
#endif
};

struct OStreamBufferCopyOut : public BufferCopyOut {
    ostream &os_;

    explicit OStreamBufferCopyOut(ostream &os) : os_(os) {
    }

    void write(const uint8_t *b, size_t len) final {
        os_.write(reinterpret_cast<const char *>(b), len);
    }
};

} // namespace

class BufferCopyOutputStream : public OutputStream {
    size_t bufferSize_;
    uint8_t *const buffer_;
    unique_ptr<BufferCopyOut> out_;
    uint8_t *next_;
    size_t available_;
    size_t byteCount_;

    // Invariant: byteCount_ == bytesWritten + bufferSize_ - available_;
    bool next(uint8_t **data, size_t *len) final {
        if (available_ == 0) {
            flush();
        }
        *data = next_;
        *len = available_;
        next_ += available_;
        byteCount_ += available_;
        available_ = 0;
        return true;
    }

    void backup(size_t len) final {
        available_ += len;
        next_ -= len;
        byteCount_ -= len;
    }

    uint64_t byteCount() const final {
        return byteCount_;
    }

    void flush() final {
        out_->write(buffer_, bufferSize_ - available_);
        next_ = buffer_;
        available_ = bufferSize_;
    }

public:
    BufferCopyOutputStream(unique_ptr<BufferCopyOut> out, size_t bufferSize) : bufferSize_(bufferSize),
                                                                               buffer_(new uint8_t[bufferSize]),
                                                                               out_(std::move(out)),
                                                                               next_(buffer_),
                                                                               available_(bufferSize_), byteCount_(0) {}

    ~BufferCopyOutputStream() override {
        delete[] buffer_;
    }
};

unique_ptr<InputStream> fileInputStream(const char *filename,
                                        size_t bufferSize) {
    unique_ptr<BufferCopyIn> in(new FileBufferCopyIn(filename));
    return unique_ptr<InputStream>(new BufferCopyInInputStream(std::move(in), bufferSize));
}

unique_ptr<SeekableInputStream> fileSeekableInputStream(const char *filename,
                                                        size_t bufferSize) {
    unique_ptr<BufferCopyIn> in(new FileBufferCopyIn(filename));
    return unique_ptr<SeekableInputStream>(new BufferCopyInInputStream(std::move(in),
                                                                       bufferSize));
}

unique_ptr<InputStream> istreamInputStream(istream &is, size_t bufferSize) {
    unique_ptr<BufferCopyIn> in(new IStreamBufferCopyIn(is));
    return unique_ptr<InputStream>(new BufferCopyInInputStream(std::move(in), bufferSize));
}

unique_ptr<InputStream> nonSeekableIstreamInputStream(
    istream &is, size_t bufferSize) {
    unique_ptr<BufferCopyIn> in(new NonSeekableIStreamBufferCopyIn(is));
    return unique_ptr<InputStream>(new BufferCopyInInputStream(std::move(in), bufferSize));
}

unique_ptr<OutputStream> fileOutputStream(const char *filename,
                                          size_t bufferSize) {
    unique_ptr<BufferCopyOut> out(new FileBufferCopyOut(filename));
    return unique_ptr<OutputStream>(new BufferCopyOutputStream(std::move(out), bufferSize));
}

unique_ptr<OutputStream> ostreamOutputStream(ostream &os,
                                             size_t bufferSize) {
    unique_ptr<BufferCopyOut> out(new OStreamBufferCopyOut(os));
    return unique_ptr<OutputStream>(new BufferCopyOutputStream(std::move(out), bufferSize));
}

} // namespace avro
