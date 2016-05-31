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

#include "mongo/config.h"
#include "mongo/logger/log_severity.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/time_support.h"

namespace mongo {

class SSLManagerInterface;

class AbstractMessagingPort {
    MONGO_DISALLOW_COPYING(AbstractMessagingPort);

protected:
    AbstractMessagingPort() = default;

public:
    virtual ~AbstractMessagingPort() = default;

    using Tag = uint32_t;

    /**
     * Used when closing sockets. This value will not close any tagged sockets.
     */
    static const Tag kSkipAllMask = 0xffffffff;

    /**
     * Sets the maximum amount of time (in ms) to wait for io operations to run.
     */
    virtual void setTimeout(Milliseconds millis) = 0;

    /**
     * Closes the underlying socket.
     */
    virtual void shutdown() = 0;

    /**
     * Sends a message and waits for a response. This is equivalent to calling `say` then `recv`.
     */
    virtual bool call(Message& toSend, Message& response) = 0;

    /**
     * Reads the next message from the socket.
     */
    virtual bool recv(Message& m) = 0;

    /**
     * Sends a message as a reply to a received message.
     */
    virtual void reply(Message& received, Message& response, int32_t responseToMsgId) = 0;
    virtual void reply(Message& received, Message& response) = 0;

    /**
     * Sends the message.
     */
    virtual void say(Message& toSend, int responseTo = 0) = 0;

    /**
     * Sends the message (does not set headers).
     */
    virtual void say(const Message& toSend) = 0;

    /**
     * Sends the data over the socket.
     */
    virtual void send(const char* data, int len, const char* context) = 0;
    virtual void send(const std::vector<std::pair<char*, int>>& data, const char* context) = 0;

    /**
     * Connect to the remote endpoint.
     */
    virtual bool connect(SockAddr& farEnd) = 0;

    /**
     * The remote endpoint.
     */
    virtual HostAndPort remote() const = 0;

    /**
     * The port of the remote endpoint.
     */
    virtual unsigned remotePort() const = 0;

    /**
     * The remote endpoint.
     */
    virtual SockAddr remoteAddr() const = 0;

    /**
     * The local endpoint.
     */
    virtual SockAddr localAddr() const = 0;

    /**
     * Whether or not this is still connected.
     */
    virtual bool isStillConnected() const = 0;

    /**
     * Point in time (in micro seconds) when this was created.
     */
    virtual uint64_t getSockCreationMicroSec() const = 0;

    /**
     * Sets the severity level for all logging.
     */
    virtual void setLogLevel(logger::LogSeverity logLevel) = 0;

    /**
     * Clear the io counters.
     */
    virtual void clearCounters() = 0;

    /**
     * The total number of bytes read (since initialization or clearing the counters).
     */
    virtual long long getBytesIn() const = 0;

    /**
     * The total number of bytes written (since initialization or clearing the counters).
     */
    virtual long long getBytesOut() const = 0;

    /**
     * Set the x509 subject name (used for SSL).
     */
    virtual void setX509SubjectName(const std::string& x509SubjectName) = 0;

    /**
     * Get the current x509 subject name (used for SSL).
     */
    virtual std::string getX509SubjectName() const = 0;

    /**
     * Set the connection ID.
     */
    virtual void setConnectionId(const long long connectionId) = 0;

    /**
     * Get the connection ID.
     */
    virtual long long connectionId() const = 0;

    /**
     * Set the tag for this messaging port, used when closing with a mask.
     * @see Listener::closeMessagingPorts()
     */
    virtual void setTag(const Tag tag) = 0;

    /**
     * Get the tag for this messaging port.
     */
    virtual Tag getTag() const = 0;

    /**
     * Initiates the TLS/SSL handshake on this AbstractMessagingPort. When this function returns,
     * further communication on this AbstractMessagingPort will be encrypted.
     * ssl - Pointer to the global SSLManager.
     * remoteHost - The hostname of the remote server.
     */
    virtual bool secure(SSLManagerInterface* ssl, const std::string& remoteHost) = 0;
};

}  // namespace mongo
