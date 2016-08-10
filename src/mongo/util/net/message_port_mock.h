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

#pragma once

#include <vector>

#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/sockaddr.h"

namespace mongo {

class MessagingPortMock final : public AbstractMessagingPort {
    MONGO_DISALLOW_COPYING(MessagingPortMock);

public:
    MessagingPortMock();
    ~MessagingPortMock();

    void setTimeout(Milliseconds millis) override;

    void shutdown() override;

    bool call(Message& toSend, Message& response) override;

    bool recv(Message& m) override;

    void reply(Message& received, Message& response, int32_t responseToMsgId) override;
    void reply(Message& received, Message& response) override;

    void say(Message& toSend, int responseTo = 0) override;
    void say(const Message& toSend) override;

    bool connect(SockAddr& farEnd) override;

    void send(const char* data, int len, const char* context) override;
    void send(const std::vector<std::pair<char*, int>>& data, const char* context) override;

    HostAndPort remote() const override;
    unsigned remotePort() const override;
    SockAddr remoteAddr() const override;
    SockAddr localAddr() const override;

    bool isStillConnected() const override;

    void setLogLevel(logger::LogSeverity logLevel) override;

    void clearCounters() override;

    long long getBytesIn() const override;

    long long getBytesOut() const override;

    uint64_t getSockCreationMicroSec() const override;

    void setX509SubjectName(const std::string& x509SubjectName) override;

    std::string getX509SubjectName() const override;

    void setConnectionId(const long long connectionId) override;

    long long connectionId() const override;

    void setTag(const AbstractMessagingPort::Tag tag) override;

    AbstractMessagingPort::Tag getTag() const override;

    bool secure(SSLManagerInterface* ssl, const std::string& remoteHost) override;

    void setRemote(const HostAndPort& remote);

private:
    HostAndPort _remote;
};

}  // namespace mongo
