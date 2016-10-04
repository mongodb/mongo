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

#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/net/message.h"

namespace mongo {

namespace transport {

class Session;
class TransportLayer;

}  // namespace transport

class ServiceEntryPointMock : public ServiceEntryPoint {
    MONGO_DISALLOW_COPYING(ServiceEntryPointMock);

public:
    ServiceEntryPointMock(transport::TransportLayer* tl);

    virtual ~ServiceEntryPointMock();

    /**
     * This method will spawn a thread that will do the following:
     *
     * - call tl->sourceMessage()
     * - call tl->wait()
     * - call tl->sinkMessage() with { ok : 1 }
     * - call tl->wait()
     *
     * ...repeat until wait() returns an error.
     */
    void startSession(transport::Session&& session) override;

private:
    void run(transport::Session&& session);

    transport::TransportLayer* _tl;

    Message _outMessage;

    stdx::mutex _shutdownLock;
    bool _inShutdown;

    std::vector<stdx::thread> _threads;
};

}  // namespace mongo
