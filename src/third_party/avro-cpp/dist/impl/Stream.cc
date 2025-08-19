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
#include <vector>

namespace avro {

using std::vector;

class MemoryInputStream : public InputStream {
    const std::vector<uint8_t *> &data_;
    const size_t chunkSize_;
    const size_t size_;
    const size_t available_;
    size_t cur_;
    size_t curLen_;

    size_t maxLen() {
        size_t n = (cur_ == (size_ - 1)) ? available_ : chunkSize_;
        if (n == curLen_) {
            if (cur_ == (size_ - 1)) {
                return 0;
            }
            ++cur_;
            n = (cur_ == (size_ - 1)) ? available_ : chunkSize_;
            curLen_ = 0;
        }
        return n;
    }

public:
    MemoryInputStream(const std::vector<uint8_t *> &b,
                      size_t chunkSize, size_t available) : data_(b), chunkSize_(chunkSize), size_(b.size()),
                                                            available_(available), cur_(0), curLen_(0) {}

    bool next(const uint8_t **data, size_t *len) final {
        if (size_t n = maxLen()) {
            *data = data_[cur_] + curLen_;
            *len = n - curLen_;
            curLen_ = n;
            return true;
        }
        return false;
    }

    void backup(size_t len) final {
        curLen_ -= len;
    }

    void skip(size_t len) final {
        while (len > 0) {
            if (size_t n = maxLen()) {
                if ((curLen_ + len) < n) {
                    n = curLen_ + len;
                }
                len -= n - curLen_;
                curLen_ = n;
            } else {
                break;
            }
        }
    }

    size_t byteCount() const final {
        return cur_ * chunkSize_ + curLen_;
    }
};

class MemoryInputStream2 : public InputStream {
    const uint8_t *const data_;
    const size_t size_;
    size_t curLen_;

public:
    MemoryInputStream2(const uint8_t *data, size_t len)
        : data_(data), size_(len), curLen_(0) {}

    bool next(const uint8_t **data, size_t *len) final {
        if (curLen_ == size_) {
            return false;
        }
        *data = &data_[curLen_];
        *len = size_ - curLen_;
        curLen_ = size_;
        return true;
    }

    void backup(size_t len) final {
        curLen_ -= len;
    }

    void skip(size_t len) final {
        if (len > (size_ - curLen_)) {
            len = size_ - curLen_;
        }
        curLen_ += len;
    }

    size_t byteCount() const final {
        return curLen_;
    }
};

class MemoryOutputStream final : public OutputStream {
public:
    const size_t chunkSize_;
    std::vector<uint8_t *> data_;
    size_t available_;
    size_t byteCount_;

    explicit MemoryOutputStream(size_t chunkSize) : chunkSize_(chunkSize),
                                                    available_(0), byteCount_(0) {}
    ~MemoryOutputStream() final {
        for (std::vector<uint8_t *>::const_iterator it = data_.begin();
             it != data_.end(); ++it) {
            delete[] *it;
        }
    }

    bool next(uint8_t **data, size_t *len) final {
        if (available_ == 0) {
            data_.push_back(new uint8_t[chunkSize_]);
            available_ = chunkSize_;
        }
        *data = &data_.back()[chunkSize_ - available_];
        *len = available_;
        byteCount_ += available_;
        available_ = 0;
        return true;
    }

    void backup(size_t len) final {
        available_ += len;
        byteCount_ -= len;
    }

    uint64_t byteCount() const final {
        return byteCount_;
    }

    void flush() final {}
};

std::unique_ptr<OutputStream> memoryOutputStream(size_t chunkSize) {
    return std::unique_ptr<OutputStream>(new MemoryOutputStream(chunkSize));
}

std::unique_ptr<InputStream> memoryInputStream(const uint8_t *data, size_t len) {
    return std::unique_ptr<InputStream>(new MemoryInputStream2(data, len));
}

std::unique_ptr<InputStream> memoryInputStream(const OutputStream &source) {
    const auto &mos =
        dynamic_cast<const MemoryOutputStream &>(source);
    return (mos.data_.empty()) ? std::unique_ptr<InputStream>(new MemoryInputStream2(nullptr, 0)) : std::unique_ptr<InputStream>(new MemoryInputStream(mos.data_, mos.chunkSize_, (mos.chunkSize_ - mos.available_)));
}

std::shared_ptr<std::vector<uint8_t>> snapshot(const OutputStream &source) {
    const auto &mos =
        dynamic_cast<const MemoryOutputStream &>(source);
    std::shared_ptr<std::vector<uint8_t>> result(new std::vector<uint8_t>());
    size_t c = mos.byteCount_;
    result->reserve(mos.byteCount_);
    for (auto it = mos.data_.begin();
         it != mos.data_.end(); ++it) {
        size_t n = std::min(c, mos.chunkSize_);
        std::copy(*it, *it + n, std::back_inserter(*result));
        c -= n;
    }
    return result;
}

} // namespace avro
