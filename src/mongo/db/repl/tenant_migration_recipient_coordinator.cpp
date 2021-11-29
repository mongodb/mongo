/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "tenant_migration_recipient_coordinator.h"

#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"

namespace mongo::repl {
namespace {
const auto _coordinatorDecoration =
    ServiceContext::declareDecoration<TenantMigrationRecipientCoordinator>();
}  // namespace

TenantMigrationRecipientCoordinator* TenantMigrationRecipientCoordinator::get(
    ServiceContext* serviceContext) {
    return &_coordinatorDecoration(serviceContext);
}

TenantMigrationRecipientCoordinator* TenantMigrationRecipientCoordinator::get(
    OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

SharedSemiFuture<void> TenantMigrationRecipientCoordinator::step(UUID migrationId,
                                                                 MigrationProgressStepEnum step) {
    stdx::lock_guard lk(_mutex);
    tassert(6112806,
            str::stream() << "Current tenant migration step is '"
                          << MigrationProgressStep_serializer(_currentStep)
                          << "' but migrationId is " << _currentMigrationId << ", not none",
            _currentStep != MigrationProgressStepEnum::kNoStep || !_currentMigrationId);
    uassert(6112800,
            str::stream() << "Beginning tenant migration step '"
                          << MigrationProgressStep_serializer(step) << "' but previous step is '"
                          << MigrationProgressStep_serializer(_currentStep) << "'",
            step > _currentStep);
    uassert(6112801,
            str::stream() << "Beginning tenant migration step '"
                          << MigrationProgressStep_serializer(step)
                          << "' but previous step isn't done",
            _currentStep == MigrationProgressStepEnum::kNoStep || _promise->getFuture().isReady());
    uassert(6112807,
            str::stream() << "Beginning tenant migration step '"
                          << MigrationProgressStep_serializer(step) << "' with migrationId "
                          << _currentMigrationId << ", which doesn't match current migrationId "
                          << migrationId,
            _currentMigrationId == migrationId ||
                _currentStep == MigrationProgressStepEnum::kNoStep);

    _currentMigrationId = migrationId;
    _currentStep = step;
    _readyMembers.clear();
    _promise = std::make_unique<SharedPromise<void>>();
    return _promise->getFuture();
}

void TenantMigrationRecipientCoordinator::cancelStep(UUID migrationId,
                                                     MigrationProgressStepEnum step) {
    stdx::lock_guard lk(_mutex);
    tassert(6112808,
            str::stream() << "Trying to cancel tenant migration step '"
                          << MigrationProgressStep_serializer(step) << "'",
            step != MigrationProgressStepEnum::kNoStep);

    if (_promise->getFuture().isReady()) {
        return;
    }

    if (migrationId != _currentMigrationId || step != _currentStep) {
        LOGV2(6112802,
              "Ignoring stale request to cancel migration step",
              "migrationId"_attr = migrationId,
              "currentMigrationId"_attr = _currentMigrationId,
              "step"_attr = MigrationProgressStep_serializer(step),
              "currentStep"_attr = MigrationProgressStep_serializer(_currentStep));
        return;
    }

    LOGV2_DEBUG(6112803,
                2,
                "Cancelling migration step",
                "step"_attr = MigrationProgressStep_serializer(step),
                "currentStep"_attr = MigrationProgressStep_serializer(_currentStep));

    _promise->setError({ErrorCodes::OperationFailed,
                        str::stream() << "Cancelled migration step '"
                                      << MigrationProgressStep_serializer(_currentStep) << "'"});

    reset();
}

void TenantMigrationRecipientCoordinator::voteForStep(UUID migrationId,
                                                      MigrationProgressStepEnum step,
                                                      const HostAndPort& host,
                                                      bool success,
                                                      const boost::optional<StringData>& reason) {
    stdx::lock_guard lk(_mutex);
    uassert(6112804,
            str::stream() << "Received vote for step '" << MigrationProgressStep_serializer(step)
                          << "' but current step is '"
                          << MigrationProgressStep_serializer(_currentStep) << "'",
            step == _currentStep);

    if (!success) {
        _promise->setError({ErrorCodes::OperationFailed,
                            str::stream()
                                << "Migration step '" << MigrationProgressStep_serializer(step)
                                << "' failed on " << host << ", error: " << reason});
        return;
    }

    _readyMembers.insert(host);
    std::vector<HostAndPort> readyMembersVector(_readyMembers.begin(), _readyMembers.end());
    CommitQuorumOptions allVotingMembers{CommitQuorumOptions::kVotingMembers};
    auto ready = repl::ReplicationCoordinator::get(getGlobalServiceContext())
                     ->isCommitQuorumSatisfied(allVotingMembers, readyMembersVector);

    if (ready) {
        LOGV2_DEBUG(6112809,
                    2,
                    "Completing migration step",
                    "step"_attr = MigrationProgressStep_serializer(step),
                    "currentStep"_attr = MigrationProgressStep_serializer(_currentStep));
        // Fulfill the promise.
        _promise->emplaceValue();
    }
}

void TenantMigrationRecipientCoordinator::reset() {
    stdx::lock_guard lk(_mutex);
    _currentMigrationId.reset();
    _currentStep = MigrationProgressStepEnum::kNoStep;
    _readyMembers.clear();
    _promise = std::make_unique<SharedPromise<void>>();
}

}  // namespace mongo::repl
