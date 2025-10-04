/*
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

#ifndef avro_BufferReader_hh__
#define avro_BufferReader_hh__

#include "Buffer.hh"
#include <type_traits>

#ifdef min
#undef min
#endif
/**
 * \file BufferReader.hh
 *
 * \brief Helper class for reading bytes from buffer in a streaming manner,
 * without the overhead of istreams.
 *
 **/

namespace avro {

/**
 * Helper class for reading bytes from buffer without worrying about
 * chunk boundaries.  May read from an InputBuffer or OutputBuffer.
 *
 **/
class AVRO_DECL BufferReader : private boost::noncopyable {

public:
    typedef detail::data_type data_type;
    typedef detail::size_type size_type;

private:
    size_type chunkRemaining() const {
        return iter_->dataSize() - chunkPos_;
    }

    void incrementChunk(size_type howMuch) {
        bytesRemaining_ -= howMuch;
        chunkPos_ += howMuch;
        if (chunkPos_ == iter_->dataSize()) {
            chunkPos_ = 0;
            ++iter_;
        }
    }

    void rewind() {
        iter_ = bufferImpl_->beginRead();
        bytesRemaining_ = bytes_;
        chunkPos_ = 0;
    }

    const data_type *addr() const {
        return iter_->tellReadPos() + chunkPos_;
    }

public:
    explicit BufferReader(const InputBuffer &buf) : bufferImpl_(buf.pimpl_),
                                                    iter_(bufferImpl_->beginRead()),
                                                    bytes_(bufferImpl_->size()),
                                                    bytesRemaining_(bytes_),
                                                    chunkPos_(0) {}

    explicit BufferReader(const OutputBuffer &buf) : bufferImpl_(buf.pimpl_),
                                                     iter_(bufferImpl_->beginRead()),
                                                     bytes_(bufferImpl_->size()),
                                                     bytesRemaining_(bytes_),
                                                     chunkPos_(0) {}

    /**
     * How many bytes are still not read from this buffer.
     **/

    size_type bytesRemaining() const {
        return bytesRemaining_;
    }

    /**
     * Read a block of data from the front of the buffer.
     **/

    size_type bytesRead() const {
        return bytes_ - bytesRemaining_;
    }

    /**
     * Read a block of data from the buffer.
     **/

    size_type read(data_type *data, size_type size) {

        if (size > bytesRemaining_) {
            size = bytesRemaining_;
        }
        size_type sizeToRead = size;

        while (sizeToRead) {
            const size_type toRead = std::min(sizeToRead, chunkRemaining());
            memcpy(data, addr(), toRead);
            sizeToRead -= toRead;
            data += toRead;
            incrementChunk(toRead);
        }

        return size;
    }

    /**
     * Read a block of data from the buffer.
     **/

    bool read(std::string &str, size_type size) {
        if (size > bytesRemaining_) {
            return false;
        }

        if (size <= chunkRemaining()) {
            fastStringRead(str, size);
        } else {
            slowStringRead(str, size);
        }

        return true;
    }

    /**
     * Read a single value from the buffer.  The value must be a "fundamental"
     * type, e.g. int, float, etc.  (otherwise use the other writeTo tests).
     *
     **/

    template<typename T>
    bool read(T &val) {
        return read(val, std::is_fundamental<T>());
    }

    /**
     * Skips a block of data from the buffer.
     **/

    bool skip(size_type bytes) {
        bool skipped = false;
        if (bytes <= bytesRemaining_) {
            doSkip(bytes);
            skipped = true;
        }
        return skipped;
    }

    /**
     * Seek to a position in the buffer.
     **/

    bool seek(size_type pos) {
        if (pos > bytes_) {
            return false;
        }

        size_type toSkip = pos;
        size_type curPos = bytesRead();
        // if the seek position is ahead, we can use skip to get there
        if (pos >= curPos) {
            toSkip -= curPos;
        }
        // if the seek position is ahead of the start of the chunk we can back up to
        // start of the chunk
        else if (pos >= (curPos - chunkPos_)) {
            curPos -= chunkPos_;
            bytesRemaining_ += chunkPos_;
            chunkPos_ = 0;
            toSkip -= curPos;
        } else {
            rewind();
        }
        doSkip(toSkip);
        return true;
    }

    bool peek(char &val) {
        bool ret = (bytesRemaining_ > 0);
        if (ret) {
            val = *(addr());
        }
        return ret;
    }

    InputBuffer copyData(size_type bytes) {
        if (bytes > bytesRemaining_) {
            // force no copy
            bytes = 0;
        }
        detail::BufferImpl::SharedPtr newImpl(new detail::BufferImpl);
        if (bytes) {
            bufferImpl_->copyData(*newImpl, iter_, chunkPos_, bytes);
            doSkip(bytes);
        }
        return InputBuffer(newImpl);
    }

private:
    void doSkip(size_type sizeToSkip) {

        while (sizeToSkip) {
            const size_type toSkip = std::min(sizeToSkip, chunkRemaining());
            sizeToSkip -= toSkip;
            incrementChunk(toSkip);
        }
    }

    template<typename T>
    bool read(T &val, const std::true_type &) {
        if (sizeof(T) > bytesRemaining_) {
            return false;
        }

        if (sizeof(T) <= chunkRemaining()) {
            val = *(reinterpret_cast<const T *>(addr()));
            incrementChunk(sizeof(T));
        } else {
            read(reinterpret_cast<data_type *>(&val), sizeof(T));
        }
        return true;
    }

    /// An uninstantiable function, that is if boost::is_fundamental check fails
    template<typename T>
    bool read(T &val, const std::false_type &) {
        static_assert(sizeof(T) == 0, "Not a valid type to read");
        return false;
    }

    void fastStringRead(std::string &str, size_type sizeToCopy) {
        str.assign(addr(), sizeToCopy);
        incrementChunk(sizeToCopy);
    }

    void slowStringRead(std::string &str, size_type sizeToCopy) {
        str.clear();
        str.reserve(sizeToCopy);
        while (sizeToCopy) {
            const size_type toCopy = std::min(sizeToCopy, chunkRemaining());
            str.append(addr(), toCopy);
            sizeToCopy -= toCopy;
            incrementChunk(toCopy);
        }
    }

    detail::BufferImpl::ConstSharedPtr bufferImpl_;
    detail::BufferImpl::ChunkList::const_iterator iter_;
    size_type bytes_;
    size_type bytesRemaining_;
    size_type chunkPos_;
};

} // namespace avro

#endif
