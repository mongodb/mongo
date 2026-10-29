// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/legacy_reply.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbmessage.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace rpc {
using namespace std::literals::string_view_literals;

LegacyReply::LegacyReply(const Message* message) {
    invariant(message->operation() == opReply);

    QueryResult::View qr = message->singleData().view2ptr();

    // should be checked by caller.
    invariant(qr.msgdata().getNetworkOp() == opReply);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad cursorId field,"
                          << " expected a value of 0 but got " << qr.getCursorId(),
            qr.getCursorId() == 0);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad nReturned field,"
                          << " expected a value of 1 but got " << qr.getNReturned(),
            qr.getNReturned() == 1);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad startingFrom field,"
                          << " expected a value of 0 but got " << qr.getStartingFrom(),
            qr.getStartingFrom() == 0);

    auto status = rpc::checkBSONObj(qr.data(), qr.dataLen());
    uassert(ErrorCodes::InvalidBSON,
            str::stream() << "Got legacy command reply with invalid BSON in the metadata field"
                          << causedBy(status),
            status.isOK());

    _commandReply = BSONObj(qr.data());
    _commandReply.shareOwnershipWith(message->sharedBuffer());

    if (_commandReply.firstElementFieldName() == "$err"sv) {
        // Upconvert legacy errors
        auto codeElement = _commandReply["code"];
        int code = codeElement.numberInt();
        if (!code) {
            code = ErrorCodes::UnknownError;
        }

        auto errmsg = _commandReply.firstElement().String();
        Status status(ErrorCodes::Error(code), errmsg, _commandReply);

        BSONObjBuilder bob;
        bob.append("ok", 0.0);
        bob.append("code", status.code());
        bob.append("errmsg", status.reason());
        if (auto extraInfo = status.extraInfo()) {
            extraInfo->serialize(&bob);
        }
        _commandReply = bob.obj();
    }

    return;
}

const BSONObj& LegacyReply::getCommandReply() const {
    return _commandReply;
}

Protocol LegacyReply::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

}  // namespace rpc
}  // namespace mongo
