// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/reply_builder_interface.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/idl/idl_parser.h"

#include <memory>
#include <utility>

#include <boost/optional/optional.hpp>

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
            ErrorReply::parse(bob.asTempObj(), IDLParserContext("augmentReplyWithStatus"));
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
