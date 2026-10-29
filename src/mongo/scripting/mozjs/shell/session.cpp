// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/scripting/mozjs/shell/session.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/logv2/log.h"
#include "mongo/scripting/mozjs/common/valuereader.h"
#include "mongo/scripting/mozjs/common/valuewriter.h"
#include "mongo/scripting/mozjs/common/wrapconstrainedmethod.h"  // IWYU pragma: keep
#include "mongo/scripting/mozjs/shell/implscope.h"
#include "mongo/scripting/mozjs/shell/scripting_util_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <js/CallArgs.h>
#include <js/Object.h>
#include <js/PropertySpec.h>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


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
using namespace std::literals::string_view_literals;

std::string_view transactionStateName(SessionHolder::TransactionState state) {
    switch (state) {
        case SessionHolder::TransactionState::kActive:
            return "active"sv;
        case SessionHolder::TransactionState::kInactive:
            return "inactive"sv;
        case SessionHolder::TransactionState::kCommitted:
            return "committed"sv;
        case SessionHolder::TransactionState::kAborted:
            return "aborted"sv;
    }

    MONGO_UNREACHABLE;
}

SessionHolder::TransactionState transactionStateEnum(std::string_view name) {
    if (name == "active"sv) {
        return SessionHolder::TransactionState::kActive;
    } else if (name == "inactive"sv) {
        return SessionHolder::TransactionState::kInactive;
    } else if (name == "committed"sv) {
        return SessionHolder::TransactionState::kCommitted;
    } else if (name == "aborted"sv) {
        return SessionHolder::TransactionState::kAborted;
    } else {
        uasserted(ErrorCodes::BadValue, str::stream() << "Invalid TransactionState name: " << name);
    }
}

SessionHolder* getHolder(JSObject* thisv) {
    return JS::GetMaybePtrFromReservedSlot<SessionHolder>(thisv, SessionInfo::SessionHolderSlot);
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

        [[maybe_unused]] auto ignored =
            holder->client->runCommand(DatabaseName::kAdmin, abortObj, out);
    }

    EndSessions es;

    es.setEndSessions({holder->lsid});

    [[maybe_unused]] auto ignored =
        holder->client->runCommand(DatabaseName::kAdmin, es.toBSON(), out);

    holder->client.reset();
}

}  // namespace

void SessionInfo::finalize(JS::GCContext* gcCtx, JSObject* obj) {
    auto holder = getHolder(obj);

    if (holder) {
        const auto lsid = holder->lsid;

        try {
            endSession(holder);
        } catch (...) {
            auto status = exceptionToStatus();

            try {
                LOGV2_INFO(22791,
                           "Failed to end logical session",
                           "lsid"_attr = lsid,
                           "error"_attr = redact(status));
            } catch (...) {
                // This is here in case logging fails.
            }
        }

        getScope(gcCtx)->trackedDelete(holder);
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
    JS::SetReservedSlot(
        obj,
        SessionHolderSlot,
        JS::PrivateValue(scope->trackedNew<SessionHolder>(std::move(client), std::move(lsid))));
}

}  // namespace mozjs
}  // namespace mongo
