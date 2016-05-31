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

#include "mongo/platform/basic.h"

#include "mongo/transport/service_entry_point_mock.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {

using namespace transport;

namespace {
void setOkResponse(Message* m) {
    // Need to set up our { ok : 1 } response.
    BufBuilder b{};

    // Leave room for the message header
    b.skip(mongo::MsgData::MsgDataHeaderSize);

    // Add our response
    auto okObj = BSON("ok" << 1.0);
    okObj.appendSelfToBufBuilder(b);

    // Add some metadata
    auto metadata = BSONObj();
    metadata.appendSelfToBufBuilder(b);

    // Set Message header fields
    MsgData::View msg = b.buf();
    msg.setLen(b.len());
    msg.setOperation(dbCommandReply);

    // Set the message, transfer buffer ownership to Message
    m->reset();
    m->setData(b.release());
}

}  // namespace

ServiceEntryPointMock::ServiceEntryPointMock(transport::TransportLayer* tl)
    : _tl(tl), _outMessage(), _inShutdown(false) {
    setOkResponse(&_outMessage);
}

ServiceEntryPointMock::~ServiceEntryPointMock() {
    {
        stdx::lock_guard<stdx::mutex> lk(_shutdownLock);
        _inShutdown = true;
    }

    for (auto& t : _threads) {
        t.join();
    }
}

void ServiceEntryPointMock::startSession(transport::Session&& session) {
    _threads.emplace_back(&ServiceEntryPointMock::run, this, std::move(session));
}

void ServiceEntryPointMock::run(transport::Session&& session) {
    Message inMessage;
    while (true) {
        {
            stdx::lock_guard<stdx::mutex> lk(_shutdownLock);
            if (_inShutdown)
                break;
        }

        // sourceMessage()
        if (!session.sourceMessage(&inMessage).wait().isOK()) {
            break;
        }

        // sinkMessage()
        if (!session.sinkMessage(_outMessage).wait().isOK()) {
            break;
        }
    }
}

}  // namespace mongo
