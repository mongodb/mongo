/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongoc_embedded/mongoc_embedded.h"

#include <algorithm>
#include <cassert>
#include <cinttypes>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#include "mongo_embedded/mongo_embedded.h"

// Only header-only includes allowed here (except for capi.h)
#include "mongo/platform/endian.h"

#if defined(_WIN32)
#define MONGO_API_CALL __cdecl
#else
#define MONGO_API_CALL
#endif

// Macro to trick the linter into accepting assert.
#define mongoc_client_assert assert

namespace {
enum RPCState { kWaitingForMessageLength, kWaitingForMessageContent, kHaveOutput };

// A non-owning memory view with that encapulates reading or writing from that memory by keeping
// track of a current pointer that advances on the read or write.
struct MemoryView {
    MemoryView() = default;
    explicit MemoryView(char* data, size_t size)
        : _begin(data), _current(data), _end(data + size) {}

    char* begin() {
        return _begin;
    }

    char* current() {
        return _current;
    }

    char* end() {
        return _end;
    }

    // Write memory to current position and advance internal pointer
    void write(const void* source, size_t size) {
        if (remaining() < size) {
            mongoc_client_assert(false);
            return;
        }

        memcpy(_current, source, size);
        _current += size;
    }

    // Read memory from current position and advance internal pointer
    size_t read(void* destination, size_t size) {
        size_t bytes_to_read = std::min(remaining(), size);
        memcpy(destination, current(), bytes_to_read);
        _current += bytes_to_read;
        return bytes_to_read;
    }

    // Size that have currently been read or written
    size_t size() const {
        return _current - _begin;
    }

    // Total capacity for the memory this view is holding
    size_t capacity() const {
        return _end - _begin;
    }

    // Remaining memory available for read or write
    size_t remaining() const {
        return _end - _current;
    }

    char* _begin{nullptr};
    char* _current{nullptr};
    char* _end{nullptr};
};

struct FreeDeleter {
    void operator()(void* x) {
        free(x);
    }
};
}  // namespace

struct mongoc_stream_embedded_t : mongoc_stream_t {
    mongo_embedded_v1_client* clientHandle;
    MemoryView inputBuf;
    std::unique_ptr<char, FreeDeleter> hiddenBuf;
    MemoryView outputBuf;
    RPCState state;
};

namespace {

struct FreeAndDestroy {
    void operator()(mongoc_stream_t* x) {
        auto stream = static_cast<mongoc_stream_embedded_t*>(x);
        mongo_embedded_v1_client_destroy(stream->clientHandle, nullptr);
        stream->~mongoc_stream_embedded_t();
        free(stream);
    }
};
extern "C" void mongoc_stream_embedded_destroy(mongoc_stream_t* s) try {
    std::unique_ptr<mongoc_stream_t, FreeAndDestroy> stream(s);
} catch (...) {
    errno = EBADMSG;
}


extern "C" ssize_t mongoc_stream_embedded_writev(mongoc_stream_t* s,
                                                 mongoc_iovec_t* iov,
                                                 size_t iovcnt,
                                                 int32_t timeout_msec) try {
    auto stream = static_cast<mongoc_stream_embedded_t*>(s);
    mongoc_client_assert(stream->state == RPCState::kWaitingForMessageContent ||
                         stream->state == RPCState::kWaitingForMessageLength);

    u_long already_read = 0;
    for (size_t i = 0; i < iovcnt; i++) {
        char* current_loc = static_cast<char*>(iov[i].iov_base);
        u_long remaining_iov = iov[i].iov_len;

        // do we need a new message?
        if (stream->state == RPCState::kWaitingForMessageLength) {
            // The message should start with a 4 byte size
            int32_t message_length;
            if (remaining_iov < sizeof(message_length)) {
                errno = EBADMSG;
                return 0;
            }

            // memcpy into message_length, to be super safe in case the buffer is not 32bit aligned.
            memcpy(&message_length, current_loc, sizeof(message_length));

            // make sure we convert from network byte order to host byte order before using it.
            message_length = mongo::endian::littleToNative(message_length);

            stream->hiddenBuf = std::unique_ptr<char, FreeDeleter>((char*)malloc(message_length));
            stream->inputBuf = MemoryView(stream->hiddenBuf.get(), message_length);
            stream->inputBuf.write(current_loc, sizeof(message_length));

            current_loc += sizeof(message_length);
            remaining_iov -= sizeof(message_length);
            already_read += sizeof(message_length);
            stream->state = RPCState::kWaitingForMessageContent;
        }

        // if there is no more message after reading length, we're done
        if (remaining_iov <= 0)
            continue;

        // copy message length into buffer
        // pipelining is not allowed, so remaining_iov must be less than input_length_to_go
        mongoc_client_assert(stream->inputBuf.remaining() >= remaining_iov);
        stream->inputBuf.write(current_loc, remaining_iov);

        // cleanup number values to reflect the copy
        already_read += remaining_iov;
        remaining_iov = 0;

        // if we found a complete message, send it
        if (stream->inputBuf.remaining() == 0) {
            void* output_buffer;
            size_t output_buffer_size;
            int retVal = mongo_embedded_v1_client_invoke(stream->clientHandle,
                                                         stream->inputBuf.begin(),
                                                         stream->inputBuf.size(),
                                                         &output_buffer,
                                                         &output_buffer_size,
                                                         nullptr);
            if (retVal != MONGO_EMBEDDED_V1_SUCCESS) {
                return -1;
            }

            // We will allocate a new one when we read in the next message length
            stream->hiddenBuf.reset();
            // and then write the output to our output buffer
            stream->outputBuf = MemoryView(static_cast<char*>(output_buffer), output_buffer_size);
            stream->state = RPCState::kHaveOutput;
        }
    }

    return already_read;
} catch (...) {
    errno = EBADMSG;
    return 0;  // not guarenteeing anything was written
}
extern "C" ssize_t mongoc_stream_embedded_readv(mongoc_stream_t* s,
                                                mongoc_iovec_t* iov,
                                                size_t iovcnt,
                                                size_t min_bytes,
                                                int32_t timeout_msec) try {
    size_t bytes_read = 0;
    auto stream = static_cast<mongoc_stream_embedded_t*>(s);
    mongoc_client_assert(stream->state == RPCState::kHaveOutput);
    for (size_t i = 0; i < iovcnt && stream->outputBuf.remaining() > 0; ++i) {

        // for each vector, fill the vector if we are able
        bytes_read += stream->outputBuf.read(iov[i].iov_base, iov[i].iov_len);
    }
    stream->state = stream->outputBuf.remaining() == 0 ? RPCState::kWaitingForMessageLength
                                                       : RPCState::kHaveOutput;
    return bytes_read;
} catch (...) {
    errno = EBADMSG;
    return 0;  // not guarenteeing anything was read
}


extern "C" int mongoc_stream_embedded_close(mongoc_stream_t* s) {
    return 0;
}

extern "C" ssize_t mongoc_stream_embedded_poll(mongoc_stream_poll_t* s,
                                               size_t array_length,
                                               int32_t timeout_msec) try {
    for (size_t i = 0; i < array_length; i++) {
        s[i].revents = s[i].events & (POLLIN | POLLOUT);
    }
    return array_length;
} catch (...) {
    errno = EBADMSG;
    return -1;
}

extern "C" bool mongoc_stream_embedded_check_closed(mongoc_stream_t* s) noexcept {
    return false;
}

extern "C" mongoc_stream_t* embedded_stream_initiator(const mongoc_uri_t* uri,
                                                      const mongoc_host_list_t* host,
                                                      void* user_data,
                                                      bson_error_t* error) try {
    std::unique_ptr<unsigned char, FreeDeleter> stream_buf(
        static_cast<unsigned char*>(bson_malloc0(sizeof(mongoc_stream_embedded_t))));
    if (!stream_buf) {
        errno = ENOMEM;
        return nullptr;
    }
    // Create the stream
    std::unique_ptr<mongoc_stream_embedded_t, FreeAndDestroy> stream(
        new (stream_buf.get()) mongoc_stream_embedded_t());
    stream_buf.release();  // This must be here so we don't have double ownership
    stream->state = RPCState::kWaitingForMessageLength;
    // Set up connections to database
    stream->clientHandle = mongo_embedded_v1_client_create(
        static_cast<mongo_embedded_v1_instance*>(user_data), nullptr);

    // Connect the functions to the stream
    // type is not relevant for us. Has to be set for the C Driver, but it has to do with picking
    // how to communicate over the networ
    stream->type = 1000;
    stream->poll = mongoc_stream_embedded_poll;
    stream->close = mongoc_stream_embedded_close;
    stream->readv = mongoc_stream_embedded_readv;
    stream->writev = mongoc_stream_embedded_writev;
    stream->destroy = mongoc_stream_embedded_destroy;
    stream->check_closed = mongoc_stream_embedded_check_closed;
    return stream.release();
} catch (...) {
    errno = EBADMSG;
    return nullptr;
}

struct ClientDeleter {
    void operator()(mongoc_client_t* x) {
        mongoc_client_destroy(x);
    }
};

}  // namespace

extern "C" mongoc_client_t* MONGO_API_CALL
mongoc_embedded_v1_client_create(mongo_embedded_v1_instance* db) try {
    if (!db) {
        errno = EINVAL;
        return nullptr;
    }
    std::unique_ptr<mongoc_client_t, ClientDeleter> client(mongoc_client_new(NULL));
    mongoc_client_set_stream_initiator(client.get(), embedded_stream_initiator, db);
    return client.release();
} catch (const std::out_of_range&) {
    errno = EACCES;
    return nullptr;
} catch (const std::overflow_error&) {
    errno = EOVERFLOW;
    return nullptr;
} catch (const std::underflow_error&) {
    errno = ERANGE;
    return nullptr;
} catch (const std::invalid_argument&) {
    errno = EINVAL;
    return nullptr;
} catch (...) {
    errno = EBADMSG;
    return nullptr;
}
