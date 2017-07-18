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
#include <string>

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"
enum RPCState { WaitingForMessageLength, WaitingForMessageContent, HaveOutput };
struct mongoc_stream_embedded_t : mongoc_stream_t {
    libmongodbcapi_client* clientHandle;
    mongo::DataRangeCursor inputBuf = mongo::DataRangeCursor(nullptr, nullptr);
    char* hiddenBuf = nullptr;
    mongo::ConstDataRangeCursor outputBuf = mongo::ConstDataRangeCursor(nullptr, nullptr);

    void* libmongo_output;
    size_t libmongo_output_size;
    // If this is 0, we have recieved a full message and expect another header
    u_long input_length_to_go;
    RPCState state;
};

ssize_t _mongoc_stream_embedded_writev(mongoc_stream_t* s,
                                       mongoc_iovec_t* iov,
                                       size_t iovcnt,
                                       int32_t timeout_msec) {
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
            stream->hiddenBuf = (char*)malloc(stream->input_length_to_go);
            stream->inputBuf = mongo::DataRangeCursor(
                stream->hiddenBuf, stream->hiddenBuf + stream->input_length_to_go);
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
            auto input_len = (size_t)(stream->inputBuf.data() - stream->hiddenBuf);
            int retVal =
                libmongodbcapi_db_client_wire_protocol_rpc(stream->clientHandle,
                                                           stream->hiddenBuf,
                                                           input_len,
                                                           &(stream->libmongo_output),
                                                           &(stream->libmongo_output_size));
            if (retVal != LIBMONGODB_CAPI_ERROR_SUCCESS) {
                return -1;
            }

            // We will allocate a new one when we read in the next message length
            free(stream->hiddenBuf);

            // and then write the output to our output buffer
            auto start = (char*)(stream->libmongo_output);
            auto end = ((char*)(stream->libmongo_output)) + stream->libmongo_output_size;
            stream->outputBuf = mongo::ConstDataRangeCursor(start, end);
            stream->state = RPCState::HaveOutput;
        }
    }

    return already_read;
}

ssize_t _mongoc_stream_embedded_readv(mongoc_stream_t* s,
                                      mongoc_iovec_t* iov,
                                      size_t iovcnt,
                                      size_t min_bytes,
                                      int32_t timeout_msec) {
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
}

void _mongoc_stream_embedded_destroy(mongoc_stream_t* s) {

    auto stream = static_cast<mongoc_stream_embedded_t*>(s);
    libmongodbcapi_db_client_destroy(stream->clientHandle);
    stream->~mongoc_stream_embedded_t();
}

int _mongoc_stream_embedded_close(mongoc_stream_t* s) {
    return 0;
}

ssize_t _mongoc_stream_embedded_poll(mongoc_stream_poll_t* s,
                                     size_t array_length,
                                     int32_t timeout_msec) {
    for (size_t i = 0; i < array_length; i++) {
        s[i].revents = s[i].events & (POLLIN | POLLOUT);
    }
    return array_length;
}

mongoc_stream_t* embedded_stream_initiator(const mongoc_uri_t* uri,
                                           const mongoc_host_list_t* host,
                                           void* user_data,
                                           bson_error_t* error) {
    mongoc_stream_embedded_t* stream =
        static_cast<mongoc_stream_embedded_t*>(bson_malloc0(sizeof(*stream)));
    if (!stream)
        return NULL;

    // Create the stream
    stream = new (stream) mongoc_stream_embedded_t();
    stream->state = RPCState::WaitingForMessageLength;
    // Set up connections to database
    stream->clientHandle = libmongodbcapi_db_client_new((libmongodbcapi_db*)user_data);

    // Connect the functions to the stream
    // type is not relevant for us. Has to be set for the C Driver, but it has to do with picking
    // how to communicate over the networ
    stream->type = 1000;
    stream->poll = _mongoc_stream_embedded_poll;
    stream->close = _mongoc_stream_embedded_close;
    stream->readv = _mongoc_stream_embedded_readv;
    stream->writev = _mongoc_stream_embedded_writev;
    stream->destroy = _mongoc_stream_embedded_destroy;

    return stream;
}
mongoc_client_t* embedded_mongoc_client_new(libmongodbcapi_db* db) {
    if (!db) {
        return nullptr;
    }
    auto client = mongoc_client_new(NULL);
    mongoc_client_set_stream_initiator(client, embedded_stream_initiator, db);
    return client;
}
