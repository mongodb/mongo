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

#include "mongo/db/s/move_primary/move_primary_recipient_service.h"

#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/move_primary/move_primary_util.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/str.h"
#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/move_primary/move_primary_oplog_applier_progress_gen.h"
#include "mongo/db/s/move_primary/move_primary_server_parameters_gen.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/grid.h"
#include "mongo/s/move_primary/move_primary_feature_flag_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {

namespace {

bool wouldSurpassBatchLimit(int currSize, int nextDocSize) {
    return (currSize + nextDocSize) >= gMovePrimaryClonerMetadataCollMaxBatchSizeBytes.load();
}

}  // anonymous namespace

MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseBeforeRunning);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterInsertingStateDoc);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterCloningState);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterApplyingState);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterBlockingState);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterPreparedState);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseBeforeDeletingStateDoc);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseBeforeCompletion);

MovePrimaryRecipientService::MovePrimaryRecipientService(ServiceContext* serviceContext)
    : repl::PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}

StringData MovePrimaryRecipientService::getServiceName() const {
    return kMovePrimaryRecipientServiceName;
}

ThreadPool::Limits MovePrimaryRecipientService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimits;
    threadPoolLimits.maxThreads = gMovePrimaryRecipientServiceMaxThreadCount;
    return threadPoolLimits;
}

/**
 * ShardingDDLCoordinator will serialize each movePrimary on same namespace. This is added for
 * safety and testing.
 */
void MovePrimaryRecipientService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto recipientDoc = MovePrimaryRecipientDocument::parse(
        IDLParserContext("MovePrimaryRecipientService::checkIfConflictsWithOtherInstances"),
        std::move(initialState));

    for (const auto instance : existingInstances) {
        auto typedInstance = checked_cast<const MovePrimaryRecipient*>(instance);
        auto dbName = typedInstance->getDatabaseName();
        uassert(ErrorCodes::MovePrimaryInProgress,
                str::stream() << "Only one movePrimary operation is allowed on a given database",
                dbName != recipientDoc.getDatabaseName());
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> MovePrimaryRecipientService::constructInstance(
    BSONObj initialState) {
    auto recipientStateDoc = MovePrimaryRecipientDocument::parse(
        IDLParserContext("MovePrimaryRecipientService::constructInstance"),
        std::move(initialState));

    return std::make_shared<MovePrimaryRecipientService::MovePrimaryRecipient>(
        this,
        recipientStateDoc,
        std::make_shared<MovePrimaryRecipientExternalStateImpl>(),
        _serviceContext,
        std::make_unique<Cloner>());
}

MovePrimaryRecipientService::MovePrimaryRecipient::MovePrimaryRecipient(
    const MovePrimaryRecipientService* service,
    MovePrimaryRecipientDocument recipientDoc,
    std::shared_ptr<MovePrimaryRecipientExternalState> externalState,
    ServiceContext* serviceContext,
    std::unique_ptr<Cloner> cloner)
    : _recipientService(service),
      _metadata(recipientDoc.getMetadata()),
      _movePrimaryRecipientExternalState(externalState),
      _serviceContext(serviceContext),
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "MovePrimaryRecipientServiceCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())),
      _startApplyingDonorOpTime(recipientDoc.getStartApplyingDonorOpTime()),
      _criticalSectionReason(BSON("reason"
                                  << "Entering kPrepared state at MovePrimaryRecipientService"
                                  << "operationInfo" << _metadata.toBSON())),
      _resumedAfterFailover(recipientDoc.getState() > MovePrimaryRecipientStateEnum::kUnused),
      _state(recipientDoc.getState()),
      _cloner(std::move(cloner)){};

void MovePrimaryRecipientService::MovePrimaryRecipient::checkIfOptionsConflict(
    const BSONObj& stateDoc) const {
    auto recipientDoc = MovePrimaryRecipientDocument::parse(
        IDLParserContext("movePrimaryCheckIfOptionsConflict"), stateDoc);
    uassert(ErrorCodes::MovePrimaryInProgress,
            str::stream() << "Found an existing movePrimary operation in progress",
            recipientDoc.getDatabaseName() == getDatabaseName() &&
                recipientDoc.getFromShardName() == _metadata.getFromShardName());
}

std::vector<AsyncRequestsSender::Response>
MovePrimaryRecipientExternalStateImpl::sendCommandToShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    return sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, dbName, command, shardIds, executor);
}

SemiFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepDownToken) noexcept {

    movePrimaryRecipientPauseBeforeRunning.pauseWhileSet();

    // We would like to abort in all cases where there is a failover and we have not yet reached
    // kPrepared state to maintain correctness of movePrimary operation across upgrades/downgrades
    // in binary versions with feature parity in online movePrimary implementation. The offline
    // cloner is not resumable after failovers.
    auto shouldAbort = [&] {
        if (!_useOnlineCloner()) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_resumedAfterFailover && _canAbort(lg)) {
                return true;
            }
        }
        return false;
    }();

    // Synchronize abort() called from a different thread before _ctHolder is initialized.
    auto abortCalled = [&] {
        stdx::lock_guard<Latch> lg(_mutex);
        _ctHolder = std::make_unique<RecipientCancellationTokenHolder>(std::move(stepDownToken));
        return _abortCalled;
    }();

    if (shouldAbort) {
        _internalAbort();
    }

    if (abortCalled) {
        abort();
    }

    _markKilledExecutor->startup();
    _retryingCancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(), _markKilledExecutor);

    return ExecutorFuture(**executor)
        .then([this, executor] { return _transitionToInitializingState(executor); })
        .then([this] {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                move_primary_util::ensureFulfilledPromise(
                    lg, _recipientDocDurablePromise, Status::OK());
            }
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();
            movePrimaryRecipientPauseAfterInsertingStateDoc.pauseWhileSetAndNotCanceled(
                opCtx, _ctHolder->getStepdownToken());
        })
        .then([this, executor] { return _initializeForCloningState(executor); })
        .then([this, executor] { return _transitionToCloningStateAndClone(executor); })
        .then([this, executor] { return _transitionToApplyingState(executor); })
        .then([this, executor] {
            return _transitionToBlockingStateAndAcquireCriticalSection(executor);
        })
        .then([this, executor] { return _transitionToPreparedState(executor); })
        .then([this, executor] {
            auto forgetMigrationFuture = ([&] {
                stdx::lock_guard<Latch> lg(_mutex);
                return _forgetMigrationPromise.getFuture();
            })();

            return future_util::withCancellation(std::move(forgetMigrationFuture),
                                                 _ctHolder->getAbortToken())
                .thenRunOn(**executor)
                .then([this, executor] {
                    return _transitionToDoneStateAndFinishMovePrimaryOp(executor);
                });
        })
        .onError([this, executor](Status status) {
            if (_ctHolder->isAborted()) {
                _retryingCancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(),
                                                        _markKilledExecutor);
                LOGV2(7307002,
                      "MovePrimaryRecipient aborting movePrimary operation",
                      "metadata"_attr = _metadata,
                      "error"_attr = status);
                return _transitionToAbortedStateAndCleanupOrphanedData(executor).then(
                    [this, executor] {
                        return _transitionToDoneStateAndFinishMovePrimaryOp(executor);
                    });
            }
            return ExecutorFuture<void>(**executor, status);
        })
        .thenRunOn(_recipientService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](Status status) {
            if (!status.isOK()) {
                LOGV2(7307003,
                      "MovePrimaryRecipient encountered error during movePrimary operation",
                      "metadata"_attr = _metadata,
                      "error"_attr = status);
            }
            _ensureUnfulfilledPromisesError(status);
            movePrimaryRecipientPauseBeforeCompletion.pauseWhileSet();
        })
        .semi();
}

void MovePrimaryRecipientService::MovePrimaryRecipient::abort() {
    stdx::lock_guard<Latch> lg(_mutex);
    _abortCalled = true;
    if (_ctHolder) {
        LOGV2(7270000,
              "MovePrimaryRecipient received abort of movePrimary operation",
              "metadata"_attr = _metadata);
        _ctHolder->abort();
    }
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_internalAbort() {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        invariant(_state < MovePrimaryRecipientStateEnum::kPrepared);
    }
    abort();
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_cloneDataFromDonor(
    OperationContext* opCtx) {
    // Enable write blocking bypass to allow cloning of catalog data even if writes are disallowed.
    WriteBlockBypass::get(opCtx).set(true);
    DisableDocumentValidation disableValidation(opCtx);
    std::set<std::string> clonedCollections;
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto fromShard =
        uassertStatusOK(shardRegistry->getShard(opCtx, _metadata.getFromShardName().toString()));
    uassertStatusOK(_cloner->copyDb(opCtx,
                                    _metadata.getDatabaseName().toString(),
                                    fromShard->getConnString().toString(),
                                    _shardedColls,
                                    &clonedCollections));
}

ExecutorFuture<void>
MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToCloningStateAndClone(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kCloning)) {
                return;
            }
            auto opCtx = factory.makeOperationContext(Client::getCurrent());
            _updateRecipientDocument(
                opCtx.get(),
                MovePrimaryRecipientDocument::kStateFieldName,
                MovePrimaryRecipientState_serializer(MovePrimaryRecipientStateEnum::kCloning));
            _transitionStateMachine(MovePrimaryRecipientStateEnum::kCloning);
            // TODO SERVER-75872: Refactor this logic after integrating online cloner.
            _cloneDataFromDonor(opCtx.get());
        })
        .onTransientError([](const Status& status) {
            LOGV2(7307000,
                  "MovePrimaryRecipient encountered transient error in _transitionToCloningState",
                  "error"_attr = redact(status));
        })
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(
                7306911,
                "MovePrimaryRecipient encountered unrecoverable error in _transitionToCloningState",
                "_metadata"_attr = _metadata,
                "_error"_attr = status);
            _internalAbort();
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            return _waitForMajority(executor).then([this, status] {
                {
                    stdx::lock_guard<Latch> lg(_mutex);
                    move_primary_util::ensureFulfilledPromise(lg, _dataClonePromise, status);
                }
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                movePrimaryRecipientPauseAfterCloningState.pauseWhileSetAndNotCanceled(
                    opCtx, _ctHolder->getStepdownToken());
            });
        });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToApplyingState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kApplying)) {
                return ExecutorFuture<void>(**executor);
            }
            auto opCtx = factory.makeOperationContext(Client::getCurrent());
            _updateRecipientDocument(
                opCtx.get(),
                MovePrimaryRecipientDocument::kStateFieldName,
                MovePrimaryRecipientState_serializer(MovePrimaryRecipientStateEnum::kApplying));
            _transitionStateMachine(MovePrimaryRecipientStateEnum::kApplying);
            return ExecutorFuture<void>(**executor);
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(7306912,
                        "MovePrimaryRecipient encountered unrecoverable error in "
                        "_transitionToApplyingState",
                        "_metadata"_attr = _metadata,
                        "_error"_attr = status);
            _internalAbort();
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            return _waitForMajority(executor).then([this] {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                movePrimaryRecipientPauseAfterApplyingState.pauseWhileSetAndNotCanceled(
                    opCtx, _ctHolder->getStepdownToken());
            });
        });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::
    _transitionToBlockingStateAndAcquireCriticalSection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            return ExecutorFuture<void>(**executor)
                .then([this, factory, executor] {
                    if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kBlocking)) {
                        return;
                    }
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    _updateRecipientDocument(opCtx.get(),
                                             MovePrimaryRecipientDocument::kStateFieldName,
                                             MovePrimaryRecipientState_serializer(
                                                 MovePrimaryRecipientStateEnum::kBlocking));
                    _transitionStateMachine(MovePrimaryRecipientStateEnum::kBlocking);
                })
                .then([this, factory, executor] {
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    ShardingRecoveryService::get(opCtx.get())
                        ->acquireRecoverableCriticalSectionBlockWrites(
                            opCtx.get(),
                            getDatabaseName(),
                            _criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                    ShardingRecoveryService::get(opCtx.get())
                        ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx.get(),
                            getDatabaseName(),
                            _criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                });
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(7306900,
                        "MovePrimaryRecipient encountered unrecoverable error in "
                        "_transitionToBlockingStateAndAcquireCriticalSection",
                        "_metadata"_attr = _metadata,
                        "_error"_attr = status);
            _internalAbort();
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            return _waitForMajority(executor).then([this] {
                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                movePrimaryRecipientPauseAfterBlockingState.pauseWhileSetAndNotCanceled(
                    opCtx, _ctHolder->getStepdownToken());
            });
        });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToPreparedState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kPrepared)) {
                return;
            }
            auto opCtx = factory.makeOperationContext(Client::getCurrent());
            _updateRecipientDocument(
                opCtx.get(),
                MovePrimaryRecipientDocument::kStateFieldName,
                MovePrimaryRecipientState_serializer(MovePrimaryRecipientStateEnum::kPrepared));
            _transitionStateMachine(MovePrimaryRecipientStateEnum::kPrepared);
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(7306910,
                        "MovePrimaryRecipient encountered unrecoverable error in "
                        "_transitionToPreparedState",
                        "_metadata"_attr = _metadata,
                        "_error"_attr = status);
            _internalAbort();
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            return _waitForMajority(executor).then([this, status] {
                {
                    stdx::lock_guard<Latch> lg(_mutex);
                    move_primary_util::ensureFulfilledPromise(lg, _preparedPromise, status);
                }

                auto opCtxHolder = cc().makeOperationContext();
                auto opCtx = opCtxHolder.get();
                movePrimaryRecipientPauseAfterPreparedState.pauseWhileSetAndNotCanceled(
                    opCtx, _ctHolder->getStepdownToken());
            });
        });
}

ExecutorFuture<void>
MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToAbortedStateAndCleanupOrphanedData(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            return ExecutorFuture(**executor)
                .then([this, factory] {
                    if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kAborted)) {
                        return;
                    }
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    _updateRecipientDocument(opCtx.get(),
                                             MovePrimaryRecipientDocument::kStateFieldName,
                                             MovePrimaryRecipientState_serializer(
                                                 MovePrimaryRecipientStateEnum::kAborted));
                    _transitionStateMachine(MovePrimaryRecipientStateEnum::kAborted);
                })
                .then([this, factory, executor] {
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    _cleanUpOrphanedDataOnRecipient(opCtx.get());
                });
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        .onCompletion([this, self = shared_from_this(), executor](Status status) {
            return _waitForMajority(executor).then([this, executor, status] {
                if (!status.isOK()) {
                    LOGV2(7307001,
                          "MovePrimaryRecipient encountered error in "
                          "_transitionToAbortedStateAndCleanupOrphanedData",
                          "error"_attr = status);
                }
                // Intentionally return OK status after logging as the abort is best effort.
                return ExecutorFuture<void>(**executor, Status::OK());
            });
        });
}

ExecutorFuture<void>
MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToDoneStateAndFinishMovePrimaryOp(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    _retryingCancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            return ExecutorFuture<void>(**executor)
                .then([this, factory, executor] {
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kDone)) {
                        return;
                    }

                    _updateRecipientDocument(
                        opCtx.get(),
                        MovePrimaryRecipientDocument::kStateFieldName,
                        MovePrimaryRecipientState_serializer(MovePrimaryRecipientStateEnum::kDone));
                    _transitionStateMachine(MovePrimaryRecipientStateEnum::kDone);
                })
                .then([this, factory, executor] {
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    _cleanUpOperationMetadata(opCtx.get(), executor);
                })
                .then([this, factory, executor] {
                    auto opCtx = factory.makeOperationContext(Client::getCurrent());
                    _clearDatabaseMetadata(opCtx.get());

                    ShardingRecoveryService::get(opCtx.get())
                        ->releaseRecoverableCriticalSection(
                            opCtx.get(),
                            getDatabaseName(),
                            _criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                    movePrimaryRecipientPauseBeforeDeletingStateDoc.pauseWhileSetAndNotCanceled(
                        opCtx.get(), _ctHolder->getStepdownToken());
                    _removeRecipientDocument(opCtx.get());
                });
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {
            LOGV2(7306901,
                  "MovePrimaryRecipient received unrecoverable error in "
                  "_transitionToDoneStateAndFinishMovePrimaryOp",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        .onCompletion([this, executor](Status status) {
            if (_ctHolder->isAborted()) {
                // Override status code to aborted after logging the original error
                status = {ErrorCodes::MovePrimaryAborted, "movePrimary operation aborted"};
            }
            return _waitForMajority(executor).then([this, executor, status] {
                if (status.isOK()) {
                    stdx::lock_guard<Latch> lg(_mutex);
                    move_primary_util::ensureFulfilledPromise(lg, _completionPromise, Status::OK());
                }
                return ExecutorFuture<void>(**executor, status);
            });
        });
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_clearDatabaseMetadata(
    OperationContext* opCtx) {
    auto dbName = getDatabaseName().dbName();
    AutoGetDb autoDb(opCtx, dbName, MODE_IX);
    auto scopedDss = DatabaseShardingState::assertDbLockedAndAcquireExclusive(opCtx, dbName);
    scopedDss->clearDbInfo(opCtx);
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_createMetadataCollection(
    OperationContext* opCtx) {
    resharding::data_copy::ensureCollectionExists(opCtx, _getCollectionsToCloneNSS(), {});
}

std::vector<NamespaceString>
MovePrimaryRecipientService::MovePrimaryRecipient::_getUnshardedCollections(
    OperationContext* opCtx) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto fromShard =
        uassertStatusOK(shardRegistry->getShard(opCtx, _metadata.getFromShardName().toString()));

    auto collectionsToCloneWithStatus = _cloner->getListOfCollections(
        opCtx, getDatabaseName().dbName().toString(), fromShard->getConnString().toString());
    auto collectionsToClone = uassertStatusOK(collectionsToCloneWithStatus);

    const auto allCollections = [&] {
        std::vector<NamespaceString> colls;
        for (const auto& collInfo : collectionsToClone) {
            std::string collName;
            uassertStatusOK(bsonExtractStringField(collInfo, "name", &collName));
            const NamespaceString nss(getDatabaseName().toString(), collName);
            if (!nss.isSystem() ||
                nss.isLegalClientSystemNS(serverGlobalParams.featureCompatibility)) {
                colls.push_back(nss);
            }
        }
        std::sort(colls.begin(), colls.end());
        return colls;
    }();

    std::vector<NamespaceString> unshardedCollections;
    std::set_difference(allCollections.cbegin(),
                        allCollections.cend(),
                        _shardedColls.cbegin(),
                        _shardedColls.cend(),
                        std::back_inserter(unshardedCollections));

    return unshardedCollections;
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_persistCollectionsToClone(
    OperationContext* opCtx) {
    auto collsToClone = _getUnshardedCollections(opCtx);
    std::vector<InsertStatement> batch;
    int i = 0;
    int numBytes = 0;
    auto collectionsToCloneNSS = _getCollectionsToCloneNSS();
    for (const auto& coll : collsToClone) {
        auto doc = BSON("_id" << i << "nss" << coll.ns());
        ++i;
        if (wouldSurpassBatchLimit(numBytes, doc.objsize())) {
            resharding::data_copy::insertBatch(opCtx, collectionsToCloneNSS, batch);
            batch.clear();
            numBytes = 0;
        }
        batch.emplace_back(InsertStatement(doc));
        numBytes += doc.objsize();
    }
    if (!batch.empty())
        resharding::data_copy::insertBatch(opCtx, collectionsToCloneNSS, batch);
}

std::vector<NamespaceString>
MovePrimaryRecipientService::MovePrimaryRecipient::_getCollectionsToClone(
    OperationContext* opCtx) const {
    std::vector<NamespaceString> collsToClone;
    auto collectionsToCloneNSS = _getCollectionsToCloneNSS();
    AutoGetCollection autoColl(opCtx, collectionsToCloneNSS, MODE_IS);
    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection '" << collectionsToCloneNSS.toStringForErrorMsg()
                          << "' did not already exist",
            autoColl);
    auto cursor = autoColl->getCursor(opCtx);
    while (auto record = cursor->next()) {
        BSONObj obj = record->data.releaseToBson();
        NamespaceString ns(
            NamespaceStringUtil::deserialize(boost::none, obj.getStringField("nss")));
        collsToClone.emplace_back(ns);
    }
    return collsToClone;
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_cleanUpOrphanedDataOnRecipient(
    OperationContext* opCtx) {
    // Drop all the collections which might have been cloned on the recipient.
    auto colls = _getCollectionsToClone(opCtx);
    for (const auto& coll : colls) {
        resharding::data_copy::ensureCollectionDropped(opCtx, coll);
    }
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_cleanUpOperationMetadata(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {

    // Drop collectionsToClone NSS.
    resharding::data_copy::ensureCollectionDropped(opCtx, _getCollectionsToCloneNSS());

    // Drop temp oplog buffer.
    resharding::data_copy::ensureCollectionDropped(
        opCtx, NamespaceString::makeMovePrimaryOplogBufferNSS(getMigrationId()));

    // Drop oplog applier progress document.
    PersistentTaskStore<MovePrimaryOplogApplierProgress> store(
        NamespaceString::kMovePrimaryApplierProgressNamespace);
    store.remove(opCtx,
                 BSON(MovePrimaryRecipientDocument::kMigrationIdFieldName << getMigrationId()),
                 WriteConcerns::kLocalWriteConcern);

    // Drop all collections with the prefix movePrimaryRecipient.<migrationId>.willBeDeleted.
    auto catalogClient = CollectionCatalog::get(opCtx);
    std::vector<NamespaceString> colls;
    {
        AutoGetDb autoDb(opCtx, DatabaseName::kConfig, MODE_S);
        colls = catalogClient->getAllCollectionNamesFromDb(opCtx, DatabaseName::kConfig);
    }
    const auto& nssPrefix =
        NamespaceString::makeMovePrimaryTempCollectionsPrefix(getMigrationId()).toString();
    for (const auto& nss : colls) {
        if (!nss.ns().startsWith(nssPrefix)) {
            continue;
        }
        LOGV2(7621600, "MovePrimaryRecipient dropping collection", "ns"_attr = nss);
        resharding::data_copy::ensureCollectionDropped(opCtx, nss);
    }
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_removeRecipientDocument(
    OperationContext* opCtx) {
    // Delete state document.
    PersistentTaskStore<MovePrimaryRecipientDocument> store(
        NamespaceString::kMovePrimaryRecipientNamespace);
    store.remove(opCtx,
                 BSON(MovePrimaryRecipientDocument::kIdFieldName << getMigrationId()),
                 WriteConcerns::kLocalWriteConcern);
    LOGV2(7306902,
          "MovePrimaryRecipient removed recipient document for movePrimary operation",
          "metadata"_attr = _metadata);
}

SharedSemiFuture<void>
MovePrimaryRecipientService::MovePrimaryRecipient::onReceiveForgetMigration() {
    stdx::lock_guard<Latch> lg(_mutex);
    LOGV2(7270001,
          "MovePrimaryRecipient received forgetMigration for movePrimary operation",
          "metadata"_attr = _metadata);
    move_primary_util::ensureFulfilledPromise(lg, _forgetMigrationPromise, Status::OK());
    return _completionPromise.getFuture();
}

SharedSemiFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::onReceiveSyncData(
    Timestamp blockTimestamp) {
    return _preparedPromise.getFuture();
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_ensureUnfulfilledPromisesError(
    Status status) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_recipientDocDurablePromise.getFuture().isReady()) {
        _recipientDocDurablePromise.setError(status);
    }
    if (!_dataClonePromise.getFuture().isReady()) {
        _dataClonePromise.setError(status);
    }
    if (!_preparedPromise.getFuture().isReady()) {
        _preparedPromise.setError(status);
    }
    if (!_forgetMigrationPromise.getFuture().isReady()) {
        _forgetMigrationPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_transitionStateMachine(
    MovePrimaryRecipientStateEnum newState) {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(newState > _state);

    std::swap(_state, newState);
    LOGV2(7271201,
          "Transitioned MovePrimaryRecipient state",
          "oldState"_attr = MovePrimaryRecipientState_serializer(newState),
          "newState"_attr = MovePrimaryRecipientState_serializer(_state),
          "migrationId"_attr = getMigrationId(),
          "databaseName"_attr = getDatabaseName(),
          "fromShard"_attr = _metadata.getFromShardName());
}

ExecutorFuture<void>
MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToInitializingState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            if (_resumedAfterFailover) {
                return;
            }
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            MovePrimaryRecipientDocument recipientDoc;
            recipientDoc.setId(getMigrationId());
            recipientDoc.setMetadata(_metadata);
            recipientDoc.setState(MovePrimaryRecipientStateEnum::kInitializing);
            recipientDoc.setStartAt(_serviceContext->getPreciseClockSource()->now());

            PersistentTaskStore<MovePrimaryRecipientDocument> store(
                NamespaceString::kMovePrimaryRecipientNamespace);
            store.add(opCtx, recipientDoc, WriteConcerns::kLocalWriteConcern);

            _transitionStateMachine(MovePrimaryRecipientStateEnum::kInitializing);
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([this](const Status& status) {
            LOGV2(7306800,
                  "MovePrimaryRecipient received unrecoverable error in "
                  "_transitionToInitializingState",
                  "error"_attr = status);
            _internalAbort();
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            return _waitForMajority(executor).then([this, status] {
                if (status.isOK()) {
                    LOGV2(7306903,
                          "MovePrimaryRecipient persisted state doc",
                          "metadata"_attr = _metadata);
                }
            });
        });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_initializeForCloningState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_checkInvalidStateTransition(MovePrimaryRecipientStateEnum::kCloning)) {
        return ExecutorFuture(**executor);
    }
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            auto opCtx = factory.makeOperationContext(Client::getCurrent());
            _shardedColls = _getShardedCollectionsFromConfigSvr(opCtx.get());
            _startApplyingDonorOpTime = _startApplyingDonorOpTime
                ? _startApplyingDonorOpTime
                : _getStartApplyingDonorOpTime(opCtx.get(), executor);
            _updateRecipientDocument(
                opCtx.get(),
                MovePrimaryRecipientDocument::kStartApplyingDonorOpTimeFieldName,
                _startApplyingDonorOpTime.get().toBSON());
            _createMetadataCollection(opCtx.get());
            _persistCollectionsToClone(opCtx.get());
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([this](const Status& status) {
            LOGV2(7306801,
                  "Received unrecoverable error while initializing for cloning state",
                  "error"_attr = status);
            _internalAbort();
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken());
}

template <class T>
void MovePrimaryRecipientService::MovePrimaryRecipient::_updateRecipientDocument(
    OperationContext* opCtx, const StringData& fieldName, T value) {
    PersistentTaskStore<MovePrimaryRecipientDocument> store(
        NamespaceString::kMovePrimaryRecipientNamespace);

    BSONObjBuilder updateBuilder;
    {
        BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
        setBuilder.append(fieldName, value);
        setBuilder.doneFast();
    }

    store.update(opCtx,
                 BSON(MovePrimaryRecipientDocument::kIdFieldName << getMigrationId()),
                 updateBuilder.done(),
                 WriteConcerns::kLocalWriteConcern);
}

repl::OpTime MovePrimaryRecipientService::MovePrimaryRecipient::_getStartApplyingDonorOpTime(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto oplogOpTimeFields =
        BSON(repl::OplogEntry::kTimestampFieldName << 1 << repl::OplogEntry::kTermFieldName << 1);
    FindCommandRequest findCmd{NamespaceString::kRsOplogNamespace};
    findCmd.setSort(BSON("$natural" << -1));
    findCmd.setProjection(oplogOpTimeFields);
    findCmd.setReadConcern(
        repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern).toBSONInner());
    findCmd.setLimit(1);

    auto rawResp = _movePrimaryRecipientExternalState->sendCommandToShards(
        opCtx,
        DatabaseName::kLocal.db(),
        findCmd.toBSON({}),
        {ShardId(_metadata.getFromShardName().toString())},
        **executor);

    uassert(7356200,
            "MovePrimaryRecipient unable to find majority committed OpTime at donor",
            !rawResp.empty());
    auto swResp = uassertStatusOK(rawResp.front().swResponse);
    BSONObj cursorObj = swResp.data["cursor"].Obj();
    BSONObj firstBatchObj = cursorObj["firstBatch"].Obj();
    auto majorityOpTime =
        uassertStatusOK(repl::OpTime::parseFromOplogEntry(firstBatchObj[0].Obj()));
    return majorityOpTime;
}

std::vector<NamespaceString>
MovePrimaryRecipientService::MovePrimaryRecipient::_getShardedCollectionsFromConfigSvr(
    OperationContext* opCtx) const {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto shardedColls =
        catalogClient->getAllShardedCollectionsForDb(opCtx,
                                                     getDatabaseName().toString(),
                                                     repl::ReadConcernLevel::kMajorityReadConcern,
                                                     BSON("ns" << 1));
    return shardedColls;
}

bool MovePrimaryRecipientService::MovePrimaryRecipient::_checkInvalidStateTransition(
    MovePrimaryRecipientStateEnum newState) {
    stdx::lock_guard<Latch> lg(_mutex);
    return newState <= _state;
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_waitForMajority(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    return ExecutorFuture<void>(**executor).then([this] {
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        return WaitForMajorityService::get(_serviceContext)
            .waitUntilMajority(std::move(clientOpTime), _ctHolder->getStepdownToken());
    });
}

boost::optional<BSONObj> MovePrimaryRecipientService::MovePrimaryRecipient::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    return boost::none;
}

NamespaceString MovePrimaryRecipientService::MovePrimaryRecipient::getDatabaseName() const {
    return _metadata.getDatabaseName();
}

NamespaceString MovePrimaryRecipientService::MovePrimaryRecipient::_getCollectionsToCloneNSS()
    const {
    return NamespaceString::makeMovePrimaryCollectionsToCloneNSS(getMigrationId());
}

UUID MovePrimaryRecipientService::MovePrimaryRecipient::getMigrationId() const {
    return _metadata.getMigrationId();
}

bool MovePrimaryRecipientService::MovePrimaryRecipient::_canAbort(WithLock) const {
    return _state < MovePrimaryRecipientStateEnum::kPrepared;
}

bool MovePrimaryRecipientService::MovePrimaryRecipient::_useOnlineCloner() const {
    return false;
}

}  // namespace mongo
