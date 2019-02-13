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

#include "mongo/platform/basic.h"

#include "mongo/rpc/legacy_reply.h"

#include <tuple>
#include <utility>

#include "mongo/bson/bson_validate.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/metadata.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace rpc {

LegacyReply::LegacyReply(const Message* message) {
    invariant(message->operation() == opReply);

    QueryResult::View qr = message->singleData().view2ptr();

    // should be checked by caller.
    invariant(qr.msgdata().getNetworkOp() == opReply);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad cursorId field,"
                          << " expected a value of 0 but got "
                          << qr.getCursorId(),
            qr.getCursorId() == 0);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad nReturned field,"
                          << " expected a value of 1 but got "
                          << qr.getNReturned(),
            qr.getNReturned() == 1);

    uassert(ErrorCodes::BadValue,
            str::stream() << "Got legacy command reply with a bad startingFrom field,"
                          << " expected a value of 0 but got "
                          << qr.getStartingFrom(),
            qr.getStartingFrom() == 0);

    auto status = Validator<BSONObj>::validateLoad(qr.data(), qr.dataLen());
    uassert(ErrorCodes::InvalidBSON,
            str::stream() << "Got legacy command reply with invalid BSON in the metadata field"
                          << causedBy(status),
            status.isOK());

    _commandReply = BSONObj(qr.data());
    _commandReply.shareOwnershipWith(message->sharedBuffer());

    if (_commandReply.firstElementFieldName() == "$err"_sd) {
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
