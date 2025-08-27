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

const std::string kReshardingCoordinatorActiveIndexName = "ReshardingCoordinatorActiveIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopAttemptingToCreateIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

}  // namespace

ThreadPool::Limits ReshardingCoordinatorService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingCoordinatorServiceMaxThreadCount;
    return threadPoolLimit;
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

void ReshardingCoordinatorService::abortAllReshardCollection(OperationContext* opCtx) {
    std::vector<SharedSemiFuture<void>> reshardingCoordinatorFutures;

    for (auto& instance : getAllInstances(opCtx)) {
        auto reshardingCoordinator = checked_pointer_cast<ReshardingCoordinator>(instance);
        reshardingCoordinatorFutures.push_back(
            reshardingCoordinator->getQuiescePeriodFinishedFuture());
        reshardingCoordinator->abort(true /* skip quiesce period */);
    }

    for (auto&& future : reshardingCoordinatorFutures) {
        future.wait(opCtx);
    }
}

}  // namespace mongo
