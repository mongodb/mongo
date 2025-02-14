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

#include <fmt/core.h>
#include <fmt/format.h>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
const std::string kCmdResponseWriteErrorsField = "writeErrors";

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ErrorWithWriteConcernErrorInfo);


boost::optional<WriteConcernErrorDetail> extractWriteConcernErrorDetail(const BSONObj& result) {
    auto wcErrorDetailPtr = getWriteConcernErrorDetailFromBSONObj(result);
    if (!wcErrorDetailPtr) {
        return boost::none;
    }
    return *wcErrorDetailPtr;
}

}  // namespace

ErrorWithWriteConcernErrorInfo::ErrorWithWriteConcernErrorInfo(
    Status mainError, WriteConcernErrorDetail writeConcernError)
    : _mainError(std::move(mainError)), _wcError(std::move(writeConcernError)) {}

std::shared_ptr<const ErrorExtraInfo> ErrorWithWriteConcernErrorInfo::parse(const BSONObj& doc) {
    uasserted(
        1004350,
        fmt::format(
            "ErrorWithWriteConcernErrorInfo should never appear in command result BSON object: {}",
            doc.toString()));
    return {};
}

void ErrorWithWriteConcernErrorInfo::serialize(BSONObjBuilder* bob) const {
    _mainError.serialize(bob);
    bob->append(kWriteConcernErrorFieldName, _wcError.toBSON());
}

const Status& ErrorWithWriteConcernErrorInfo::getMainStatus() const {
    return _mainError;
}

const WriteConcernErrorDetail& ErrorWithWriteConcernErrorInfo::getWriteConcernErrorDetail() const {
    return _wcError;
}

Status getStatusWithWCErrorDetailFromCommandResult(const BSONObj& result) {
    auto mainStatus = getStatusFromCommandResult(result);
    auto optionalWCErrorDetail = extractWriteConcernErrorDetail(result);
    if (!optionalWCErrorDetail) {
        return mainStatus;
    }
    if (mainStatus.code() == ErrorCodes::ErrorWithWriteConcernError) {
        return mainStatus;
    }
    return Status{ErrorWithWriteConcernErrorInfo{mainStatus, optionalWCErrorDetail.get()},
                  "Error paired with write concern error"};
}

void appendWriteConcernErrorDetailToCommandResponse(const ShardId& shardId,
                                                    WriteConcernErrorDetail wcError,
                                                    BSONObjBuilder& responseBuilder) {
    if (responseBuilder.hasField(kWriteConcernErrorFieldName)) {
        return;
    }

    auto status = wcError.toStatus();
    wcError.setStatus(
        status.withReason(str::stream() << status.reason() << " at " << shardId.toString()));
    responseBuilder.append(kWriteConcernErrorFieldName, wcError.toBSON());
}

Status getStatusWithWCErrorDetailFromCommandResult(const BSONObj& result,
                                                   const ShardId& shardId,
                                                   BSONObjBuilder& bob) {
    auto mainStatus = getStatusFromCommandResult(result);
    auto optionalWcErrorDetail = extractWriteConcernErrorDetail(result);

    if (!optionalWcErrorDetail) {
        return mainStatus;
    }
    appendWriteConcernErrorDetailToCommandResponse(shardId, *optionalWcErrorDetail, bob);
    return Status{ErrorWithWriteConcernErrorInfo{mainStatus, *optionalWcErrorDetail},
                  "error paired with write concern error"};
}

Status getStatusFromCommandResult(const BSONObj& result) {
    BSONElement okElement = result["ok"];

    // StaleConfig doesn't pass "ok" in legacy servers
    BSONElement dollarErrElement = result["$err"];

    if (okElement.eoo() && dollarErrElement.eoo()) {
        return Status(ErrorCodes::CommandResultSchemaViolation,
                      str::stream() << "No \"ok\" field in command result " << result);
    }
    if (okElement.trueValue()) {
        return Status::OK();
    }
    return getErrorStatusFromCommandResult(result);
}

Status getErrorStatusFromCommandResult(const BSONObj& result) {
    BSONElement codeElement = result["code"];
    BSONElement errmsgElement = result["errmsg"];

    int code = codeElement.numberInt();
    if (0 == code) {
        code = ErrorCodes::UnknownError;
    }

    std::string errmsg;
    if (errmsgElement.type() == String) {
        errmsg = errmsgElement.String();
    } else if (!errmsgElement.eoo()) {
        errmsg = errmsgElement.toString();
    }

    // we can't use startsWith(errmsg, "no such")
    // as we have errors such as "no such collection"
    if (code == ErrorCodes::UnknownError &&
        (str::startsWith(errmsg, "no such cmd") || str::startsWith(errmsg, "no such command"))) {
        code = ErrorCodes::CommandNotFound;
    }
    return Status(ErrorCodes::Error(code), std::move(errmsg), result);
}

Status getWriteConcernStatusFromCommandResult(const BSONObj& obj) {
    auto optionalWcErrorDetail = extractWriteConcernErrorDetail(obj);
    if (!optionalWcErrorDetail) {
        return Status::OK();
    }

    return optionalWcErrorDetail->toStatus();
}

Status getFirstWriteErrorStatusFromCommandResult(const BSONObj& cmdResponse) {
    BSONElement writeErrorElem;
    auto status = bsonExtractTypedField(
        cmdResponse, kCmdResponseWriteErrorsField, BSONType::Array, &writeErrorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status::OK();
        } else {
            return status;
        }
    }

    auto firstWriteErrorElem = writeErrorElem.Obj().firstElement();
    if (!firstWriteErrorElem) {
        return Status::OK();
    }

    if (firstWriteErrorElem.type() != BSONType::Object) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream() << "writeErrors should be an array of objects, found "
                                    << typeName(firstWriteErrorElem.type()));
    }

    auto firstWriteErrorObj = firstWriteErrorElem.Obj();

    return Status(ErrorCodes::Error(firstWriteErrorObj["code"].Int()),
                  firstWriteErrorObj["errmsg"].String(),
                  firstWriteErrorObj);
}

Status getFirstWriteErrorStatusFromBulkWriteResult(const BSONObj& cmdResponse) {
    BSONElement cursorElem;
    Status status = bsonExtractTypedField(cmdResponse, "cursor", Object, &cursorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status::OK();
        } else {
            return status;
        }
    }

    BSONElement firstBatchElem;
    status = bsonExtractTypedField(cursorElem.Obj(), "firstBatch", Array, &firstBatchElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status::OK();
        } else {
            return status;
        }
    }

    // Iterate over firstBatch. Must manually parse the elements since including
    // bulk_write_parser invokes a circular dependency.
    for (const auto& elem : firstBatchElem.Array()) {
        // extract ok field.
        BSONElement okElem;
        status = bsonExtractTypedField(elem.Obj(), "ok", NumberDouble, &okElem);
        if (!status.isOK()) {
            if (status == ErrorCodes::NoSuchKey) {
                continue;
            } else {
                return status;
            }
        }
        if (okElem.Double() == 1.0) {
            continue;
        }
        // extract error code field.
        BSONElement codeElem;
        status = bsonExtractTypedField(elem.Obj(), "code", NumberInt, &codeElem);
        if (!status.isOK()) {
            if (status == ErrorCodes::NoSuchKey) {
                continue;
            } else {
                return status;
            }
        }

        // extract errmsg field.
        BSONElement errMsgElem;
        std::string errmsg = "";
        status = bsonExtractTypedField(elem.Obj(), "errmsg", String, &errMsgElem);
        if (!status.isOK()) {
            if (status != ErrorCodes::NoSuchKey) {
                return status;
            }
        } else {
            errmsg = errMsgElem.str();
        }

        return Status(ErrorCodes::Error(codeElem.Int()), std::move(errmsg), elem.Obj());
    }

    return Status::OK();
}

Status getStatusFromWriteCommandReply(const BSONObj& cmdResponse) {
    auto status = getStatusFromCommandResult(cmdResponse);
    if (!status.isOK()) {
        return status;
    }
    status = getFirstWriteErrorStatusFromCommandResult(cmdResponse);
    if (!status.isOK()) {
        return status;
    }
    status = getFirstWriteErrorStatusFromBulkWriteResult(cmdResponse);
    if (!status.isOK()) {
        return status;
    }
    return getWriteConcernStatusFromCommandResult(cmdResponse);
}

}  // namespace mongo
