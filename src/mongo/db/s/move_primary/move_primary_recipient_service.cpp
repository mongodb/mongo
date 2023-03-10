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

#include "mongo/db/s/move_primary/move_primary_util.h"
#include "mongo/util/str.h"
#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/move_primary/move_primary_server_parameters_gen.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {

MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterInsertingStateDoc);
MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterTransitionToCloningState);

namespace {

constexpr StringData kUninitialized = "uninitialized"_sd;
constexpr StringData kCloning = "cloning"_sd;
constexpr StringData kApplying = "applying"_sd;
constexpr StringData kBlocking = "blocking"_sd;
constexpr StringData kPrepared = "prepared"_sd;
constexpr StringData kAborted = "aborted"_sd;
constexpr StringData kDone = "done"_sd;

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

StringData MovePrimaryRecipientService::MovePrimaryRecipient::_parseRecipientState(
    MovePrimaryRecipientState value) {
    switch (value) {
        case MovePrimaryRecipientState::kUninitialized:
            return kUninitialized;
        case MovePrimaryRecipientState::kCloning:
            return kCloning;
        case MovePrimaryRecipientState::kApplying:
            return kApplying;
        case MovePrimaryRecipientState::kBlocking:
            return kBlocking;
        case MovePrimaryRecipientState::kPrepared:
            return kPrepared;
        case MovePrimaryRecipientState::kAborted:
            return kAborted;
        case MovePrimaryRecipientState::kDone:
            return kDone;
        default:
            MONGO_UNREACHABLE
    }
}

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
        _serviceContext);
}

MovePrimaryRecipientService::MovePrimaryRecipient::MovePrimaryRecipient(
    const MovePrimaryRecipientService* service,
    MovePrimaryRecipientDocument recipientDoc,
    std::shared_ptr<MovePrimaryRecipientExternalState> externalState,
    ServiceContext* serviceContext)
    : _recipientService(service),
      _metadata(recipientDoc.getMovePrimaryRecipientMetadata()),
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
      _state(recipientDoc.getState()){};


void MovePrimaryRecipientService::MovePrimaryRecipient::checkIfOptionsConflict(
    const BSONObj& stateDoc) const {
    auto recipientDoc = MovePrimaryRecipientDocument::parse(
        IDLParserContext("movePrimaryCheckIfOptionsConflict"), stateDoc);
    uassert(ErrorCodes::MovePrimaryInProgress,
            str::stream() << "Found an existing movePrimary operation in progress",
            recipientDoc.getDatabaseName() == _metadata.getDatabaseName() &&
                recipientDoc.getFromShard() == _metadata.getFromShard());
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
    const CancellationToken& token) noexcept {
    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
        LOGV2(7271200, "Aborting movePrimary as the recipient is upgrading or downgrading");
    }
    _markKilledExecutor->startup();
    _retryingCancelableOpCtxFactory.emplace(token, _markKilledExecutor);

    return ExecutorFuture(**executor)
        .then([this, token, executor] { return _persistRecipientDoc(executor, token); })
        .then([this, self = shared_from_this()] {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                move_primary_util::ensureFulfilledPromise(lg, _recipientDocDurablePromise);
            }
            movePrimaryRecipientPauseAfterInsertingStateDoc.pauseWhileSet();
        })
        .then([this, self = shared_from_this(), executor, token] {
            return _initializeForCloningState(executor, token);
        })
        .then([this, self = shared_from_this(), executor, token] {
            return _transitionToCloningState(executor, token);
        })
        .then([this, self = shared_from_this(), executor, token] {
            return _transitionToApplyingState(executor, token);
        })
        .semi();
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToCloningState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, token, executor](const auto& factory) {
            return ExecutorFuture(**executor).then([this, token, executor, factory] {
                auto opCtx = factory.makeOperationContext(Client::getCurrent());
                {
                    stdx::lock_guard<Latch> lg(_mutex);

                    if (_state > MovePrimaryRecipientState::kUninitialized) {
                        return;
                    }

                    _transitionStateMachine(lg, MovePrimaryRecipientState::kCloning);
                }
                _updateRecipientDocument(opCtx.get(),
                                         MovePrimaryRecipientDocument::kStateFieldName,
                                         MovePrimaryRecipientState::kCloning);
            });
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, token)
        .onCompletion([this, self = shared_from_this(), executor, token](Status status) {
            {
                stdx::lock_guard<Latch> lg(_mutex);
                move_primary_util::ensureFulfilledPromise(lg, _recipientDataClonePromise);
            }
            movePrimaryRecipientPauseAfterTransitionToCloningState.pauseWhileSet();
        });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_transitionToApplyingState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, token, executor](const auto& factory) {
            auto opCtx = factory.makeOperationContext(Client::getCurrent());
            {
                stdx::lock_guard<Latch> lg(_mutex);

                if (_state > MovePrimaryRecipientState::kCloning) {
                    return;
                }
                _transitionStateMachine(lg, MovePrimaryRecipientState::kApplying);
            }
            _updateRecipientDocument(opCtx.get(),
                                     MovePrimaryRecipientDocument::kStateFieldName,
                                     MovePrimaryRecipientState::kApplying);
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, token);
}

void MovePrimaryRecipientService::MovePrimaryRecipient::_transitionStateMachine(
    WithLock, MovePrimaryRecipientState newState) {
    // This can happen during a retry of AsyncTry loop.
    if (newState == _state) {
        return;
    }
    invariant(newState > _state);

    std::swap(_state, newState);
    LOGV2(7271201,
          "Transitioned movePrimary recipient state",
          "oldState"_attr = _parseRecipientState(newState),
          "newState"_attr = _parseRecipientState(_state),
          "migrationId"_attr = _metadata.getMigrationId(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "fromShard"_attr = _metadata.getFromShard());
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_persistRecipientDoc(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
    return ExecutorFuture(**executor).then([this, token, executor] {
        {
            stdx::lock_guard<Latch> lg(_mutex);

            if (_state > MovePrimaryRecipientState::kUninitialized) {
                return;
            }
        }
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

        MovePrimaryRecipientDocument recipientDoc;
        recipientDoc.setMovePrimaryRecipientMetadata(_metadata);
        recipientDoc.setId(_metadata.getMigrationId());
        recipientDoc.setState(MovePrimaryRecipientState::kCloning);
        recipientDoc.setStartAt(_serviceContext->getPreciseClockSource()->now());

        PersistentTaskStore<MovePrimaryRecipientDocument> store(
            NamespaceString::kMovePrimaryRecipientNamespace);
        store.add(opCtx, recipientDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
    });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_initializeForCloningState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {
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
        })
        .onTransientError([](const Status& status) {})
        .onUnrecoverableError([](const Status& status) {
            LOGV2(7306800,
                  "Received unrecoverable error while initializing for cloning state",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, token);
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

    store.update(
        opCtx,
        BSON(MovePrimaryRecipientDocument::kMigrationIdFieldName << _metadata.getMigrationId()),
        updateBuilder.done(),
        WriteConcerns::kMajorityWriteConcernNoTimeout);
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
        _metadata.getDatabaseName(),
        findCmd.toBSON({}),
        {ShardId(_metadata.getFromShard().toString())},
        **executor);

    uassert(7356200, "Unable to find majority committed OpTime at donor", !rawResp.empty());
    auto swResp = uassertStatusOK(rawResp.front().swResponse);
    auto majorityOpTime = uassertStatusOK(repl::OpTime::parseFromOplogEntry(swResp.data));
    return majorityOpTime;
}

std::vector<NamespaceString>
MovePrimaryRecipientService::MovePrimaryRecipient::_getShardedCollectionsFromConfigSvr(
    OperationContext* opCtx) const {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto shardedColls =
        catalogClient->getAllShardedCollectionsForDb(opCtx,
                                                     _metadata.getDatabaseName(),
                                                     repl::ReadConcernLevel::kMajorityReadConcern,
                                                     BSON("ns" << 1));
    return shardedColls;
}

void MovePrimaryRecipientService::MovePrimaryRecipient::interrupt(Status status) {}

boost::optional<BSONObj> MovePrimaryRecipientService::MovePrimaryRecipient::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    return boost::none;
}

StringData MovePrimaryRecipientService::MovePrimaryRecipient::getDatabaseName() const {
    return _metadata.getDatabaseName();
}

UUID MovePrimaryRecipientService::MovePrimaryRecipient::getMigrationId() const {
    return _metadata.getMigrationId();
}

}  // namespace mongo
