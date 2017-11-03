/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/session.h"

#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/end_sessions_gen.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/mongo.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/log.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec SessionInfo::methods[3] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(end, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getId, SessionInfo),
    JS_FS_END,
};

const char* const SessionInfo::className = "Session";

struct SessionHolder {
    SessionHolder(std::shared_ptr<DBClientBase> client, BSONObj lsid)
        : client(std::move(client)), lsid(std::move(lsid)) {}

    std::shared_ptr<DBClientBase> client;
    BSONObj lsid;
};

namespace {

SessionHolder* getHolder(JSObject* thisv) {
    return static_cast<SessionHolder*>(JS_GetPrivate(thisv));
}

SessionHolder* getHolder(JS::CallArgs& args) {
    return getHolder(args.thisv().toObjectOrNull());
}

void endSession(SessionHolder* holder) {
    if (!holder->client) {
        return;
    }

    EndSessions es;

    es.setEndSessions({holder->lsid});

    BSONObj out;
    holder->client->runCommand("admin", es.toBSON(), out);

    holder->client.reset();
}

}  // namespace

void SessionInfo::finalize(JSFreeOp* fop, JSObject* obj) {
    auto holder = getHolder(obj);

    if (holder) {
        const auto lsid = holder->lsid;

        try {
            endSession(holder);
        } catch (...) {
            auto status = exceptionToStatus();

            try {
                LOG(0) << "Failed to end session " << lsid << " due to " << status;
            } catch (...) {
                // This is here in case logging fails.
            }
        }

        getScope(fop)->trackedDelete(holder);
    }
}

void SessionInfo::Functions::end::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);

    endSession(holder);

    args.rval().setUndefined();
}

void SessionInfo::Functions::getId::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);

    ValueReader(cx, args.rval()).fromBSON(holder->lsid, nullptr, 1);
}

void SessionInfo::make(JSContext* cx,
                       JS::MutableHandleObject obj,
                       std::shared_ptr<DBClientBase> client,
                       BSONObj lsid) {
    auto scope = getScope(cx);

    scope->getProto<SessionInfo>().newObject(obj);
    JS_SetPrivate(obj, scope->trackedNew<SessionHolder>(std::move(client), std::move(lsid)));
}

}  // namespace mozjs
}  // namespace mongo
