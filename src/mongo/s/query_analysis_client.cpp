// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query_analysis_client.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/intent_guard.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/replication_state_transition_lock_guard.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace analyze_shard_key {

namespace {

MONGO_FAIL_POINT_DEFINE(queryAnalysisClientHangExecutingCommandLocally);
MONGO_FAIL_POINT_DEFINE(queryAnalysisClientHangExecutingCommandRemotely);

const auto getQueryAnalysisClient = ServiceContext::declareDecoration<QueryAnalysisClient>();
const auto getTaskExecutor =
    ServiceContext::declareDecoration<std::shared_ptr<executor::TaskExecutor>>();

const int kMaxRetriesOnRetryableErrors = 5;

// The write concern for writes done as part of query sampling or analyzing a shard key.
const Seconds writeConcernTimeout{60};
const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, writeConcernTimeout};

}  // namespace

QueryAnalysisClient& QueryAnalysisClient::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

QueryAnalysisClient& QueryAnalysisClient::get(ServiceContext* serviceContext) {
    return getQueryAnalysisClient(serviceContext);
}

void QueryAnalysisClient::setTaskExecutor(ServiceContext* service,
                                          std::shared_ptr<executor::TaskExecutor> executor) {
    getTaskExecutor(service) = std::move(executor);
}

bool QueryAnalysisClient::_canAcceptWrites(OperationContext* opCtx, const DatabaseName& dbName) {
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        return rss::consensus::IntentRegistry::get(opCtx->getServiceContext())
            .canDeclareIntent(rss::consensus::IntentRegistry::Intent::Write, opCtx);
    }
    repl::ReplicationStateTransitionLockGuard rstl(opCtx, MODE_IX);
    return mongo::repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(opCtx,
                                                                                       dbName);
}

BSONObj QueryAnalysisClient::_executeCommandOnPrimaryLocal(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    DBDirectClient client(opCtx);
    BSONObj resObj;
    client.runCommand(dbName, cmdObj, resObj);
    uassertCmdStatusFn(resObj);
    return resObj;
}

BSONObj QueryAnalysisClient::_executeCommandOnPrimaryRemote(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    auto hostAndPort = repl::ReplicationCoordinator::get(opCtx)->getCurrentPrimaryHostAndPort();

    if (hostAndPort.empty()) {
        uasserted(ErrorCodes::PrimarySteppedDown, "No primary exists currently");
    }

    auto executor = getTaskExecutor(opCtx->getServiceContext());
    invariant(executor, "Failed to run command since the executor has not been initialized");

    executor::RemoteCommandRequest request(std::move(hostAndPort), dbName, cmdObj, opCtx);
    auto [promise, future] = makePromiseFuture<executor::TaskExecutor::RemoteCommandCallbackArgs>();
    auto promisePtr = std::make_shared<Promise<executor::TaskExecutor::RemoteCommandCallbackArgs>>(
        std::move(promise));

    auto scheduleResult = executor->scheduleRemoteCommand(
        std::move(request), [promisePtr](const auto& args) { promisePtr->emplaceValue(args); });
    if (!scheduleResult.isOK()) {
        // Since the command failed to be scheduled, the callback above did not and will not run.
        // Thus, it is safe to fulfill the promise here without worrying about synchronizing access
        // with the executor's thread.
        promisePtr->setError(scheduleResult.getStatus());
    }

    auto rcr = uassertStatusOK(future.getNoThrow(opCtx));
    uassertStatusOK(rcr.response.status);
    uassertCmdStatusFn(rcr.response.data);
    return rcr.response.data;
}

BSONObj QueryAnalysisClient::executeCommandOnPrimary(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    // The query analysis client is used for analyze shard key. This is not mission critical, and
    // therefore should always be deprioritized when the server is overloaded.
    ScopedAdmissionPriority<ExecutionAdmissionContext> skipAdmissionControl(
        opCtx, AdmissionContext::Priority::kLow);

    auto numRetries = 0;

    while (true) {
        try {
            if (_canAcceptWrites(opCtx, dbName)) {
                // There is a window here where this mongod may step down after check above. In this
                // case, a NotWritablePrimary error would be thrown. However, this is preferable to
                // running the command while holding locks.
                queryAnalysisClientHangExecutingCommandLocally.pauseWhileSet(opCtx);
                return _executeCommandOnPrimaryLocal(opCtx, dbName, cmdObj, uassertCmdStatusFn);
            }

            queryAnalysisClientHangExecutingCommandRemotely.pauseWhileSet(opCtx);
            return _executeCommandOnPrimaryRemote(opCtx, dbName, cmdObj, uassertCmdStatusFn);
        } catch (DBException& ex) {
            if (ErrorCodes::isRetriableError(ex) && numRetries < kMaxRetriesOnRetryableErrors) {
                numRetries++;
                continue;
            }
            throw;
        }
    }

    MONGO_UNREACHABLE;
}

void QueryAnalysisClient::insert(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const std::vector<BSONObj>& docs,
                                 const std::function<void(const BSONObj&)>& uassertCmdStatusFn) {
    write_ops::InsertCommandRequest insertCmd(nss);
    insertCmd.setDocuments(docs);
    insertCmd.setWriteCommandRequestBase([&] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        wcb.setBypassDocumentValidation(false);
        return wcb;
    }());
    insertCmd.setWriteConcern(kMajorityWriteConcern);
    auto insertCmdObj = insertCmd.toBSON();

    executeCommandOnPrimary(opCtx, nss.dbName(), std::move(insertCmdObj), uassertCmdStatusFn);
}

}  // namespace analyze_shard_key
}  // namespace mongo
