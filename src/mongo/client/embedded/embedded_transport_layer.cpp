/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/client/embedded/embedded_transport_layer.h"

#include <cstdlib>
#include <deque>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"
enum RPCState { WaitingForMessageLength, WaitingForMessageContent, HaveOutput };

struct FreeDeleter {
    void operator()(void* x) {
        free(x);
    }
};

struct mongoc_stream_embedded_t : mongoc_stream_t {
    libmongodbcapi_client* clientHandle;
    mongo::DataRangeCursor inputBuf = mongo::DataRangeCursor(nullptr, nullptr);
    std::unique_ptr<char, FreeDeleter> hiddenBuf;
    mongo::ConstDataRangeCursor outputBuf = mongo::ConstDataRangeCursor(nullptr, nullptr);

    void* libmongo_output;
    size_t libmongo_output_size;
    // If this is 0, we have recieved a full message and expect another header
    u_long input_length_to_go;
    RPCState state;
};

namespace {

struct FreeAndDestroy {
    void operator()(mongoc_stream_t* x) {
    auto stream = static_cast<mongoc_stream_embedded_t*>(x);
    libmongodbcapi_db_client_destroy(stream->clientHandle);
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
    invariant(stream->state == RPCState::WaitingForMessageContent ||
              stream->state == RPCState::WaitingForMessageLength);

    u_long already_read = 0;
    for (size_t i = 0; i < iovcnt; i++) {
        char* current_loc = static_cast<char*>(iov[i].iov_base);
        u_long remaining_iov = iov[i].iov_len;

        // @TODO for now just not handling vecs of this size
        invariant(remaining_iov >= 4);

        // do we need a new message?
        if (stream->state == RPCState::WaitingForMessageLength) {
            // message length is the first four bytes
            // Should use dataview from mongo server
            stream->input_length_to_go =
                mongo::ConstDataView(current_loc).read<mongo::LittleEndian<int32_t>>();
            // stream->hiddenBuf = (char*)malloc(stream->input_length_to_go);
            stream->hiddenBuf =
                std::unique_ptr<char, FreeDeleter>((char*)malloc(stream->input_length_to_go));
            stream->inputBuf = mongo::DataRangeCursor(
                stream->hiddenBuf.get(), stream->hiddenBuf.get() + stream->input_length_to_go);
            auto writeOK =
                stream->inputBuf.writeAndAdvance(mongo::DataRange(current_loc, current_loc + 4));
            invariant(writeOK.isOK());
            current_loc += 4;
            remaining_iov -= 4;
            stream->input_length_to_go -= 4;
            already_read += 4;
            stream->state = RPCState::WaitingForMessageContent;
        }

        // if there is no more message after reading length, we're done
        if (remaining_iov <= 0)
            continue;

        // copy message length into buffer
        // pipelining is not allowed, so remaining_iov must be less than input_length_to_go
        invariant(stream->input_length_to_go >= remaining_iov);
        auto writeOK = stream->inputBuf.writeAndAdvance(
            mongo::DataRange(current_loc, current_loc + remaining_iov));
        invariant(writeOK.isOK());
        // cleanup number values to reflect the copy
        stream->input_length_to_go -= remaining_iov;
        already_read += remaining_iov;
        remaining_iov = 0;

        // if we found a complete message, send it
        if (stream->input_length_to_go == 0) {
            auto input_len = (size_t)(stream->inputBuf.data() - stream->hiddenBuf.get());
            int retVal =
                libmongodbcapi_db_client_wire_protocol_rpc(stream->clientHandle,
                                                           stream->hiddenBuf.get(),
                                                           input_len,
                                                           &(stream->libmongo_output),
                                                           &(stream->libmongo_output_size));
            if (retVal != LIBMONGODB_CAPI_ERROR_SUCCESS) {
                return -1;
            }

            // We will allocate a new one when we read in the next message length
            stream->hiddenBuf.reset();
            // and then write the output to our output buffer
            auto start = static_cast<char*>(stream->libmongo_output);
            auto end = (static_cast<char*>(stream->libmongo_output)) + stream->libmongo_output_size;
            stream->outputBuf = mongo::ConstDataRangeCursor(start, end);
            stream->state = RPCState::HaveOutput;
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
    invariant(stream->state == RPCState::HaveOutput);
    for (size_t i = 0; i < iovcnt && stream->outputBuf.length() > 0; i++) {

        // for each vector, fill the vector if we are able
        size_t bytes_to_copy = std::min(iov[i].iov_len, stream->outputBuf.length());
        memcpy(iov[i].iov_base, stream->outputBuf.data(), bytes_to_copy);
        auto x = stream->outputBuf.advance(bytes_to_copy);
        invariant(x.isOK());
        bytes_read += bytes_to_copy;
    }
    stream->state =
        stream->outputBuf.length() == 0 ? RPCState::WaitingForMessageLength : RPCState::HaveOutput;
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
    stream->state = RPCState::WaitingForMessageLength;
    // Set up connections to database
    stream->clientHandle = libmongodbcapi_db_client_new((libmongodbcapi_db*)user_data);

    // Connect the functions to the stream
    // type is not relevant for us. Has to be set for the C Driver, but it has to do with picking
    // how to communicate over the networ
    stream->type = 1000;
    stream->poll = mongoc_stream_embedded_poll;
    stream->close = mongoc_stream_embedded_close;
    stream->readv = mongoc_stream_embedded_readv;
    stream->writev = mongoc_stream_embedded_writev;
    stream->destroy = mongoc_stream_embedded_destroy;

    return stream.release();
} catch (...) {
    errno = EBADMSG;
    return nullptr;
}

}  // namespace

struct ClientDeleter {
    void operator()(mongoc_client_t* x) {
        mongoc_client_destroy(x);
    }
};

extern "C" mongoc_client_t* embedded_mongoc_client_new(libmongodbcapi_db* db) try {
    if (!db) {
        errno = EINVAL;
        return nullptr;
    }
    std::unique_ptr<mongoc_client_t, ClientDeleter> client(mongoc_client_new(NULL));
    mongoc_client_set_stream_initiator(client.get(), embedded_stream_initiator, db);
    return client.release();
} catch (const std::out_of_range& c) {
    errno = EACCES;
    return nullptr;
} catch (const std::overflow_error& c) {
    errno = EOVERFLOW;
    return nullptr;
} catch (const std::underflow_error& c) {
    errno = ERANGE;
    return nullptr;
} catch (const std::invalid_argument& c) {
    errno = EINVAL;
    return nullptr;
} catch (...) {
    errno = EBADMSG;
    return nullptr;
}
