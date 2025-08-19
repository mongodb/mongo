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

#ifndef avro_BufferDetail_hh__
#define avro_BufferDetail_hh__

#include <boost/function.hpp>
#include <boost/shared_array.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/static_assert.hpp>
#include <boost/utility.hpp>
#include <utility>
#ifdef HAVE_BOOST_ASIO
#include <boost/asio/buffer.hpp>
#endif
#include <cassert>
#include <deque>
#include <exception>

/**
 * \file BufferDetail.hh
 *
 * \brief The implementation details for the Buffer class.
 *
 **/

namespace avro {

namespace detail {

typedef char data_type;
typedef size_t size_type;
#ifdef HAVE_BOOST_ASIO
typedef boost::asio::const_buffer ConstAsioBuffer;
typedef boost::asio::mutable_buffer MutableAsioBuffer;
#endif

/// The size in bytes for blocks backing buffer chunks.
const size_type kMinBlockSize = 4096;
const size_type kMaxBlockSize = 16384;
const size_type kDefaultBlockSize = kMinBlockSize;

typedef boost::function<void(void)> free_func;

/**
 * Simple class to hold a functor that executes on delete
 **/
class CallOnDestroy {
public:
    explicit CallOnDestroy(free_func func) : func_(std::move(func)) {}
    ~CallOnDestroy() {
        if (func_) {
            func_();
        }
    }

private:
    free_func func_;
};

/**
 * \brief A chunk is the building block for buffers.
 *
 * A chunk is backed by a memory block, and internally it maintains information
 * about which area of the block it may use, and the portion of this area that
 * contains valid data.  More than one chunk may share the same underlying
 * block, but the areas should never overlap.  Chunk holds a shared pointer to
 * an array of bytes so that shared blocks are reference counted.
 *
 * When a chunk is copied, the copy shares the same underlying buffer, but the
 * copy receives its own copies of the start/cursor/end pointers, so each copy
 * can be manipulated independently.  This allows different buffers to share
 * the same non-overlapping parts of a chunk, or even overlapping parts of a
 * chunk if the situation arises.
 *
 **/

class Chunk {

public:
    /// Default constructor, allocates a new underlying block for this chunk.
    explicit Chunk(size_type size) : underlyingBlock_(new data_type[size]),
                                     readPos_(underlyingBlock_.get()),
                                     writePos_(readPos_),
                                     endPos_(readPos_ + size) {}

    /// Foreign buffer constructor, uses the supplied data for this chunk, and
    /// only for reading.
    Chunk(const data_type *data, size_type size, const free_func &func) : callOnDestroy_(new CallOnDestroy(func)),
                                                                          readPos_(const_cast<data_type *>(data)),
                                                                          writePos_(readPos_ + size),
                                                                          endPos_(writePos_) {}

private:
    // reference counted object will call a functor when it's destroyed
    boost::shared_ptr<CallOnDestroy> callOnDestroy_;

public:
    /// Remove readable bytes from the front of the chunk by advancing the
    /// chunk start position.
    void truncateFront(size_type howMuch) {
        readPos_ += howMuch;
        assert(readPos_ <= writePos_);
    }

    /// Remove readable bytes from the back of the chunk by moving the
    /// chunk cursor position.
    void truncateBack(size_type howMuch) {
        writePos_ -= howMuch;
        assert(readPos_ <= writePos_);
    }

    /// Tell the position the next byte may be written to.
    data_type *tellWritePos() const {
        return writePos_;
    }

    /// Tell the position of the first byte containing valid data.
    const data_type *tellReadPos() const {
        return readPos_;
    }

    /// After a write operation, increment the write position.
    void incrementCursor(size_type howMuch) {
        writePos_ += howMuch;
        assert(writePos_ <= endPos_);
    }

    /// Tell how many bytes of data were written to this chunk.
    size_type dataSize() const {
        return (writePos_ - readPos_);
    }

    /// Tell how many bytes this chunk has available to write to.
    size_type freeSize() const {
        return (endPos_ - writePos_);
    }

    /// Tell how many bytes of data this chunk can hold (used and free).
    size_type capacity() const {
        return (endPos_ - readPos_);
    }

private:
    friend bool operator==(const Chunk &lhs, const Chunk &rhs);
    friend bool operator!=(const Chunk &lhs, const Chunk &rhs);

    // more than one buffer can share an underlying block, so use SharedPtr
    boost::shared_array<data_type> underlyingBlock_;

    data_type *readPos_;  ///< The first readable byte in the block
    data_type *writePos_; ///< The end of written data and start of free space
    data_type *endPos_;   ///< Marks the end of the usable block area
};

/**
 * Compare underlying buffers and return true if they are equal
 **/
inline bool operator==(const Chunk &lhs, const Chunk &rhs) {
    return lhs.underlyingBlock_ == rhs.underlyingBlock_;
}

/**
 * Compare underlying buffers and return true if they are unequal
 **/
inline bool operator!=(const Chunk &lhs, const Chunk &rhs) {
    return lhs.underlyingBlock_ != rhs.underlyingBlock_;
}

/**
 * \brief Implementation details for Buffer class
 *
 * Internally, BufferImpl keeps two lists of chunks, one list consists entirely of
 * chunks containing data, and one list which contains chunks with free space.
 *
 *
 */

class BufferImpl : boost::noncopyable {

    /// Add a new chunk to the list of chunks for this buffer, growing the
    /// buffer by the default block size.
    void allocChunkChecked(size_type size = kDefaultBlockSize) {
        writeChunks_.push_back(Chunk(size));
        freeSpace_ += writeChunks_.back().freeSize();
    }

    /// Add a new chunk to the list of chunks for this buffer, growing the
    /// buffer by the requested size, but within the range of a minimum and
    /// maximum.
    void allocChunk(size_type size) {
        if (size < kMinBlockSize) {
            size = kMinBlockSize;
        } else if (size > kMaxBlockSize) {
            size = kMaxBlockSize;
        }
        allocChunkChecked(size);
    }

    /// Update the state of the chunks after a write operation.  This function
    /// ensures the chunk states are consistent with the write.
    void postWrite(size_type size) {

        // precondition to this function is that the writeChunk_.front()
        // contains the data that was just written, so make sure writeChunks_
        // is not empty:

        assert(size <= freeSpace_ && !writeChunks_.empty());

        // This is probably the one tricky part of BufferImpl.  The data that
        // was written now exists in writeChunks_.front().  Now we must make
        // sure that same data exists in readChunks_.back().
        //
        // There are two cases:
        //
        // 1. readChunks_.last() and writeChunk_.front() refer to the same
        // underlying block, in which case they both just need their cursor
        // updated to reflect the new state.
        //
        // 2. readChunk_.last() is not the same block as writeChunks_.front(),
        // in which case it should be, since the writeChunk.front() contains
        // the next bit of data that will be appended to readChunks_, and
        // therefore needs to be copied there so we can proceed with updating
        // their state.
        //

        // if readChunks_ is not the same as writeChunks_.front(), make a copy
        // of it there

        if (readChunks_.empty() || (readChunks_.back() != writeChunks_.front())) {
            const Chunk &curChunk = writeChunks_.front();
            readChunks_.push_back(curChunk);

            // Any data that existed in the write chunk previously doesn't
            // belong to this buffer (otherwise it would have already been
            // added to the readChunk_ list).  Here, adjust the start of the
            // readChunk to begin after any data already existing in curChunk

            readChunks_.back().truncateFront(curChunk.dataSize());
        }

        assert(readChunks_.back().freeSize() == writeChunks_.front().freeSize());

        // update the states of both readChunks_ and writeChunks_ to indicate that they are
        // holding the new data

        readChunks_.back().incrementCursor(size);
        writeChunks_.front().incrementCursor(size);
        size_ += size;
        freeSpace_ -= size;

        // if there is no more free space in writeChunks_, the next write cannot use
        // it, so dispose of it now

        if (writeChunks_.front().freeSize() == 0) {
            writeChunks_.pop_front();
        }
    }

public:
    typedef std::deque<Chunk> ChunkList;
    typedef boost::shared_ptr<BufferImpl> SharedPtr;
    typedef boost::shared_ptr<const BufferImpl> ConstSharedPtr;

    /// Default constructor, creates a buffer without any chunks
    BufferImpl() : freeSpace_(0),
                   size_(0) {}

    /// Copy constructor, gets a copy of all the chunks with data.
    BufferImpl(const BufferImpl &src) : readChunks_(src.readChunks_),
                                        freeSpace_(0),
                                        size_(src.size_) {}

    /// Amount of data held in this buffer.
    size_type size() const {
        return size_;
    }

    /// Capacity that may be written before the buffer must allocate more memory.
    size_type freeSpace() const {
        return freeSpace_;
    }

    /// Add enough free chunks to make the reservation size available.
    /// Actual amount may be more (rounded up to next chunk).
    void reserveFreeSpace(size_type reserveSize) {
        while (freeSpace_ < reserveSize) {
            allocChunk(reserveSize - freeSpace_);
        }
    }

    /// Return the chunk avro's begin iterator for reading.
    ChunkList::const_iterator beginRead() const {
        return readChunks_.begin();
    }

    /// Return the chunk avro's end iterator for reading.
    ChunkList::const_iterator endRead() const {
        return readChunks_.end();
    }

    /// Return the chunk avro's begin iterator for writing.
    ChunkList::const_iterator beginWrite() const {
        return writeChunks_.begin();
    }

    /// Return the chunk avro's end iterator for writing.
    ChunkList::const_iterator endWrite() const {
        return writeChunks_.end();
    }

    /// Write a single value to buffer, add a new chunk if necessary.
    template<typename T>
    void writeTo(T val, const std::true_type &) {
        if (freeSpace_ && (sizeof(T) <= writeChunks_.front().freeSize())) {
            // fast path, there's enough room in the writeable chunk to just
            // straight out copy it
            *(reinterpret_cast<T *>(writeChunks_.front().tellWritePos())) = val;
            postWrite(sizeof(T));
        } else {
            // need to fixup chunks first, so use the regular memcpy
            // writeTo method
            writeTo(reinterpret_cast<data_type *>(&val), sizeof(T));
        }
    }

    /// An uninstantiable function, this is if boost::is_fundamental check fails,
    /// and will compile-time assert.
    template<typename T>
    void writeTo(T /*val*/, const std::false_type &) {
        BOOST_STATIC_ASSERT(sizeof(T) == 0);
    }

    /// Write a block of data to the buffer, adding new chunks if necessary.
    size_type writeTo(const data_type *data, size_type size) {
        size_type bytesLeft = size;
        while (bytesLeft) {

            if (freeSpace_ == 0) {
                allocChunkChecked();
            }

            Chunk &chunk = writeChunks_.front();
            size_type toCopy = std::min<size_type>(chunk.freeSize(), bytesLeft);
            assert(toCopy);
            memcpy(chunk.tellWritePos(), data, toCopy);
            postWrite(toCopy);
            data += toCopy;
            bytesLeft -= toCopy;
        }
        return size;
    }

    /// Update internal status of chunks after data is written using iterator.
    size_type wroteTo(size_type size) {
        assert(size <= freeSpace_);
        size_type bytesLeft = size;
        while (bytesLeft) {

            Chunk &chunk = writeChunks_.front();
            size_type wrote = std::min<size_type>(chunk.freeSize(), bytesLeft);
            assert(wrote);
            postWrite(wrote);
            bytesLeft -= wrote;
        }
        return size;
    }

    /// Append the chunks that have data in src to this buffer
    void append(const BufferImpl &src) {
        std::copy(src.readChunks_.begin(), src.readChunks_.end(), std::back_inserter(readChunks_));
        size_ += src.size_;
    }

    /// Remove all the chunks that contain data from this buffer.
    void discardData() {
        readChunks_.clear();
        size_ = 0;
    }

    /// Remove the specified amount of data from the chunks, starting at the front.
    void discardData(size_type bytes) {
        assert(bytes && bytes <= size_);

        size_type bytesToDiscard = bytes;
        while (bytesToDiscard) {

            size_t currentSize = readChunks_.front().dataSize();

            // see if entire chunk is discarded
            if (currentSize <= bytesToDiscard) {
                readChunks_.pop_front();
                bytesToDiscard -= currentSize;
            } else {
                readChunks_.front().truncateFront(bytesToDiscard);
                bytesToDiscard = 0;
            }
        }

        size_ -= bytes;
    }

    /// Remove the specified amount of data from the chunks, moving the
    /// data to dest's chunks
    void extractData(BufferImpl &dest, size_type bytes) {
        assert(bytes && bytes <= size_);

        size_type bytesToExtract = bytes;
        while (bytesToExtract) {

            size_t currentSize = readChunks_.front().dataSize();
            dest.readChunks_.push_back(readChunks_.front());

            // see if entire chunk was extracted
            if (currentSize <= bytesToExtract) {
                readChunks_.pop_front();
                bytesToExtract -= currentSize;
            } else {
                readChunks_.front().truncateFront(bytesToExtract);
                size_t excess = currentSize - bytesToExtract;
                dest.readChunks_.back().truncateBack(excess);
                bytesToExtract = 0;
            }
        }

        size_ -= bytes;
        dest.size_ += bytes;
    }

    /// Move data from this to the destination, leaving this buffer without data
    void extractData(BufferImpl &dest) {
        assert(dest.readChunks_.empty());
        dest.readChunks_.swap(readChunks_);
        dest.size_ = size_;
        size_ = 0;
    }

    /// Copy data to a different buffer by copying the chunks.  It's
    /// a bit like extract, but without modifying the source buffer.
    static void copyData(BufferImpl &dest,
                         ChunkList::const_iterator iter,
                         size_type offset,
                         size_type bytes) {
        // now we are positioned to start the copying, copy as many
        // chunks as we need, the first chunk may have a non-zero offset
        // if the data to copy is not at the start of the chunk
        size_type copied = 0;
        while (copied < bytes) {

            dest.readChunks_.push_back(*iter);

            // offset only applies in the first chunk,
            // all subsequent chunks are copied from the start
            dest.readChunks_.back().truncateFront(offset);
            offset = 0;

            copied += dest.readChunks_.back().dataSize();
            ++iter;
        }

        // if the last chunk copied has more bytes than we need, truncate it
        size_type excess = copied - bytes;
        dest.readChunks_.back().truncateBack(excess);

        dest.size_ += bytes;
    }

    /// The number of chunks containing data.  Used for debugging.
    size_t numDataChunks() const {
        return readChunks_.size();
    }

    /// The number of chunks containing free space (note that an entire chunk
    /// may not be free).  Used for debugging.
    size_t numFreeChunks() const {
        return writeChunks_.size();
    }

    /// Add unmanaged data to the buffer.  The buffer will not automatically
    /// free the data, but it will call the supplied function when the data is
    /// no longer referenced by the buffer (or copies of the buffer).
    void appendForeignData(const data_type *data, size_type size, const free_func &func) {
        readChunks_.push_back(Chunk(data, size, func));
        size_ += size;
    }
    BufferImpl &operator=(const BufferImpl &src) = delete;

private:
    ChunkList readChunks_;  ///< chunks of this buffer containing data
    ChunkList writeChunks_; ///< chunks of this buffer containing free space

    size_type freeSpace_; ///< capacity of buffer before allocation required
    size_type size_;      ///< amount of data in buffer
};

} // namespace detail

} // namespace avro

#endif
