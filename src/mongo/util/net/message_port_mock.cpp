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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/message_port_mock.h"

namespace mongo {

using std::string;

MessagingPortMock::MessagingPortMock() : AbstractMessagingPort() {}
MessagingPortMock::~MessagingPortMock() {}

void MessagingPortMock::setTimeout(Milliseconds millis) {}

void MessagingPortMock::shutdown() {}

bool MessagingPortMock::call(Message& toSend, Message& response) {
    return true;
}

bool MessagingPortMock::recv(Message& m) {
    return true;
}

void MessagingPortMock::reply(Message& received, Message& response, int32_t responseToMsgId) {}
void MessagingPortMock::reply(Message& received, Message& response) {}

void MessagingPortMock::say(Message& toSend, int responseTo) {}
void MessagingPortMock::say(const Message& toSend) {}

bool MessagingPortMock::connect(SockAddr& farEnd) {
    return true;
}

void MessagingPortMock::send(const char* data, int len, const char* context) {}
void MessagingPortMock::send(const std::vector<std::pair<char*, int>>& data, const char* context) {}

HostAndPort MessagingPortMock::remote() const {
    return _remote;
}

unsigned MessagingPortMock::remotePort() const {
    return _remote.port();
}

SockAddr MessagingPortMock::remoteAddr() const {
    return SockAddr{};
}

SockAddr MessagingPortMock::localAddr() const {
    return SockAddr{};
}

bool MessagingPortMock::isStillConnected() const {
    return true;
}

void MessagingPortMock::setLogLevel(logger::LogSeverity logLevel) {}

void MessagingPortMock::clearCounters() {}

long long MessagingPortMock::getBytesIn() const {
    return 0;
}

long long MessagingPortMock::getBytesOut() const {
    return 0;
}


uint64_t MessagingPortMock::getSockCreationMicroSec() const {
    return 0;
}

void MessagingPortMock::setX509SubjectName(const std::string& x509SubjectName) {}

std::string MessagingPortMock::getX509SubjectName() const {
    return "mock";
}

void MessagingPortMock::setConnectionId(const long long connectionId) {}

long long MessagingPortMock::connectionId() const {
    return 42;
}

void MessagingPortMock::setTag(const AbstractMessagingPort::Tag tag) {}

AbstractMessagingPort::Tag MessagingPortMock::getTag() const {
    return 0;
}

bool MessagingPortMock::secure(SSLManagerInterface* ssl, const std::string& remoteHost) {
    return true;
}

void MessagingPortMock::setRemote(const HostAndPort& remote) {
    _remote = remote;
}

}  // namespace mongo
