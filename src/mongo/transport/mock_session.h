/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/transport/session.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace transport {

class TransportLayer;

class MockSession : public Session {
    MONGO_DISALLOW_COPYING(MockSession);

public:
    static std::shared_ptr<MockSession> create(TransportLayer* tl) {
        std::shared_ptr<MockSession> handle(new MockSession(tl));
        return handle;
    }

    static std::shared_ptr<MockSession> create(HostAndPort remote,
                                               HostAndPort local,
                                               TransportLayer* tl) {
        std::shared_ptr<MockSession> handle(
            new MockSession(std::move(remote), std::move(local), tl));
        return handle;
    }

    TransportLayer* getTransportLayer() const override {
        return _tl;
    }

    const HostAndPort& remote() const override {
        return _remote;
    }

    const HostAndPort& local() const override {
        return _local;
    }

protected:
    explicit MockSession(TransportLayer* tl) : _tl(tl), _remote(), _local() {}
    explicit MockSession(HostAndPort remote, HostAndPort local, TransportLayer* tl)
        : _tl(tl), _remote(std::move(remote)), _local(std::move(local)) {}

    TransportLayer* _tl;

    HostAndPort _remote;
    HostAndPort _local;
};

}  // namespace transport
}  // namespace mongo
