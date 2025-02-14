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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/shard_id.h"
#include "mongo/rpc/write_concern_error_detail.h"

namespace mongo {
class BSONObj;
class Status;

class ErrorWithWriteConcernErrorInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ErrorWithWriteConcernError;
    ErrorWithWriteConcernErrorInfo(Status mainError, WriteConcernErrorDetail statusWithWCError);

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    const Status& getMainStatus() const;
    const WriteConcernErrorDetail& getWriteConcernErrorDetail() const;

private:
    const Status _mainError;
    const WriteConcernErrorDetail _wcError;
};

Status getStatusWithWCErrorDetailFromCommandResult(const BSONObj& result);

Status getStatusWithWCErrorDetailFromCommandResult(const BSONObj& result,
                                                   const ShardId& shardId,
                                                   BSONObjBuilder& bob);

/**
 * Converts "result" into a Status object.  The input is expected to be the object returned
 * by running a command.  Returns ErrorCodes::CommandResultSchemaViolation if "result" does
 * not look like the result of a command.
 *
 * Command results must have a field called "ok" whose value may be interpreted as a boolean.
 * If the value interpreted as a boolean is true, the resultant status is Status::OK().
 * Otherwise, it is an error status.  The code comes from the "code" field, if present and
 * number-ish, while the reason message will come from the errmsg field, if present and
 * string-ish.
 */
Status getStatusFromCommandResult(const BSONObj& result);

/**
 * Converts error "result" into a Status object.  The input is expected to be the object returned
 * by running a command.  Assumes the result does not represent ok: 1.
 */
Status getErrorStatusFromCommandResult(const BSONObj& result);


/**
 * Extracts the write concern error from a command response.
 */
Status getWriteConcernStatusFromCommandResult(const BSONObj& cmdResponse);


/**
 * Extracts the first write error from a command response and converts it into a status. This
 * ignores all errors after the first and does not preserve the write error index, so it should not
 * be used with bulk writes.
 */
Status getFirstWriteErrorStatusFromCommandResult(const BSONObj& cmdResponse);

/**
 * Extracts the first write error from a bulk write command response and converts it into a status.
 * This ignores all errors after the first.
 */
Status getFirstWriteErrorStatusFromBulkWriteResult(const BSONObj& cmdResponse);

/**
 * Extracts any type of error from a write command response.
 */
Status getStatusFromWriteCommandReply(const BSONObj& cmdResponse);

void appendWriteConcernErrorDetailToCommandResponse(const ShardId& shardId,
                                                    WriteConcernErrorDetail wcError,
                                                    BSONObjBuilder& responseBuilder);

}  // namespace mongo
