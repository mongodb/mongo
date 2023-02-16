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

#include <boost/none.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/move_primary/move_primary_server_parameters_gen.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {

MONGO_FAIL_POINT_DEFINE(movePrimaryRecipientPauseAfterInsertingStateDoc);

namespace {

constexpr StringData kUninitialized = "uninitialized"_sd;
constexpr StringData kStarted = "started"_sd;
constexpr StringData kConsistent = "consistent"_sd;
constexpr StringData kBlocking = "blocking"_sd;
constexpr StringData kAborted = "aborted"_sd;
constexpr StringData kDone = "done"_sd;

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

StringData MovePrimaryRecipientService::MovePrimaryRecipient::_parseRecipientState(
    MovePrimaryRecipientState value) {
    switch (value) {
        case MovePrimaryRecipientState::kUninitialized:
            return kUninitialized;
        case MovePrimaryRecipientState::kStarted:
            return kStarted;
        case MovePrimaryRecipientState::kConsistent:
            return kConsistent;
        case MovePrimaryRecipientState::kBlocking:
            return kBlocking;
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
        this, recipientStateDoc, _serviceContext);
}

MovePrimaryRecipientService::MovePrimaryRecipient::MovePrimaryRecipient(
    const MovePrimaryRecipientService* service,
    MovePrimaryRecipientDocument recipientDoc,
    ServiceContext* serviceContext)
    : _recipientService(service),
      _metadata(recipientDoc.getMovePrimaryRecipientMetadata()),
      _serviceContext(serviceContext),
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

SemiFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {

    // (Generic FCV reference): This FCV check should exist across LTS binary versions.
    if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
        LOGV2(7271200, "Aborting movePrimary as the recipient is upgrading or downgrading");
    }

    return ExecutorFuture(**executor)
        .then([this, self = shared_from_this(), executor, token] {
            return _enterStartedState(executor, token);
        })
        .semi();
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_enterStartedState(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {

    {
        stdx::lock_guard<Latch> lg(_mutex);

        if (_state > MovePrimaryRecipientState::kUninitialized) {
            return ExecutorFuture(**executor);
        }

        MovePrimaryRecipientState _oldState = _state;
        _state = MovePrimaryRecipientState::kStarted;

        LOGV2(7271201,
              "Transitioned movePrimary recipient state",
              "oldState"_attr = _parseRecipientState(_oldState),
              "newState"_attr = _parseRecipientState(_state),
              "migrationId"_attr = _metadata.getMigrationId());
    }

    return _persistRecipientDoc(executor, token).then([this, self = shared_from_this()] {
        movePrimaryRecipientPauseAfterInsertingStateDoc.pauseWhileSet();
    });
}

ExecutorFuture<void> MovePrimaryRecipientService::MovePrimaryRecipient::_persistRecipientDoc(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const CancellationToken& token) {

    return AsyncTry([this, self = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               MovePrimaryRecipientDocument recipientDoc;
               recipientDoc.setMovePrimaryRecipientMetadata(_metadata);
               recipientDoc.setId(_metadata.getMigrationId());
               recipientDoc.setState(MovePrimaryRecipientState::kStarted);
               recipientDoc.setStartAt(_serviceContext->getPreciseClockSource()->now());

               PersistentTaskStore<MovePrimaryRecipientDocument> store(
                   NamespaceString::kMovePrimaryRecipientNamespace);
               store.add(opCtx, recipientDoc, WriteConcerns::kMajorityWriteConcernNoTimeout);
               if (!_recipientDocDurablePromise.getFuture().isReady()) {
                   _recipientDocDurablePromise.emplaceValue();
               }
           })
        .until([&](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
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
