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
    stream->close();
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
    invariant(connected);
    stream->cancel();
}

}  // namespace executor
}  // namespace mongo
