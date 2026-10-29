// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/get_status_from_command_result_write_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {

Status getWriteConcernStatusFromCommandResult(const BSONObj& obj) {
    BSONElement wcErrorElem;
    Status status = bsonExtractTypedField(obj, "writeConcernError", BSONType::object, &wcErrorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status::OK();
        } else {
            return status;
        }
    }

    BSONObj wcErrObj(wcErrorElem.Obj());

    WriteConcernErrorDetail wcError;
    std::string msg;
    if (!(wcError.parseBSON(wcErrObj, &msg) && wcError.isValid(&msg))) {
        return Status(ErrorCodes::UnsupportedFormat,
                      fmt::format("Failed to parse write concern section due to {}", msg));
    }
    return wcError.toStatus();
}

Status getFirstWriteErrorStatusFromCommandResult(const BSONObj& cmdResponse) {
    BSONElement writeErrorElem;
    auto status =
        bsonExtractTypedField(cmdResponse, "writeErrors", BSONType::array, &writeErrorElem);
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

    if (firstWriteErrorElem.type() != BSONType::object) {
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
    Status status = bsonExtractTypedField(cmdResponse, "cursor", BSONType::object, &cursorElem);
    if (!status.isOK()) {
        if (status == ErrorCodes::NoSuchKey) {
            return Status::OK();
        } else {
            return status;
        }
    }

    BSONElement firstBatchElem;
    status =
        bsonExtractTypedField(cursorElem.Obj(), "firstBatch", BSONType::array, &firstBatchElem);
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
        status = bsonExtractTypedField(elem.Obj(), "ok", BSONType::numberDouble, &okElem);
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
        status = bsonExtractTypedField(elem.Obj(), "code", BSONType::numberInt, &codeElem);
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
        status = bsonExtractTypedField(elem.Obj(), "errmsg", BSONType::string, &errMsgElem);
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
