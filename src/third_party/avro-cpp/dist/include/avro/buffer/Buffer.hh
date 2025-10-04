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

#ifndef avro_Buffer_hh__
#define avro_Buffer_hh__

#ifndef _WIN32
#include <sys/uio.h>
#endif
#include <utility>
#include <vector>

#include "../Config.hh"
#include "detail/BufferDetail.hh"
#include "detail/BufferDetailIterator.hh"

/**
 * \file Buffer.hh
 *
 * \brief Definitions for InputBuffer and OutputBuffer classes
 *
 **/

namespace avro {

class OutputBuffer;
class InputBuffer;

/**
 * The OutputBuffer (write-only buffer)
 *
 * Use cases for OutputBuffer
 *
 * - write message to buffer using ostream class or directly
 * - append messages to headers
 * - building up streams of messages via append
 * - converting to read-only buffers for sending
 * - extracting parts of the messages into read-only buffers
 *
 * -# ASIO access:
 *     - write to a buffer(s) by asio using iterator
 *     - convert to read buffer for deserializing
 *
 * OutputBuffer is assignable and copy-constructable.  On copy or assignment,
 * only a pointer is copied, so the two resulting copies are identical, so
 * modifying one will modify both.
 **/

class AVRO_DECL OutputBuffer {

public:
    typedef detail::size_type size_type;
    typedef detail::data_type data_type;

    /**
     * The asio library expects a const_iterator (the const-ness refers to the
     * fact that the underlying avro of buffers will not be modified, even
     * though the data in those buffers is being modified).  The iterator
     * provides the list of addresses an operation can write to.
     **/

    typedef detail::OutputBufferIterator const_iterator;

    /**
     * Default constructor.  Will pre-allocate at least the requested size, but
     * can grow larger on demand.
     *
     * Destructor uses the default, which resets a shared pointer, deleting the
     * underlying data if no other copies of exist.
     *
     * Copy and assignment operators are not explicitly provided because the
     * default ones work fine.  The default makes only a shallow copy, so the
     * copies will refer to the same memory.  This is required by asio
     * functions, which will implicitly make copies for asynchronous
     * operations.  Therefore, the user must be careful that if they create
     * multiple copies of the same OutputBuffer, only one is being modified
     * otherwise undefined behavior may occur.
     *
     **/

    explicit OutputBuffer(size_type reserveSize = 0) : pimpl_(new detail::BufferImpl) {
        if (reserveSize) {
            reserve(reserveSize);
        }
    }

    /**
     * Reserve enough space for a wroteTo() operation.  When using writeTo(),
     * the buffer will grow dynamically as needed.  But when using the iterator
     * to write (followed by wroteTo()), data may only be written to the space
     * available,  so this ensures there is enough room in the buffer before
     * the write operation.
     **/

    void reserve(size_type reserveSize) {
        pimpl_->reserveFreeSpace(reserveSize);
    }

    /**
     * Write a block of data to the buffer.  The buffer size will automatically
     * grow if the size is larger than what is currently free.
     **/

    size_type writeTo(const data_type *data, size_type size) {
        return pimpl_->writeTo(data, size);
    }

    /**
     * Write a single value to the buffer. The buffer size will automatically
     * grow if there is not room for the byte.  The value must be a
     * "fundamental" type, e.g. int, float, etc.  (otherwise use the other
     * writeTo tests).
     **/

    template<typename T>
    void writeTo(T val) {
        pimpl_->writeTo(val, std::is_fundamental<T>());
    }

    /**
     * Update the state of the buffer after writing through the iterator
     * interface.  This function exists primarily for the boost:asio which
     * writes directly to the buffer using its iterator.  In this case, the
     * internal state of the buffer does not reflect that the data was written
     * This informs the buffer how much data was written.
     *
     * The buffer does not automatically resize in this case, the bytes written
     * cannot exceed the amount of free space.  Attempting to write more will
     * throw a std::length_error exception.
     **/

    size_type wroteTo(size_type size) {
        size_type wrote = 0;
        if (size) {
            if (size > freeSpace()) {
                throw std::length_error("Impossible to write more data than free space");
            }
            wrote = pimpl_->wroteTo(size);
        }
        return wrote;
    }

    /**
     * Does the buffer have any data?
     **/

    bool empty() const {
        return (pimpl_->size() == 0);
    }

    /**
     *  Returns the size of the buffer, in bytes.
     */

    size_type size() const {
        return pimpl_->size();
    }

    /**
     * Returns the current free space that is available to write to in the
     * buffer, in bytes.  This is not a strict limit in size, as writeTo() can
     * automatically increase capacity if necessary.
     **/

    size_type freeSpace() const {
        return pimpl_->freeSpace();
    }

    /**
     * Appends the data in the argument to the end of this buffer.  The
     * argument can be either an InputBuffer or OutputBuffer.
     *
     **/

    template<class BufferType>
    void append(const BufferType &buf) {
        // don't append an empty buffer
        if (buf.size()) {
            pimpl_->append(*(buf.pimpl_.get()));
        }
    }

    /**
     * Return an iterator pointing to the first data chunk of this buffer
     * that may be written to.
     **/

    const_iterator begin() const {
        return const_iterator(pimpl_->beginWrite());
    }

    /**
     * Return the end iterator for writing.
     **/

    const_iterator end() const {
        return const_iterator(pimpl_->endWrite());
    }

    /**
     * Discard any data in this buffer.
     **/

    void discardData() {
        pimpl_->discardData();
    }

    /**
     * Discard the specified number of bytes from this data, starting at the beginning.
     * Throws if the size is greater than the number of bytes.
     **/

    void discardData(size_t bytes) {
        if (bytes > 0) {
            if (bytes < pimpl_->size()) {
                pimpl_->discardData(bytes);
            } else if (bytes == pimpl_->size()) {
                pimpl_->discardData();
            } else {
                throw std::out_of_range("trying to discard more data than exists");
            }
        }
    }

    /**
     * Remove bytes from this buffer, starting from the beginning, and place
     * them into a new buffer.  Throws if the number of requested bytes exceeds
     * the size of the buffer.  Data and freeSpace in the buffer after bytes
     * remains in this buffer.
     **/

    InputBuffer extractData(size_type bytes);

    /**
     * Remove all bytes from this buffer, returning them in a new buffer.
     * After removing data, some freeSpace may remain in this buffer.
     **/

    InputBuffer extractData();

    /**
     * Clone this buffer, creating a copy that contains the same data.
     **/

    OutputBuffer clone() const {
        detail::BufferImpl::SharedPtr newImpl(new detail::BufferImpl(*pimpl_));
        return OutputBuffer(newImpl);
    }

    /**
     * Add unmanaged data to the buffer.  The buffer will not automatically
     * free the data, but it will call the supplied function when the data is
     * no longer referenced by the buffer (or copies of the buffer).
     **/

    void appendForeignData(const data_type *data, size_type size, const detail::free_func &func) {
        pimpl_->appendForeignData(data, size, func);
    }

    /**
     * Returns the number of chunks that contain free space.
     **/

    size_t numChunks() const {
        return pimpl_->numFreeChunks();
    }

    /**
     * Returns the number of chunks that contain data
     **/

    size_t numDataChunks() const {
        return pimpl_->numDataChunks();
    }

private:
    friend class InputBuffer;
    friend class BufferReader;

    explicit OutputBuffer(detail::BufferImpl::SharedPtr pimpl) : pimpl_(std::move(pimpl)) {}

    detail::BufferImpl::SharedPtr pimpl_; ///< Must never be null.
};

/**
 * The InputBuffer (read-only buffer)
 *
 * InputBuffer is an immutable buffer which that may be constructed from an
 * OutputBuffer, or several of OutputBuffer's methods.  Once the data is
 * transfered to an InputBuffer it cannot be modified, only read (via
 * BufferReader, istream, or its iterator).
 *
 * Assignments and copies are shallow copies.
 *
 * -# ASIO access: - iterate using const_iterator for sending messages
 *
 **/

class AVRO_DECL InputBuffer {

public:
    typedef detail::size_type size_type;
    typedef detail::data_type data_type;

    // needed for asio
    typedef detail::InputBufferIterator const_iterator;

    /**
     * Default InputBuffer creates an empty buffer.
     *
     * Copy/assignment functions use the default ones.  They will do a shallow
     * copy, and because InputBuffer is immutable, the copies will be
     * identical.
     *
     * Destructor also uses the default, which resets a shared pointer,
     * deleting the underlying data if no other copies of exist.
     **/

    InputBuffer() : pimpl_(new detail::BufferImpl) {}

    /**
     * Construct an InputBuffer that contains the contents of an OutputBuffer.
     * The two buffers will have the same contents, but this copy will be
     * immutable, while the the OutputBuffer may still be written to.
     *
     * If you wish to move the data from the OutputBuffer to a new InputBuffer
     * (leaving only free space in the OutputBuffer),
     * OutputBuffer::extractData() will do this more efficiently.
     *
     * Implicit conversion is allowed.
     **/
    // NOLINTNEXTLINE(google-explicit-constructor)
    InputBuffer(const OutputBuffer &src) : pimpl_(new detail::BufferImpl(*src.pimpl_)) {}

    /**
     * Does the buffer have any data?
     **/

    bool empty() const {
        return (pimpl_->size() == 0);
    }

    /**
     * Returns the size of the buffer, in bytes.
     **/

    size_type size() const {
        return pimpl_->size();
    }

    /**
     * Return an iterator pointing to the first data chunk of this buffer
     * that contains data.
     **/

    const_iterator begin() const {
        return const_iterator(pimpl_->beginRead());
    }

    /**
     * Return the end iterator.
     **/

    const_iterator end() const {
        return const_iterator(pimpl_->endRead());
    }

    /**
     * Returns the number of chunks containing data.
     **/

    size_t numChunks() const {
        return pimpl_->numDataChunks();
    }

private:
    friend class OutputBuffer; // for append function
    friend class istreambuf;
    friend class BufferReader;

    explicit InputBuffer(const detail::BufferImpl::SharedPtr &pimpl) : pimpl_(pimpl) {}

    /**
     * Class to indicate that a copy of a OutputBuffer to InputBuffer should be
     * a shallow copy, used to enable reading of the contents of an
     * OutputBuffer without need to convert it to InputBuffer using a deep
     * copy.  It is private and only used by BufferReader and istreambuf
     * classes.
     *
     * Writing to an OutputBuffer while it is being read may lead to undefined
     * behavior.
     **/

    class ShallowCopy {};

    /**
     * Make a shallow copy of an OutputBuffer in order to read it without
     * causing conversion overhead.
     **/
    InputBuffer(const OutputBuffer &src, const ShallowCopy &) : pimpl_(src.pimpl_) {}

    /**
     * Make a shallow copy of an InputBuffer.  The default copy constructor
     * already provides shallow copy, this is just provided for generic
     * algorithms that wish to treat InputBuffer and OutputBuffer in the same
     * manner.
     **/

    InputBuffer(const InputBuffer &src, const ShallowCopy &) : pimpl_(src.pimpl_) {}

    detail::BufferImpl::ConstSharedPtr pimpl_; ///< Must never be null.
};

/*
 * Implementations of some OutputBuffer functions are inlined here
 * because InputBuffer definition was required before.
 */

inline InputBuffer OutputBuffer::extractData() {
    detail::BufferImpl::SharedPtr newImpl(new detail::BufferImpl);
    if (pimpl_->size()) {
        pimpl_->extractData(*newImpl);
    }
    return InputBuffer(newImpl);
}

inline InputBuffer OutputBuffer::extractData(size_type bytes) {
    if (bytes > pimpl_->size()) {
        throw std::out_of_range("trying to extract more data than exists");
    }

    detail::BufferImpl::SharedPtr newImpl(new detail::BufferImpl);
    if (bytes > 0) {
        if (bytes < pimpl_->size()) {
            pimpl_->extractData(*newImpl, bytes);
        } else {
            pimpl_->extractData(*newImpl);
        }
    }

    return InputBuffer(newImpl);
}

#ifndef _WIN32
/**
 * Create an array of iovec structures from the buffer.  This utility is used
 * to support writev and readv function calls.  The caller should ensure the
 * buffer object is not deleted while using the iovec vector.
 *
 * If the BufferType is an InputBuffer, the iovec will point to the data that
 * already exists in the buffer, for reading.
 *
 * If the BufferType is an OutputBuffer, the iovec will point to the free
 * space, which may be written to.  Before writing, the caller should call
 * OutputBuffer::reserve() to create enough room for the desired write (which
 * can be verified by calling OutputBuffer::freeSpace()), and after writing,
 * they MUST call OutputBuffer::wroteTo(), otherwise the buffer will not know
 * the space is not free anymore.
 *
 **/

template<class BufferType>
inline void toIovec(BufferType &buf, std::vector<struct iovec> &iov) {
    const size_t chunks = buf.numChunks();
    iov.resize(chunks);
    typename BufferType::const_iterator iter = buf.begin();
    for (size_t i = 0; i < chunks; ++i) {
        iov[i].iov_base = const_cast<typename BufferType::data_type *>(iter->data());
        iov[i].iov_len = iter->size();
        ++iter;
    }
}
#endif

} // namespace avro

#endif
