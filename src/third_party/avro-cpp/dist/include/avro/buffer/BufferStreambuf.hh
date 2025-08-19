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

#ifndef avro_BufferStreambuf_hh__
#define avro_BufferStreambuf_hh__

#include <utility>

#include "Buffer.hh"

/** \file BufferStreambuf.hh
    \brief streambuf implementation for istream and ostream.
*/

#ifdef min
#undef min
#endif
namespace avro {

/**
 * \brief Implementation of streambuf for use by the Buffer's ostream.
 *
 * This class derives from std::streambuf and implements the virtual functions
 * needed to operate on OutputBuffer.  The override functions are overflow and
 * xsputn.   Typically custom streambufs will also override sync for output,
 * but we have no need since all writes are immediately stored in the buffer.
 **/

class AVRO_DECL ostreambuf : public std::streambuf {

public:
    /// Default constructor creates a new OutputBuffer.
    ostreambuf() : std::streambuf(),
                   buffer_() {}

    /// Construct using an existing OutputBuffer.
    explicit ostreambuf(OutputBuffer &buffer) : std::streambuf(),
                                                buffer_(buffer) {}

    /// Return the buffer.
    const OutputBuffer &getBuffer() const {
        return buffer_;
    }

protected:
    /// Write a single character to the stream.
    int_type overflow(int_type c) override {
        buffer_.writeTo(static_cast<OutputBuffer::data_type>(c));
        return c;
    }

    /// Write a block of characters to the stream.
    std::streamsize xsputn(const char_type *s, std::streamsize n) override {
        return buffer_.writeTo(s, static_cast<size_t>(n));
    }

private:
    OutputBuffer buffer_;
};

/**
 * \brief Implementation of streambuf for use by the Buffer's istream.
 *
 * This class derives from std::streambuf and implements the virtual functions
 * needed to operate on InputBuffer.  The override functions are underflow,
 * seekpos, showmanyc, and seek.  This is considered a buffered streambuf,
 * because it can access a chunk of the InputBuffer at a time, using the
 * iterator interface.  Because the input is already buffered, uflow is not
 * required.  pbackfail is not yet implemented but can be if necessary (the
 * inherited behavior is to fail, and has yet to be a problem).
 *
 **/

class AVRO_DECL istreambuf : public std::streambuf {

public:
    /// Default constructor requires an InputBuffer to read from.
    explicit istreambuf(InputBuffer buffer) : std::streambuf(),
                                              buffer_(std::move(buffer)),
                                              basePos_(0),
                                              iter_(buffer_.begin()) {
        setBuffer();
    }

    /// Default constructor converts an OutputBuffer to an InputBuffer
    explicit istreambuf(const OutputBuffer &buffer) : std::streambuf(),
                                                      buffer_(buffer, InputBuffer::ShallowCopy()),
                                                      basePos_(0),
                                                      iter_(buffer_.begin()) {
        setBuffer();
    }

    /// Return the buffer.
    const InputBuffer &getBuffer() const {
        return buffer_;
    }

protected:
    /// The current chunk of data is exhausted, read the next chunk.
    int_type underflow() override {
        if (iter_ != buffer_.end()) {
            basePos_ += (egptr() - eback());
            ++iter_;
        }
        return setBuffer();
    }

    /// Get a block of data from the stream.  Overrides default behavior
    /// to ignore eof characters that may reside in the stream.
    std::streamsize xsgetn(char_type *c, std::streamsize len) override {
        std::streamsize bytesCopied = 0;

        while (bytesCopied < len) {

            size_t inBuffer = egptr() - gptr();

            if (inBuffer) {
                auto remaining = static_cast<size_t>(len - bytesCopied);
                size_t toCopy = std::min(inBuffer, remaining);
                memcpy(c, gptr(), toCopy);
                c += toCopy;
                bytesCopied += toCopy;
                while (toCopy > static_cast<size_t>(std::numeric_limits<int>::max())) {
                    gbump(std::numeric_limits<int>::max());
                    toCopy -= static_cast<size_t>(std::numeric_limits<int>::max());
                }
                gbump(static_cast<int>(toCopy));
            }

            if (bytesCopied < len) {
                underflow();
                if (iter_ == buffer_.end()) {
                    break;
                }
            }
        }

        return bytesCopied;
    }

    /// Special seek override to navigate InputBuffer chunks.
    pos_type seekoff(off_type off, std::ios::seekdir dir, std::ios_base::openmode) override {

        off_type curpos = basePos_ + (gptr() - eback());
        off_type newpos = off;

        if (dir == std::ios::cur) {
            newpos += curpos;
        } else if (dir == std::ios::end) {
            newpos += buffer_.size();
        }
        // short circuit for tell()
        if (newpos == curpos) {
            return curpos;
        }

        off_type endpos = basePos_ + (egptr() - eback());

        // if the position is after our current buffer make
        // sure it's not past the end of the buffer
        if ((newpos > endpos) && (newpos > static_cast<off_type>(buffer_.size()))) {
            return {-1};
        }
        // if the new position is before our current iterator
        // reset the iterator to the beginning
        else if (newpos < basePos_) {
            iter_ = buffer_.begin();
            basePos_ = 0;
            setBuffer();
            endpos = (egptr() - eback());
        }

        // now if the new position is after the end of the buffer
        // increase the buffer until it is not
        while (newpos > endpos) {
            istreambuf::underflow();
            endpos = basePos_ + (egptr() - eback());
        }

        setg(eback(), eback() + (newpos - basePos_), egptr());
        return newpos;
    }

    /// Calls seekoff for implemention.
    pos_type seekpos(pos_type pos, std::ios_base::openmode) override {
        return istreambuf::seekoff(pos, std::ios::beg, std::ios_base::openmode(0));
    }

    /// Shows the number of bytes buffered in the current chunk, or next chunk if
    /// current is exhausted.
    std::streamsize showmanyc() override {

        // this function only gets called when the current buffer has been
        // completely read, verify this is the case, and if so, underflow to
        // fetch the next buffer

        if (egptr() - gptr() == 0) {
            istreambuf::underflow();
        }
        return egptr() - gptr();
    }

private:
    /// Setup the streambuf buffer pointers after updating
    /// the value of the iterator.  Returns the first character
    /// in the new buffer, or eof if there is no buffer.
    int_type setBuffer() {
        int_type ret = traits_type::eof();

        if (iter_ != buffer_.end()) {
            char *loc = const_cast<char *>(iter_->data());
            setg(loc, loc, loc + iter_->size());
            ret = std::char_traits<char>::to_int_type(*gptr());
        } else {
            setg(nullptr, nullptr, nullptr);
        }
        return ret;
    }

    const InputBuffer buffer_;
    off_type basePos_;
    InputBuffer::const_iterator iter_;
};

} // namespace avro

#endif
