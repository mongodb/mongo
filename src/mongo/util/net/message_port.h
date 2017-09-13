// message_port.h

/*    Copyright 2009 10gen Inc.
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

#pragma once

#include <vector>

#include "mongo/config.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/sock.h"

namespace mongo {

class MessagingPort;

class MessagingPort final : public AbstractMessagingPort {
public:
    MessagingPort(int fd, const SockAddr& remote);

    // in some cases the timeout will actually be 2x this value - eg we do a partial send,
    // then the timeout fires, then we try to send again, then the timeout fires again with
    // no data sent, then we detect that the other side is down
    MessagingPort(double so_timeout = 0, logger::LogSeverity logLevel = logger::LogSeverity::Log());

    MessagingPort(std::shared_ptr<Socket> socket);

    ~MessagingPort() override;

    void setTimeout(Milliseconds millis) override;

    void shutdown() override;

    /* it's assumed if you reuse a message object, that it doesn't cross MessagingPort's.
       also, the Message data will go out of scope on the subsequent recv call.
    */
    bool recv(Message& m) override;
    bool call(const Message& toSend, Message& response) override;

    void say(const Message& toSend) override;

    unsigned remotePort() const override {
        return _psock->remotePort();
    }
    virtual HostAndPort remote() const override;
    virtual SockAddr remoteAddr() const override;
    virtual SockAddr localAddr() const override;

    void send(const char* data, int len, const char* context) override {
        _psock->send(data, len, context);
    }
    void send(const std::vector<std::pair<char*, int>>& data, const char* context) override {
        _psock->send(data, context);
    }
    bool connect(SockAddr& farEnd) override {
        return _psock->connect(farEnd);
    }

    void setLogLevel(logger::LogSeverity ll) override {
        _psock->setLogLevel(ll);
    }

    void clearCounters() override {
        _psock->clearCounters();
    }

    long long getBytesIn() const override {
        return _psock->getBytesIn();
    }

    long long getBytesOut() const override {
        return _psock->getBytesOut();
    }

    void setX509PeerInfo(SSLPeerInfo x509PeerInfo) override;

    const SSLPeerInfo& getX509PeerInfo() const override;

    void setConnectionId(const long long connectionId) override;

    long long connectionId() const override;

    void setTag(const AbstractMessagingPort::Tag tag) override;

    AbstractMessagingPort::Tag getTag() const override;

    /**
     * Initiates the TLS/SSL handshake on this MessagingPort.
     * When this function returns, further communication on this
     * MessagingPort will be encrypted.
     * ssl - Pointer to the global SSLManager.
     * remoteHost - The hostname of the remote server.
     */
    bool secure(SSLManagerInterface* ssl, const std::string& remoteHost) override {
#ifdef MONGO_CONFIG_SSL
        return _psock->secure(ssl, remoteHost);
#else
        return false;
#endif
    }

    bool isStillConnected() const override {
        return _psock->isStillConnected();
    }

    uint64_t getSockCreationMicroSec() const override {
        return _psock->getSockCreationMicroSec();
    }

private:
    // this is the parsed version of remote
    HostAndPort _remoteParsed;
    SSLPeerInfo _x509PeerInfo;
    long long _connectionId;
    AbstractMessagingPort::Tag _tag;
    std::shared_ptr<Socket> _psock;
};

}  // namespace mongo
