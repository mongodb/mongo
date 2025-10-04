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

#ifndef avro_BufferDetailIterator_hh__
#define avro_BufferDetailIterator_hh__

#include "BufferDetail.hh"

/**
 * \file BufferDetailIterator.hh
 *
 * \brief The implementation details for the Buffer iterators.
 **/

namespace avro {

namespace detail {

/**
 * \brief Implements conversion from a chunk to asio::const_buffer
 *
 * Iterators for an InputBuffer will iterate over the avro of chunks, so
 * internally they contain an iterator.  But the iterator needs to be
 * convertable to an asio buffer for use in boost::asio functions.  This class
 * wraps the iterator with a cast operator to do this conversion.
 **/

struct InputIteratorHelper {
    /// Construct a helper with an unnassigned iterator.
    InputIteratorHelper() : iter_() {}

    /// Construct a helper with an iterator.
    explicit InputIteratorHelper(const BufferImpl::ChunkList::const_iterator &iter) : iter_(iter) {}

    /// The location of valid data in this chunk.
    const data_type *data() const {
        return iter_->tellReadPos();
    }

    /// The size of valid data in this chunk.
    size_type size() const {
        return iter_->dataSize();
    }

    /// Conversion operator.   It doesn't check for null, because the only
    /// the only time the chunk should be null is when it's the iterator
    /// end(), which should never be dereferenced anyway.
#ifdef HAVE_BOOST_ASIO
    operator ConstAsioBuffer() const {
        return ConstAsioBuffer(data(), size());
    }
#endif

    BufferImpl::ChunkList::const_iterator iter_; ///< the current iterator
};

/**
 * \brief Implements conversion from a chunk to asio::buffer
 *
 * Iterators for an OutputBuffer will iterate over the avro of chunks, so
 * internally they contain an iterator.  But the iterator needs to be
 * convertable to an asio buffer for use in boost::asio functions.  This class
 * wraps the iterator with a cast operator to do this conversion.
 */

struct OutputIteratorHelper {
    /// Construct a helper with an unnassigned iterator.
    OutputIteratorHelper() : iter_() {}

    /// Construct a helper with an iterator.
    explicit OutputIteratorHelper(const BufferImpl::ChunkList::const_iterator &iter) : iter_(iter) {}

    /// The location of the first writable byte in this chunk.
    data_type *data() const {
        return iter_->tellWritePos();
    }

    /// The size of area that can be written in this chunk.
    size_type size() const {
        return iter_->freeSize();
    }

    /// Conversion operator.   It doesn't check for null, because the only
    /// the only time the chunk should be null is when it's the iterator
    /// end(), which should never be dereferenced anyway.
#ifdef HAVE_BOOST_ASIO
    operator MutableAsioBuffer() const {
        return MutableAsioBuffer(data(), size());
    }
#endif

    BufferImpl::ChunkList::const_iterator iter_; ///< the current iterator
};

/**
 * \brief Implements the iterator for Buffer, that iterates through the
 * buffer's chunks.
 **/

template<typename Helper>
class BufferIterator {

public:
    typedef BufferIterator<Helper> this_type;

    /**
     * @name Typedefs
     *
     * STL iterators define the following declarations.  According to
     * boost::asio documentation, the library expects the iterator to be
     * bidirectional, however this implements only the forward iterator type.
     * So far this has not created any problems with asio, but may change if
     * future versions of the asio require it.
     **/

    //@{
    typedef std::forward_iterator_tag iterator_category; // this is a lie to appease asio
    typedef Helper value_type;
    typedef std::ptrdiff_t difference_type;
    typedef value_type *pointer;
    typedef value_type &reference;
    //@}

    /// Construct an unitialized iterator.
    BufferIterator() : helper_() {}

    /* The default implementations are good here
    /// Copy constructor.
    BufferIterator(const BufferIterator &src) :
        helper_(src.helper_)
    { }
    /// Assignment.
    this_type& operator= (const this_type &rhs) {
        helper_ = rhs.helper_;
        return *this;
    }
    */

    /// Construct iterator at the position in the buffer's chunk list.
    explicit BufferIterator(BufferImpl::ChunkList::const_iterator iter) : helper_(iter) {}

    /// Dereference iterator, returns InputIteratorHelper or OutputIteratorHelper wrapper.
    reference operator*() {
        return helper_;
    }

    /// Dereference iterator, returns const InputIteratorHelper or OutputIteratorHelper wrapper.
    const value_type &operator*() const {
        return helper_;
    }

    /// Dereference iterator, returns InputIteratorHelper or OutputIteratorHelper wrapper.
    pointer operator->() {
        return &helper_;
    }

    /// Dereference iterator, returns const InputIteratorHelper or OutputIteratorHelper wrapper.
    const value_type *operator->() const {
        return &helper_;
    }

    /// Increment to next chunk in list, or to end() iterator.
    this_type &operator++() {
        ++helper_.iter_;
        return *this;
    }

    /// Increment to next chunk in list, or to end() iterator.
    this_type operator++(int) {
        this_type ret = *this;
        ++helper_.iter_;
        return ret;
    }

    /// True if iterators point to same chunks.
    bool operator==(const this_type &rhs) const {
        return (helper_.iter_ == rhs.helper_.iter_);
    }

    /// True if iterators point to different chunks.
    bool operator!=(const this_type &rhs) const {
        return (helper_.iter_ != rhs.helper_.iter_);
    }

private:
    Helper helper_;
};

typedef BufferIterator<InputIteratorHelper> InputBufferIterator;
typedef BufferIterator<OutputIteratorHelper> OutputBufferIterator;

} // namespace detail

} // namespace avro

#endif
