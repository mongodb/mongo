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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/db/service_entry_point_mongod.h"

#include <vector>

#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/instance.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace {

// Set up proper headers for formatting an exhaust request, if we need to
bool setExhaustMessage(Message* m, const DbResponse& dbresponse) {
    MsgData::View header = dbresponse.response.header();
    QueryResult::View qr = header.view2ptr();
    long long cursorid = qr.getCursorId();

    if (!cursorid) {
        return false;
    }

    verify(dbresponse.exhaustNS.size() && dbresponse.exhaustNS[0]);

    auto ns = dbresponse.exhaustNS;  // reset() will free this

    m->reset();

    BufBuilder b(512);
    b.appendNum(static_cast<int>(0) /* size set later in appendData() */);
    b.appendNum(header.getId());
    b.appendNum(header.getResponseToMsgId());
    b.appendNum(static_cast<int>(dbGetMore));
    b.appendNum(static_cast<int>(0));
    b.appendStr(ns);
    b.appendNum(static_cast<int>(0));  // ntoreturn
    b.appendNum(cursorid);

    MsgData::View(b.buf()).setLen(b.len());
    m->setData(b.release());

    return true;
}

}  // namespace

using transport::Session;
using transport::TransportLayer;

ServiceEntryPointMongod::ServiceEntryPointMongod(TransportLayer* tl) : _tl(tl) {}

void ServiceEntryPointMongod::startSession(Session&& session) {
    launchWrappedServiceEntryWorkerThread(std::move(session),
                                          [this](Session* session) { _sessionLoop(session); });
}

void ServiceEntryPointMongod::_sessionLoop(Session* session) {
    Message inMessage;
    bool inExhaust = false;
    int64_t counter = 0;

    while (true) {
        // 1. Source a Message from the client (unless we are exhausting)
        if (!inExhaust) {
            inMessage.reset();
            auto status = session->sourceMessage(&inMessage).wait();

            if (ErrorCodes::isInterruption(status.code())) {
                break;
            }

            uassertStatusOK(status);
        }

        // 2. Pass sourced Message up to mongod
        DbResponse dbresponse;
        {
            auto opCtx = cc().makeOperationContext();
            assembleResponse(opCtx.get(), inMessage, dbresponse, session->remote());

            // opCtx must go out of scope here so that the operation cannot show
            // up in currentOp results after the response reaches the client
        }

        // 3. Format our response, if we have one
        Message& toSink = dbresponse.response;
        if (!toSink.empty()) {
            toSink.header().setId(nextMessageId());
            toSink.header().setResponseToMsgId(inMessage.header().getId());

            // If this is an exhaust cursor, don't source more Messages
            if (dbresponse.exhaustNS.size() > 0 && setExhaustMessage(&inMessage, dbresponse)) {
                log() << "we are in exhaust";
                inExhaust = true;
            } else {
                inExhaust = false;
            }

            // 4. Sink our response to the client
            uassertStatusOK(session->sinkMessage(toSink).wait());
        } else {
            inExhaust = false;
        }

        if ((counter++ & 0xf) == 0) {
            markThreadIdle();
        }
    }
}

}  // namespace mongo
