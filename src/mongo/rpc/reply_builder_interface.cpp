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

#include "mongo/rpc/reply_builder_interface.h"

#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/jsobj.h"
#include "mongo/idl/basic_types_gen.h"

namespace mongo {
namespace rpc {
namespace {

const char kOKField[] = "ok";
const char kCodeField[] = "code";
const char kCodeNameField[] = "codeName";
const char kErrorField[] = "errmsg";

// Similar to appendCommandStatusNoThrow (duplicating logic here to avoid cyclic library dependency)
BSONObj augmentReplyWithStatus(const Status& status, BSONObj reply) {
    auto okField = reply.getField(kOKField);
    if (!okField.eoo() && okField.trueValue()) {
        return reply;
    }

    BSONObjBuilder bob(std::move(reply));
    if (okField.eoo()) {
        bob.append(kOKField, status.isOK() ? 1.0 : 0.0);
    }
    if (status.isOK()) {
        return bob.obj();
    }

    if (!bob.asTempObj().hasField(kErrorField)) {
        bob.append(kErrorField, status.reason());
    }

    if (!bob.asTempObj().hasField(kCodeField)) {
        bob.append(kCodeField, status.code());
        bob.append(kCodeNameField, ErrorCodes::errorString(status.code()));
    }

    if (auto extraInfo = status.extraInfo()) {
        extraInfo->serialize(&bob);
    }

    // Ensure the error reply satisfies the IDL-defined requirements.
    // Only validate error reply in test mode so that we don't expose users to errors if we
    // construct an invalid error reply.
    if (getTestCommandsEnabled()) {
        try {
            ErrorReply::parse(IDLParserContext("augmentReplyWithStatus"), bob.asTempObj());
        } catch (const DBException&) {
            invariant(false,
                      "invalid error-response to a command constructed in "
                      "rpc::augmentReplyWithStatus. All erroring command responses "
                      "must comply with the format specified by the IDL-defined struct ErrorReply, "
                      "defined in idl/basic_types.idl");
        }
    }

    return bob.obj();
}

}  // namespace

ReplyBuilderInterface& ReplyBuilderInterface::setCommandReply(StatusWith<BSONObj> commandReply) {
    auto reply = commandReply.isOK() ? std::move(commandReply.getValue()) : BSONObj();
    return setRawCommandReply(augmentReplyWithStatus(commandReply.getStatus(), std::move(reply)));
}

ReplyBuilderInterface& ReplyBuilderInterface::setCommandReply(Status nonOKStatus,
                                                              BSONObj extraErrorInfo) {
    invariant(!nonOKStatus.isOK());
    return setRawCommandReply(augmentReplyWithStatus(nonOKStatus, std::move(extraErrorInfo)));
}

bool ReplyBuilderInterface::shouldRunAgainForExhaust() const {
    return _shouldRunAgainForExhaust;
}


boost::optional<BSONObj> ReplyBuilderInterface::getNextInvocation() const {
    return _nextInvocation;
}

void ReplyBuilderInterface::setNextInvocation(boost::optional<BSONObj> nextInvocation) {
    _shouldRunAgainForExhaust = true;
    _nextInvocation = nextInvocation;
}

}  // namespace rpc
}  // namespace mongo
