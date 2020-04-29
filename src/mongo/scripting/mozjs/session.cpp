/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/scripting/mozjs/session.h"

#include "mongo/logv2/log.h"
#include "mongo/scripting/mozjs/bson.h"
#include "mongo/scripting/mozjs/implscope.h"
#include "mongo/scripting/mozjs/mongo.h"
#include "mongo/scripting/mozjs/scripting_util_gen.h"
#include "mongo/scripting/mozjs/valuereader.h"
#include "mongo/scripting/mozjs/wrapconstrainedmethod.h"
#include "mongo/util/str.h"

namespace mongo {
namespace mozjs {

const JSFunctionSpec SessionInfo::methods[8] = {
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(end, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getId, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getTxnState, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(setTxnState, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(getTxnNumber, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(setTxnNumber, SessionInfo),
    MONGO_ATTACH_JS_CONSTRAINED_METHOD_NO_PROTO(incrementTxnNumber, SessionInfo),
    JS_FS_END,
};

const char* const SessionInfo::className = "Session";
struct SessionHolder {
    enum class TransactionState { kActive, kInactive, kCommitted, kAborted };
    // txnNumber starts at -1 because when we increment it, the first transaction
    // and retryable write will both have a txnNumber of 0.
    SessionHolder(std::shared_ptr<DBClientBase> client, BSONObj lsid)
        : client(std::move(client)),
          lsid(std::move(lsid)),
          txnState(TransactionState::kInactive),
          txnNumber(-1) {}

    std::shared_ptr<DBClientBase> client;
    BSONObj lsid;
    TransactionState txnState;
    std::int64_t txnNumber;
};

namespace {

StringData transactionStateName(SessionHolder::TransactionState state) {
    switch (state) {
        case SessionHolder::TransactionState::kActive:
            return "active"_sd;
        case SessionHolder::TransactionState::kInactive:
            return "inactive"_sd;
        case SessionHolder::TransactionState::kCommitted:
            return "committed"_sd;
        case SessionHolder::TransactionState::kAborted:
            return "aborted"_sd;
    }

    MONGO_UNREACHABLE;
}

SessionHolder::TransactionState transactionStateEnum(StringData name) {
    if (name == "active"_sd) {
        return SessionHolder::TransactionState::kActive;
    } else if (name == "inactive"_sd) {
        return SessionHolder::TransactionState::kInactive;
    } else if (name == "committed"_sd) {
        return SessionHolder::TransactionState::kCommitted;
    } else if (name == "aborted"_sd) {
        return SessionHolder::TransactionState::kAborted;
    } else {
        uasserted(ErrorCodes::BadValue, str::stream() << "Invalid TransactionState name: " << name);
    }
}

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

    BSONObj out;

    if (holder->txnState == SessionHolder::TransactionState::kActive) {
        holder->txnState = SessionHolder::TransactionState::kAborted;
        BSONObj abortObj = BSON("abortTransaction" << 1 << "lsid" << holder->lsid << "txnNumber"
                                                   << holder->txnNumber << "autocommit" << false);

        MONGO_COMPILER_VARIABLE_UNUSED auto ignored =
            holder->client->runCommand("admin", abortObj, out);
    }

    EndSessions es;

    es.setEndSessions({holder->lsid});

    MONGO_COMPILER_VARIABLE_UNUSED auto ignored =
        holder->client->runCommand("admin", es.toBSON(), out);

    holder->client.reset();
}

}  // namespace

void SessionInfo::finalize(js::FreeOp* fop, JSObject* obj) {
    auto holder = getHolder(obj);

    if (holder) {
        const auto lsid = holder->lsid;

        try {
            endSession(holder);
        } catch (...) {
            auto status = exceptionToStatus();

            try {
                LOGV2_INFO(22791,
                           "Failed to end session {lsid} due to {status}",
                           "lsid"_attr = lsid,
                           "status"_attr = status);
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

void SessionInfo::Functions::getTxnState::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);
    uassert(ErrorCodes::BadValue, "getTxnState takes no arguments", args.length() == 0);

    ValueReader(cx, args.rval()).fromStringData(transactionStateName(holder->txnState));
}

void SessionInfo::Functions::setTxnState::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);
    uassert(ErrorCodes::BadValue, "setTxnState takes 1 argument", args.length() == 1);

    auto arg = args.get(0);
    holder->txnState = transactionStateEnum(ValueWriter(cx, arg).toString().c_str());
    args.rval().setUndefined();
}

void SessionInfo::Functions::getTxnNumber::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);
    uassert(ErrorCodes::BadValue, "getTxnNumber takes no arguments", args.length() == 0);

    ValueReader(cx, args.rval()).fromInt64(holder->txnNumber);
}

void SessionInfo::Functions::setTxnNumber::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);
    uassert(ErrorCodes::BadValue, "setTxnNumber takes 1 argument", args.length() == 1);

    auto arg = args.get(0);
    holder->txnNumber = ValueWriter(cx, arg).toInt64();
    args.rval().setUndefined();
}

void SessionInfo::Functions::incrementTxnNumber::call(JSContext* cx, JS::CallArgs args) {
    auto holder = getHolder(args);
    invariant(holder);
    uassert(ErrorCodes::BadValue, "incrementTxnNumber takes no arguments", args.length() == 0);

    ++holder->txnNumber;
    args.rval().setUndefined();
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
