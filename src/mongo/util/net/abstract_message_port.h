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
#include "mongo/util/net/message.h"
#include "mongo/util/net/sock.h"

namespace mongo {

class AbstractMessagingPort {
    MONGO_DISALLOW_COPYING(AbstractMessagingPort);

public:
    AbstractMessagingPort() : tag(0), _connectionId(0) {}
    virtual ~AbstractMessagingPort() {}
    // like the reply below, but doesn't rely on received.data still being available
    virtual void reply(Message& received, Message& response, int32_t responseToMsgId) = 0;
    virtual void reply(Message& received, Message& response) = 0;

    virtual HostAndPort remote() const = 0;
    virtual unsigned remotePort() const = 0;
    virtual SockAddr remoteAddr() const = 0;
    virtual SockAddr localAddr() const = 0;

    void setX509SubjectName(const std::string& x509SubjectName) {
        _x509SubjectName = x509SubjectName;
    }

    std::string getX509SubjectName() {
        return _x509SubjectName;
    }

    long long connectionId() const {
        return _connectionId;
    }
    void setConnectionId(long long connectionId);

public:
    // TODO make this private with some helpers

    /* ports can be tagged with various classes.  see closeAllSockets(tag). defaults to 0. */
    unsigned tag;

private:
    long long _connectionId;
    std::string _x509SubjectName;
};

}  // namespace mongo
