// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <vector>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace analyze_shard_key {

/**
 * The interface used by the analyzeShardKey machinery on a mongod in a shardsvr or standalone
 * replica set to run commands internally against the primary mongod.
 */
class QueryAnalysisClient final {
public:
    QueryAnalysisClient() = default;
    ~QueryAnalysisClient() = default;

    QueryAnalysisClient(const QueryAnalysisClient&) = delete;
    QueryAnalysisClient& operator=(const QueryAnalysisClient&) = delete;

    /**
     * Obtains the service-wide QueryAnalysisClient instance.
     */
    static QueryAnalysisClient& get(OperationContext* opCtx);
    static QueryAnalysisClient& get(ServiceContext* serviceContext);

    /**
     * Sets the task executor to be used for running commands. This must be invoked before this
     * interface can be used to run any command.
     */
    static void setTaskExecutor(ServiceContext* serviceContext,
                                std::shared_ptr<executor::TaskExecutor> executor);

    /*
     * Runs the command 'cmdObj' against the database 'dbName'. If this mongod is currently the
     * primary, runs the command locally. Otherwise, runs the command on the remote primary. Then
     * asserts the command status using the given 'uassertCmdStatusFn' callback. Internally retries
     * the command on retryable errors for a set number of times so the command must be idempotent.
     * Returns the command response.
     */
    BSONObj executeCommandOnPrimary(OperationContext* opCtx,
                                    const DatabaseName& dbName,
                                    const BSONObj& cmdObj,
                                    const std::function<void(const BSONObj&)>& uassertCmdStatusFn);

    /*
     * Inserts the documents 'docs' into the collection 'nss'. If this mongod is currently the
     * primary, runs the insert command locally. Otherwise, runs the command on the remote primary.
     * Then asserts the command status using the 'uassertCmdStatusFn' callback. Internally retries
     * the insert command on retryable errors.
     */
    void insert(OperationContext* opCtx,
                const NamespaceString& nss,
                const std::vector<BSONObj>& docs,
                const std::function<void(const BSONObj&)>& uassertCmdStatusFn);

private:
    /*
     * Returns true if this mongod can accept writes to the database 'dbName'. Unless it is the
     * "local" database, this will only return true if this mongod is a primary (or a standalone).
     */
    bool _canAcceptWrites(OperationContext* opCtx, const DatabaseName& dbName);

    /*
     * Used by 'executeCommandOnPrimary'. Runs the command 'cmdObj' against the database 'dbName'
     * locally. Then asserts that command status using the 'uassertCmdStatusFn' callback. Returns
     * the command response.
     */
    BSONObj _executeCommandOnPrimaryLocal(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const BSONObj& cmdObj,
        const std::function<void(const BSONObj&)>& uassertCmdStatusFn);

    /*
     * Used by 'executeCommandOnPrimary'. Runs the command 'cmdObj' against the database 'dbName' on
     * the (remote) primary. Then asserts that the command status using the given
     * 'uassertCmdStatusFn' callback. Throws a PrimarySteppedDown error if no primary is found.
     * Returns the command response.
     */
    BSONObj _executeCommandOnPrimaryRemote(
        OperationContext* opCtx,
        const DatabaseName& dbName,
        const BSONObj& cmdObj,
        const std::function<void(const BSONObj&)>& uassertCmdStatusFn);
};

}  // namespace analyze_shard_key
}  // namespace mongo
