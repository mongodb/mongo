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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/base/system_error.h"
#include "mongo/db/stats/counters.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/ticket_asio.h"
#include "mongo/util/log.h"

#include "mongo/transport/session_asio.h"

namespace mongo {
namespace transport {
namespace {
constexpr auto kHeaderSize = sizeof(MSGHEADER::Value);

}  // namespace


std::shared_ptr<TransportLayerASIO::ASIOSession> TransportLayerASIO::ASIOTicket::getSession() {
    auto session = _session.lock();
    if (!session || !session->isOpen()) {
        finishFill(Ticket::SessionClosedStatus);
        return nullptr;
    }
    return session;
}

bool TransportLayerASIO::ASIOTicket::isSync() const {
    return _fillSync;
}

TransportLayerASIO::ASIOTicket::ASIOTicket(const ASIOSessionHandle& session, Date_t expiration)
    : _session(session), _sessionId(session->id()), _expiration(expiration) {}

TransportLayerASIO::ASIOSourceTicket::ASIOSourceTicket(const ASIOSessionHandle& session,
                                                       Date_t expiration,
                                                       Message* msg)
    : ASIOTicket(session, expiration), _target(msg) {}

TransportLayerASIO::ASIOSinkTicket::ASIOSinkTicket(const ASIOSessionHandle& session,
                                                   Date_t expiration,
                                                   const Message& msg)
    : ASIOTicket(session, expiration), _msgToSend(msg) {}

void TransportLayerASIO::ASIOSourceTicket::_bodyCallback(const std::error_code& ec, size_t size) {
    if (ec) {
        finishFill(errorCodeToStatus(ec));
        return;
    }

    _target->setData(std::move(_buffer));
    networkCounter.hitPhysicalIn(_target->size());
    finishFill(Status::OK());
}

void TransportLayerASIO::ASIOSourceTicket::_headerCallback(const std::error_code& ec, size_t size) {
    if (ec) {
        finishFill(errorCodeToStatus(ec));
        return;
    }

    auto session = getSession();
    if (!session)
        return;

    MSGHEADER::View headerView(_buffer.get());
    auto msgLen = static_cast<size_t>(headerView.getMessageLength());
    if (msgLen < kHeaderSize || msgLen > MaxMessageSizeBytes) {
        LOG(0) << "recv(): message msgLen " << msgLen << " is invalid. "
               << "Min " << kHeaderSize << " Max: " << MaxMessageSizeBytes;
        finishFill(errorCodeToStatus(ec));
        return;
    }

    if (msgLen == size) {
        finishFill(Status::OK());
        return;
    }

    _buffer.realloc(msgLen);
    MsgData::View msgView(_buffer.get());

    session->read(isSync(),
                  asio::buffer(msgView.data(), msgView.dataLen()),
                  [this](const std::error_code& ec, size_t size) { _bodyCallback(ec, size); });
}

void TransportLayerASIO::ASIOSourceTicket::fillImpl() {
    auto session = getSession();
    if (!session)
        return;

    const auto initBufSize = kHeaderSize;
    _buffer = SharedBuffer::allocate(initBufSize);

    session->read(isSync(),
                  asio::buffer(_buffer.get(), initBufSize),
                  [this](const std::error_code& ec, size_t size) { _headerCallback(ec, size); });
}

void TransportLayerASIO::ASIOSinkTicket::_sinkCallback(const std::error_code& ec, size_t size) {
    networkCounter.hitPhysicalOut(_msgToSend.size());
    finishFill(ec ? errorCodeToStatus(ec) : Status::OK());
}

void TransportLayerASIO::ASIOSinkTicket::fillImpl() {
    auto session = getSession();
    if (!session)
        return;

    session->write(isSync(),
                   asio::buffer(_msgToSend.buf(), _msgToSend.size()),
                   [this](const std::error_code& ec, size_t size) { _sinkCallback(ec, size); });
}

void TransportLayerASIO::ASIOTicket::finishFill(Status status) {
    // We want to make sure that a Ticket can only be filled once; filling a ticket invalidates it.
    // So we check that the _fillCallback is set, then move it out of the ticket and into a local
    // variable, and then call that. It's illegal to interact with the ticket after calling the
    // fillCallback, so we have to move it out of _fillCallback so there are no writes to any
    // variables in ASIOTicket after it gets called.
    invariant(_fillCallback);
    auto fillCallback = std::move(_fillCallback);
    fillCallback(status);
}

void TransportLayerASIO::ASIOTicket::fill(bool sync, TicketCallback&& cb) {
    _fillSync = sync;
    dassert(!_fillCallback);
    _fillCallback = std::move(cb);
    fillImpl();
}

}  // namespace transport
}  // namespace mongo
