// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopAttemptingToCreateIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

}  // namespace

auto ReshardingCoordinatorService::getThreadPoolLimits() const -> ThreadPoolLimits {
    return {.maxThreads =
                static_cast<size_t>(resharding::gReshardingCoordinatorServiceMaxThreadCount)};
}

void ReshardingCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto coordinatorDoc = ReshardingCoordinatorDocument::parse(
        initialState,
        IDLParserContext("ReshardingCoordinatorService::checkIfConflictsWithOtherInstances"));

    for (const auto& instance : existingInstances) {
        auto typedInstance = checked_cast<const ReshardingCoordinator*>(instance);
        // Instances which have already completed do not conflict with other instances, unless
        // their user resharding UUIDs are the same.
        const bool isUserReshardingUUIDSame =
            typedInstance->getMetadata().getUserReshardingUUID() ==
            coordinatorDoc.getUserReshardingUUID();
        if (!isUserReshardingUUIDSame && typedInstance->getCompletionFuture().isReady()) {
            LOGV2_DEBUG(7760400,
                        1,
                        "Ignoring 'conflict' with completed instance of resharding",
                        "newNss"_attr = coordinatorDoc.getSourceNss(),
                        "oldNss"_attr = typedInstance->getMetadata().getSourceNss(),
                        "newUUID"_attr = coordinatorDoc.getReshardingUUID(),
                        "oldUUID"_attr = typedInstance->getMetadata().getReshardingUUID());
            continue;
        }
        // For resharding commands with no UUID provided by the user, we will re-connect to an
        // instance with the same NS and resharding key, if that instance was originally started
        // with no user-provided UUID. If a UUID is provided by the user, we will connect only
        // to the original instance.
        const bool isNssSame =
            typedInstance->getMetadata().getSourceNss() == coordinatorDoc.getSourceNss();
        const bool isReshardingKeySame = SimpleBSONObjComparator::kInstance.evaluate(
            typedInstance->getMetadata().getReshardingKey().toBSON() ==
            coordinatorDoc.getReshardingKey().toBSON());

        const bool isProvenanceSame =
            (typedInstance->getMetadata().getProvenance() ==
             coordinatorDoc.getCommonReshardingMetadata().getProvenance());

        iassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Only one resharding operation is allowed to be active at a "
                                 "time, aborting resharding op for "
                              << coordinatorDoc.getSourceNss().toStringForErrorMsg(),
                isUserReshardingUUIDSame && isNssSame && isReshardingKeySame && isProvenanceSame);

        std::string userReshardingIdMsg;
        if (coordinatorDoc.getUserReshardingUUID()) {
            userReshardingIdMsg = str::stream()
                << " and user resharding UUID " << coordinatorDoc.getUserReshardingUUID();
        }

        iasserted(ReshardingCoordinatorServiceConflictingOperationInProgressInfo(
                      typedInstance->shared_from_this()),
                  str::stream() << "Found an active resharding operation for "
                                << coordinatorDoc.getSourceNss().toStringForErrorMsg()
                                << " with resharding key "
                                << coordinatorDoc.getReshardingKey().toString()
                                << userReshardingIdMsg);
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingCoordinatorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<ReshardingCoordinator>(
        this,
        ReshardingCoordinatorDocument::parse(initialState,
                                             IDLParserContext("ReshardingCoordinatorStateDoc")),
        std::make_shared<ReshardingCoordinatorExternalStateImpl>(),
        _serviceContext);
}

ExecutorFuture<void> ReshardingCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {

    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);
               BSONObj result;
               client.runCommand(nss.dbName(), BSON("create" << nss.coll()), result);
               const auto& status = getStatusFromCommandResult(result);
               if (status.code() != ErrorCodes::NamespaceExists) {
                   uassertStatusOK(status);
               }
           })
        .until([token](Status status) { return shouldStopAttemptingToCreateIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void ReshardingCoordinatorService::abortAllReshardCollection(
    OperationContext* opCtx, ReshardingCoordinator::AbortRequest abortRequest) {
    std::vector<SharedSemiFuture<void>> reshardingCoordinatorFutures;

    for (auto& instance : getAllInstances(opCtx)) {
        auto reshardingCoordinator = checked_pointer_cast<ReshardingCoordinator>(instance);
        reshardingCoordinatorFutures.push_back(
            reshardingCoordinator->getQuiescePeriodFinishedFuture());
        reshardingCoordinator->abort(abortRequest);
    }

    for (auto&& future : reshardingCoordinatorFutures) {
        future.wait(opCtx);
    }
}

void ReshardingCoordinatorService::stepDown_forTest() {
    LOGV2(12755408, "Performing resharding coordinator service stepdown for test");
    onStepDown_forTest();
}

void ReshardingCoordinatorService::stepUp_forTest() {
    LOGV2(12755409, "Performing resharding coordinator service stepup for test");
    onStepUp_forTest();
}

}  // namespace mongo
