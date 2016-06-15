/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <asio.hpp>
#include <system_error>
#include <utility>

#include "mongo/util/assert_util.h"

namespace mongo {
namespace executor {

void logCloseFailed(std::error_code ec);

template <typename ASIOStream>
void destroyStream(ASIOStream* stream, bool connected) {
    if (!connected) {
        return;
    }

    std::error_code ec;

    stream->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    if (ec) {
        logCloseFailed(ec);
    }

    stream->close(ec);
    if (ec) {
        logCloseFailed(ec);
    }
}

template <typename ASIOStream, typename Buffer, typename Handler>
void writeStream(ASIOStream* stream,
                 asio::io_service::strand* strand,
                 bool connected,
                 Buffer&& buffer,
                 Handler&& handler) {
    invariant(connected);
    asio::async_write(*stream,
                      asio::buffer(std::forward<Buffer>(buffer)),
                      strand->wrap(std::forward<Handler>(handler)));
}

template <typename ASIOStream, typename Buffer, typename Handler>
void readStream(ASIOStream* stream,
                asio::io_service::strand* strand,
                bool connected,
                Buffer&& buffer,
                Handler&& handler) {
    invariant(connected);
    asio::async_read(*stream,
                     asio::buffer(std::forward<Buffer>(buffer)),
                     strand->wrap(std::forward<Handler>(handler)));
}

template <typename ASIOStream>
void cancelStream(ASIOStream* stream, bool connected) {
    stream->cancel();
}

void logFailureInSetStreamNonBlocking(std::error_code ec);
void logFailureInSetStreamNoDelay(std::error_code ec);

template <typename ASIOStream>
std::error_code setStreamNonBlocking(ASIOStream* stream) {
    std::error_code ec;
    stream->non_blocking(true, ec);
    if (ec) {
        logFailureInSetStreamNonBlocking(ec);
    }
    return ec;
}

template <typename ASIOStream>
std::error_code setStreamNoDelay(ASIOStream* stream) {
    std::error_code ec;
    stream->set_option(asio::ip::tcp::no_delay(true), ec);
    if (ec) {
        logFailureInSetStreamNoDelay(ec);
    }
    return ec;
}

void logUnexpectedErrorInCheckOpen(std::error_code ec);

template <typename ASIOStream>
bool checkIfStreamIsOpen(ASIOStream* stream, bool connected) {
    if (!connected) {
        return false;
    };
    std::error_code ec;
    std::array<char, 1> buf;
    // Although we call the blocking form of receive, we ensure the socket is in non-blocking mode.
    // ASIO implements receive on POSIX using the 'recvmsg' system call, which returns immediately
    // if the socket is non-blocking and in a valid state, but there is no data to receive. On
    // windows, receive is implemented with WSARecv, which has the same semantics.
    invariant(stream->non_blocking());
    stream->receive(asio::buffer(buf), asio::socket_base::message_peek, ec);
    if (!ec || ec == asio::error::would_block || ec == asio::error::try_again) {
        // If the read worked or we got EWOULDBLOCK or EAGAIN (since we are in non-blocking mode),
        // we assume the socket is still open.
        return true;
    } else if (ec == asio::error::eof) {
        return false;
    }
    // We got a different error. Log it and return false so we throw the connection away.
    logUnexpectedErrorInCheckOpen(ec);
    return false;
}

}  // namespace executor
}  // namespace mongo
