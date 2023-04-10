/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/task_executor.h"

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
