/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/s/sharding_ddl_coordinator.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/future_util.h"

namespace mongo {
namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc) {
    return ShardingDDLCoordinatorMetadata::parse(
        IDLParserErrorContext("ShardingDDLCoordinatorMetadata"), coorDoc);
}

ShardingDDLCoordinator::ShardingDDLCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& coorDoc)
    : _service(service),
      _coorMetadata(extractShardingDDLCoordinatorMetadata(coorDoc)),
      _recoveredFromDisk(_coorMetadata.getRecoveredFromDisk()) {}

ShardingDDLCoordinator::~ShardingDDLCoordinator() {
    invariant(_constructionCompletionPromise.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

bool ShardingDDLCoordinator::_removeDocument(OperationContext* opCtx) {
    DBDirectClient dbClient(opCtx);
    auto commandResponse = dbClient.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kShardingDDLCoordinatorsNamespace);

        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(ShardingDDLCoordinatorMetadata::kIdFieldName
                            << _coorMetadata.getId().toBSON()));
            entry.setMulti(true);
            return entry;
        }()});

        return deleteOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    BatchedCommandResponse batchedResponse;
    std::string unusedErrmsg;
    batchedResponse.parseBSON(commandReply, &unusedErrmsg);

    WriteConcernResult ignoreResult;
    const WriteConcernOptions majorityWriteConcern{
        WriteConcernOptions::kMajority,
        WriteConcernOptions::SyncMode::UNSET,
        WriteConcernOptions::kWriteConcernTimeoutSharding};
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, majorityWriteConcern, &ignoreResult));

    return batchedResponse.getN() > 0;
}

void ShardingDDLCoordinator::interrupt(Status status) {
    LOGV2_DEBUG(5390535,
                1,
                "Sharding DDL Coordinator received an interrupt",
                "coordinatorId"_attr = _coorMetadata.getId(),
                "reason"_attr = redact(status));

    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_constructionCompletionPromise.getFuture().isReady()) {
        _constructionCompletionPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

SemiFuture<void> ShardingDDLCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                             const CancellationToken& token) noexcept {

    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            const auto coorName =
                DDLCoordinatorType_serializer(_coorMetadata.getId().getOperationType());

            auto distLockManager = DistLockManager::get(opCtx);
            auto dbDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, nss().db(), coorName, DistLockManager::kDefaultLockTimeout));
            _scopedLocks.emplace(dbDistLock.moveToAnotherThread());

            if (!nss().isConfigDB() && !_coorMetadata.getRecoveredFromDisk()) {
                invariant(_coorMetadata.getDatabaseVersion());

                OperationShardingState::get(opCtx).initializeClientRoutingVersions(
                    nss(), boost::none /* ChunkVersion */, _coorMetadata.getDatabaseVersion());
                // Check under the dbLock if this is still the primary shard for the database
                DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, nss().db());
            };

            if (!nss().coll().empty()) {
                auto collDistLock = uassertStatusOK(distLockManager->lock(
                    opCtx, nss().ns(), coorName, DistLockManager::kDefaultLockTimeout));
                _scopedLocks.emplace(collDistLock.moveToAnotherThread());
            }

            for (auto& lock : _acquireAdditionalLocks(opCtx)) {
                _scopedLocks.emplace(lock.moveToAnotherThread());
            }

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.emplaceValue();
            }
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            static constexpr auto& errorMsg =
                "Failed to complete construction of sharding DDL coordinator";
            LOGV2_ERROR(5390530,
                        errorMsg,
                        "coordinatorId"_attr = _coorMetadata.getId(),
                        "reason"_attr = redact(status));

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.setError(status);
            }

            return status;
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            return AsyncTry([this, executor, token] { return _runImpl(executor, token); })
                .until([this, token](Status status) {
                    // Retry until either:
                    //  - The coordinator succeed
                    //  - The coordiantor failed with non-retryable error
                    //  - The node is stepping/shutting down
                    //
                    //  If the token is not cancelled we retry stepdown errors because it could have
                    //  been generated by a remote node.
                    if (!status.isOK() &&
                        (status.isA<ErrorCategory::NotPrimaryError>() ||
                         status.isA<ErrorCategory::ShutdownError>()) &&
                        !token.isCanceled()) {
                        LOGV2_DEBUG(5656000,
                                    1,
                                    "Re-executing sharding DDL coordinator",
                                    "coordinatorId"_attr = _coorMetadata.getId(),
                                    "reason"_attr = redact(status));
                        return false;
                    }
                    return true;
                })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, CancellationToken::uncancelable());
        })
        .onCompletion([this, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            auto completionStatus = status;

            // Release the coordinator only if we are not stepping down
            if (!status.isA<ErrorCategory::NotPrimaryError>() &&
                !status.isA<ErrorCategory::ShutdownError>()) {

                try {
                    LOGV2(5565601,
                          "Releasing sharding DDL coordinator",
                          "coordinatorId"_attr = _coorMetadata.getId());

                    const auto docWasRemoved = _removeDocument(opCtx);

                    if (!docWasRemoved) {
                        _service->releaseInstance(BSON(ShardingDDLCoordinatorMetadata::kIdFieldName
                                                       << _coorMetadata.getId().toBSON()),
                                                  status);
                    }
                } catch (DBException& ex) {
                    static constexpr auto errMsg = "Failed to release sharding DDL coordinator";
                    LOGV2_WARNING(5565605,
                                  errMsg,
                                  "coordinatorId"_attr = _coorMetadata.getId(),
                                  "error"_attr = redact(ex));
                    completionStatus = ex.toStatus(errMsg);
                }
            }

            while (!_scopedLocks.empty()) {
                _scopedLocks.top().assignNewOpCtx(opCtx);
                _scopedLocks.pop();
            }

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.setFrom(completionStatus);
            }

            return completionStatus;
        })
        .semi();
}

ShardingDDLCoordinator_NORESILIENT::ShardingDDLCoordinator_NORESILIENT(OperationContext* opCtx,
                                                                       const NamespaceString& ns)
    : _nss(ns), _forwardableOpMetadata(opCtx) {}

SemiFuture<void> ShardingDDLCoordinator_NORESILIENT::run(OperationContext* opCtx) {
    if (!_nss.isConfigDB()) {
        // Check that the operation context has a database version for this namespace
        const auto clientDbVersion = OperationShardingState::get(opCtx).getDbVersion(_nss.db());
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Request sent without attaching database version",
                clientDbVersion);

        // Checks that this is the primary shard for the namespace's db
        DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, _nss.db());
    }
    return runImpl(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor());
}

}  // namespace mongo
