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

#ifndef avro_BufferStream_hh__
#define avro_BufferStream_hh__

#include "BufferStreambuf.hh"

/**
 * \file BufferStream.hh
 *
 * \brief Custom istream and ostream classes for use with buffers
 **/

namespace avro {

/**
 *
 * \brief Custom ostream class for writing to an OutputBuffer
 *
 **/

class AVRO_DECL ostream : public std::ostream {

public:
    /// Default constructor, creates a new OutputBuffer.
    ostream() : std::ostream(&obuf_) {}

    /// Output to a specific buffer.
    explicit ostream(OutputBuffer &buf) : std::ostream(&obuf_),
                                          obuf_(buf) {}

    /// Return the output buffer created by the write operations to this ostream.
    const OutputBuffer &getBuffer() const {
        return obuf_.getBuffer();
    }

protected:
    ostreambuf obuf_;
};

/**
 * \brief Custom istream class for reading from an InputBuffer.
 *
 * If the buffer contains binary data, then it is recommended to only use the
 * read() and readsome() functions--get() or getline() may be confused if the
 * binary data happens to contain an EOF character.
 *
 * For buffers containing text, the full implementation of istream is safe.
 *
 **/

class AVRO_DECL istream : public std::istream {

public:
    /// Constructor, requires an InputBuffer to read from.
    explicit istream(const InputBuffer &buf) : std::istream(&ibuf_), ibuf_(buf) {}

    /// Constructor, takes an OutputBuffer to read from (by making a shallow copy to an InputBuffer).
    /// Writing to the OutputBuffer while an istream is using it may lead to undefined behavior.
    explicit istream(const OutputBuffer &buf) : std::istream(&ibuf_), ibuf_(buf) {}

    /// Return the InputBuffer this stream is reading from.
    const InputBuffer &getBuffer() const {
        return ibuf_.getBuffer();
    }

protected:
    istreambuf ibuf_;
};

} // namespace avro

#endif
