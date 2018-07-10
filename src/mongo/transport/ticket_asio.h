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

#pragma once

#include "mongo/transport/transport_layer_asio.h"

#include "asio.hpp"

namespace mongo {
namespace transport {

class TransportLayerASIO::ASIOTicket : public TicketImpl {
    MONGO_DISALLOW_COPYING(ASIOTicket);

public:
    explicit ASIOTicket(const ASIOSessionHandle& session, Date_t expiration);

    SessionId sessionId() const final {
        return _sessionId;
    }

    Date_t expiration() const final {
        return _expiration;
    }

    /**
     * Run this ticket's work item.
     */
    void fill(bool sync, TicketCallback&& cb);

protected:
    void finishFill(Status status);
    std::shared_ptr<ASIOSession> getSession();
    bool isSync() const;

    // This must be implemented by the Source/Sink subclasses as the actual implementation
    // of filling the ticket.
    virtual void fillImpl() = 0;

private:
    std::weak_ptr<ASIOSession> _session;
    const SessionId _sessionId;
    const Date_t _expiration;

    TicketCallback _fillCallback;
    bool _fillSync;
};

class TransportLayerASIO::ASIOSourceTicket : public TransportLayerASIO::ASIOTicket {
public:
    ASIOSourceTicket(const ASIOSessionHandle& session, Date_t expiration, Message* msg);

protected:
    void fillImpl() final;

private:
    void _headerCallback(const Status& ec, size_t size);
    void _bodyCallback(const Status& ec, size_t size);

    SharedBuffer _buffer;
    Message* _target;
};

class TransportLayerASIO::ASIOSinkTicket : public TransportLayerASIO::ASIOTicket {
public:
    ASIOSinkTicket(const ASIOSessionHandle& session, Date_t expiration, const Message& msg);

protected:
    void fillImpl() final;

private:
    void _sinkCallback(const Status& ec, size_t size);
    Message _msgToSend;
};

}  // namespace transport
}  // namespace mongo
