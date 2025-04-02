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

#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
const std::string kCmdResponseWriteConcernField = "writeConcernError";
const std::string kCmdResponseWriteErrorsField = "writeErrors";
}  // namespace

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
    BSONElement wcErrorElem;
    Status status = bsonExtractTypedField(obj, kCmdResponseWriteConcernField, Object, &wcErrorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status::OK();
        } else {
            return status;
        }
    }

    BSONObj wcErrObj(wcErrorElem.Obj());

    WriteConcernErrorDetail wcError;
    std::string wcErrorParseMsg;
    if (!wcError.parseBSON(wcErrObj, &wcErrorParseMsg)) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream()
                          << "Failed to parse write concern section due to " << wcErrorParseMsg);
    }
    std::string wcErrorInvalidMsg;
    if (!wcError.isValid(&wcErrorInvalidMsg)) {
        return Status(ErrorCodes::UnsupportedFormat,
                      str::stream()
                          << "Failed to parse write concern section due to " << wcErrorInvalidMsg);
    }
    return wcError.toStatus();
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
