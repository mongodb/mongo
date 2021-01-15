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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/transaction_router.h"

namespace mongo {
namespace {

/**
 * Implements the commitTransaction command on mongos.
 */
class ClusterCommitTransactionCmd
    : public BasicCommandWithRequestParser<ClusterCommitTransactionCmd> {
public:
    using BasicCommandWithRequestParser::BasicCommandWithRequestParser;
    using Request = CommitTransaction;
    using Reply = OkReply;

    void validateResult(const BSONObj& resultObj) final {
        auto ctx = IDLParserErrorContext("CommitReply");
        auto status = getStatusFromCommandResult(resultObj);
        auto wcStatus = getWriteConcernStatusFromCommandResult(resultObj);

        if (!wcStatus.isOK()) {
            if (wcStatus.code() == ErrorCodes::TypeMismatch) {
                // Result has "writeConcerError" field but it is not valid wce object.
                uassertStatusOK(wcStatus);
            }
        }

        if (!status.isOK()) {
            // Will throw if the result doesn't match the ErrorReply.
            ErrorReply::parse(ctx, resultObj);
        } else {
            // Will throw if the result doesn't match the committReply.
            Reply::parse(ctx, resultObj);
        }
    }

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Commits a transaction";
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        return Status::OK();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& result) final {
        auto txnRouter = TransactionRouter::get(opCtx);
        uassert(ErrorCodes::InvalidOptions,
                "commitTransaction can only be run within a session",
                txnRouter);

        auto commitRes =
            txnRouter.commitTransaction(opCtx, requestParser.request().getRecoveryToken());
        CommandHelpers::filterCommandReplyForPassthrough(commitRes, &result);
        return true;
    }

} clusterCommitTransactionCmd;

}  // namespace
}  // namespace mongo
