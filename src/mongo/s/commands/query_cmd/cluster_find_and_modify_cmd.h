/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/update_metrics.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/commands/document_shard_key_update_util.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/commands/strategy.h"

#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class FindAndModifyCmd : public BasicCommand {
public:
    FindAndModifyCmd() : BasicCommand("findAndModify", "findandmodify") {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    bool adminOnly() const override {
        return false;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    bool supportsRawData() const override {
        return true;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        return {{level != repl::ReadConcernLevel::kLocalReadConcern &&
                     level != repl::ReadConcernLevel::kSnapshotReadConcern,
                 {ErrorCodes::InvalidOptions, "read concern not supported"}},
                {{ErrorCodes::InvalidOptions, "default read concern not permitted"}}};
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override;

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override;

    bool allowedInTransactions() const final {
        return true;
    }

    bool supportsRetryableWrite() const final {
        return true;
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override;

    /**
     * Changes the shard key for the document if the response object contains a
     * WouldChangeOwningShard error. If the original command was sent as a retryable write, starts a
     * transaction on the same session and txnNum, deletes the original document, inserts the new
     * one, and commits the transaction. If the original command is part of a transaction, deletes
     * the original document and inserts the new one.
     */
    static void handleWouldChangeOwningShardError(OperationContext* opCtx,
                                                  const ShardId& shardId,
                                                  const NamespaceString& nss,
                                                  const BSONObj& cmdObj,
                                                  const Status& responseStatus,
                                                  BSONObjBuilder* result);

protected:
    void doInitializeClusterRole(ClusterRole role) override {
        BasicCommand::doInitializeClusterRole(role);
        _updateMetrics.emplace(getName(), role);
    }

private:
    static bool getCrudProcessedFromCmd(const BSONObj& cmdObj);

    // Catches errors in the given response, and reruns the command if necessary. Uses the given
    // response to construct the findAndModify command result passed to the client.
    static void _constructResult(OperationContext* opCtx,
                                 const ShardId& shardId,
                                 const boost::optional<ShardVersion>& shardVersion,
                                 const boost::optional<DatabaseVersion>& dbVersion,
                                 const NamespaceString& nss,
                                 const BSONObj& cmdObj,
                                 const Status& responseStatus,
                                 const BSONObj& response,
                                 bool isTimeseriesViewRequest,
                                 BSONObjBuilder* result);

    // Two-phase protocol to run a findAndModify command without a shard key or _id.
    static void _runCommandWithoutShardKey(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& cmdObj,
                                           bool isTimeseriesViewRequest,
                                           BSONObjBuilder* result);

    // Two-phase protocol to run an explain for a findAndModify command without a shard key or _id.
    static void _runExplainWithoutShardKey(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& cmdObj,
                                           ExplainOptions::Verbosity verbosity,
                                           BSONObjBuilder* result);

    // Command invocation to be used if a shard key is specified or the collection is unsharded.
    static void _runCommand(OperationContext* opCtx,
                            const ShardId& shardId,
                            const boost::optional<ShardVersion>& shardVersion,
                            const boost::optional<DatabaseVersion>& dbVersion,
                            const NamespaceString& nss,
                            const BSONObj& cmdObj,
                            bool isExplain,
                            boost::optional<bool> allowShardKeyUpdatesWithoutFullShardKeyInQuery,
                            bool isTimeseriesViewRequest,
                            BSONObjBuilder* result);

    // TODO SERVER-67429: Remove this function.
    static void _handleWouldChangeOwningShardErrorRetryableWriteLegacy(
        OperationContext* opCtx,
        const ShardId& shardId,
        const boost::optional<ShardVersion>& shardVersion,
        const boost::optional<DatabaseVersion>& dbVersion,
        const NamespaceString& nss,
        const BSONObj& cmdObj,
        bool isTimeseriesViewRequest,
        BSONObjBuilder* result);

    // Update related command execution metrics.
    boost::optional<UpdateMetrics> _updateMetrics;
};

}  // namespace mongo
