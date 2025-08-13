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


#include "mongo/db/s/resharding/resharding_manual_cleanup.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/request_types/cleanup_reshard_collection_gen.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

namespace {

std::vector<ShardId> getAllParticipantsFromCoordDoc(const ReshardingCoordinatorDocument& doc) {
    std::vector<ShardId> participants;

    auto donorShards = resharding::extractShardIdsFromParticipantEntriesAsSet(doc.getDonorShards());
    auto recipientShards =
        resharding::extractShardIdsFromParticipantEntriesAsSet(doc.getRecipientShards());
    std::set_union(donorShards.begin(),
                   donorShards.end(),
                   recipientShards.begin(),
                   recipientShards.end(),
                   std::back_inserter(participants));

    return participants;
}

std::vector<AsyncRequestsSender::Request> createShardCleanupRequests(
    const NamespaceString& nss, UUID reshardingUUID, const ReshardingCoordinatorDocument& doc) {

    auto participants = getAllParticipantsFromCoordDoc(doc);
    std::vector<AsyncRequestsSender::Request> requests;
    for (const auto& participant : participants) {
        requests.emplace_back(participant,
                              ShardsvrCleanupReshardCollection(nss, reshardingUUID).toBSON());
    }
    return requests;
}

void assertResponseOK(const NamespaceString& nss,
                      StatusWith<executor::RemoteCommandResponse> response,
                      ShardId shardId) {
    auto errorContext =
        fmt::format("Unable to cleanup reshard collection for namespace {} on shard {}",
                    nss.toStringForErrorMsg(),
                    shardId.toString());
    auto shardResponse = uassertStatusOKWithContext(std::move(response), errorContext);

    auto status = getStatusFromCommandResult(shardResponse.data);
    uassertStatusOKWithContext(status, errorContext);

    auto wcStatus = getWriteConcernStatusFromCommandResult(shardResponse.data);
    uassertStatusOKWithContext(wcStatus, errorContext);
}

}  // anonymous namespace

template <class Service, class StateMachine, class ReshardingDocument>
ReshardingCleaner<Service, StateMachine, ReshardingDocument>::ReshardingCleaner(
    NamespaceString reshardingDocumentNss,
    NamespaceString originalCollectionNss,
    UUID reshardingUUID)
    : _originalCollectionNss(originalCollectionNss),
      _reshardingDocumentNss(reshardingDocumentNss),
      _reshardingUUID(reshardingUUID),
      _store(reshardingDocumentNss) {}

template <class Service, class StateMachine, class ReshardingDocument>
void ReshardingCleaner<Service, StateMachine, ReshardingDocument>::clean(OperationContext* opCtx) {

    LOGV2(5403503,
          "Cleaning up resharding operation",
          logAttrs(_originalCollectionNss),
          "reshardingUUID"_attr = _reshardingUUID,
          "serviceType"_attr = Service::kServiceName);

    auto reshardingDocument = _fetchReshardingDocumentFromDisk(opCtx);
    if (!reshardingDocument) {
        return;
    }

    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    _waitOnMachineCompletionIfExists(opCtx);

    // Do another fetch of the document in case metadata has changed as a result of the machine
    // completion. If the document does not exist now, keep the member variable as-is to retain
    // access to metadata information.
    if (auto attemptRetrieveFinalReshardingDocument = _fetchReshardingDocumentFromDisk(opCtx)) {
        reshardingDocument = attemptRetrieveFinalReshardingDocument;
    }

    _doClean(opCtx, *reshardingDocument);

    // Remove resharding document on disk.
    _store.remove(opCtx, BSON(ReshardingDocument::kReshardingUUIDFieldName << _reshardingUUID));
}

template <class Service, class StateMachine, class ReshardingDocument>
boost::optional<ReshardingDocument>
ReshardingCleaner<Service, StateMachine, ReshardingDocument>::_fetchReshardingDocumentFromDisk(
    OperationContext* opCtx) {
    boost::optional<ReshardingDocument> docOptional;
    _store.forEach(opCtx,
                   BSON(ReshardingDocument::kReshardingUUIDFieldName << _reshardingUUID),
                   [&](const ReshardingDocument& doc) {
                       docOptional.emplace(doc);
                       return false;
                   });
    return docOptional;
}

template <class Service, class StateMachine, class ReshardingDocument>
void ReshardingCleaner<Service, StateMachine, ReshardingDocument>::_waitOnMachineCompletionIfExists(
    OperationContext* opCtx) {
    auto machine =
        resharding::tryGetReshardingStateMachine<Service, StateMachine, ReshardingDocument>(
            opCtx, _reshardingUUID);

    if (machine) {
        auto completionFuture = machine->get()->getCompletionFuture();
        _abortMachine(**machine);
        auto completionStatus = completionFuture.waitNoThrow(opCtx);
        if (!completionStatus.isOK()) {
            LOGV2_INFO(
                5403505,
                "While cleaning up resharding operation, discovered that the original operation "
                "failed; no action required",
                "originalError"_attr = redact(completionStatus));
        }
    }
}

template class ReshardingCleaner<ReshardingCoordinatorService,
                                 ReshardingCoordinator,
                                 ReshardingCoordinatorDocument>;

template class ReshardingCleaner<ReshardingDonorService,
                                 ReshardingDonorService::DonorStateMachine,
                                 ReshardingDonorDocument>;

template class ReshardingCleaner<ReshardingRecipientService,
                                 ReshardingRecipientService::RecipientStateMachine,
                                 ReshardingRecipientDocument>;

ReshardingCoordinatorCleaner::ReshardingCoordinatorCleaner(NamespaceString nss, UUID reshardingUUID)
    : ReshardingCleaner(NamespaceString::kConfigReshardingOperationsNamespace,
                        std::move(nss),
                        std::move(reshardingUUID)) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
}

void ReshardingCoordinatorCleaner::_doClean(OperationContext* opCtx,
                                            const ReshardingCoordinatorDocument& doc) {
    _cleanOnParticipantShards(opCtx, doc);

    if (!_checkExistsTempReshardingCollection(opCtx, doc.getTempReshardingNss())) {
        return;
    }

    // Only drop the temporary resharding collection if the coordinator's state indicates that the
    // resharding operation exited before persisting its decision to commit.
    uassert(ErrorCodes::ManualInterventionRequired,
            "Can't drop temporary resharding collection if resharding operation has already "
            "committed ",
            doc.getState() != CoordinatorStateEnum::kCommitting);

    _dropTemporaryReshardingCollection(opCtx, doc.getTempReshardingNss());
}

void ReshardingCoordinatorCleaner::_abortMachine(ReshardingCoordinator& machine) {
    machine.abort();
}

void ReshardingCoordinatorCleaner::_cleanOnParticipantShards(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) {
    AsyncRequestsSender ars(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
        DatabaseName::kAdmin,
        createShardCleanupRequests(_originalCollectionNss, _reshardingUUID, doc),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        Shard::RetryPolicy::kIdempotent,
        nullptr /* resourceYielder */,
        {} /* designatedHostsMap */);

    while (!ars.done()) {
        auto arsResponse = ars.next();
        assertResponseOK(_originalCollectionNss,
                         std::move(arsResponse.swResponse),
                         std::move(arsResponse.shardId));
    }
}

bool ReshardingCoordinatorCleaner::_checkExistsTempReshardingCollection(
    OperationContext* opCtx, const NamespaceString& tempReshardingNss) {
    try {
        Grid::get(opCtx)->catalogClient()->getCollection(
            opCtx, tempReshardingNss, repl::ReadConcernLevel::kMajorityReadConcern);
        return true;
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // If the temporary resharding collection has already been dropped, exit early.
        return false;
    }
}

void ReshardingCoordinatorCleaner::_dropTemporaryReshardingCollection(
    OperationContext* opCtx, const NamespaceString& tempReshardingNss) {
    ShardsvrDropCollection dropCollectionCommand(tempReshardingNss);
    dropCollectionCommand.setDbName(tempReshardingNss.dbName());
    generic_argument_util::setMajorityWriteConcern(dropCollectionCommand,
                                                   &opCtx->getWriteConcern());

    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(),
                                             tempReshardingNss.dbName());
    router.route(opCtx,
                 "dropTemporaryReshardingCollection"_sd,
                 [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                     auto cmdResponse = executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                         opCtx,
                         tempReshardingNss.dbName(),
                         dbInfo,
                         dropCollectionCommand.toBSON(),
                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                         Shard::RetryPolicy::kIdempotent);

                     assertResponseOK(_originalCollectionNss,
                                      std::move(cmdResponse.swResponse),
                                      std::move(cmdResponse.shardId));
                 });
}

ReshardingDonorCleaner::ReshardingDonorCleaner(NamespaceString nss, UUID reshardingUUID)
    : ReshardingCleaner(NamespaceString::kDonorReshardingOperationsNamespace,
                        std::move(nss),
                        std::move(reshardingUUID)) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
}

ReshardingRecipientCleaner::ReshardingRecipientCleaner(NamespaceString nss, UUID reshardingUUID)
    : ReshardingCleaner(NamespaceString::kRecipientReshardingOperationsNamespace,
                        std::move(nss),
                        std::move(reshardingUUID)) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
}

void ReshardingRecipientCleaner::_doClean(OperationContext* opCtx,
                                          const ReshardingRecipientDocument& doc) {
    resharding::data_copy::ensureOplogCollectionsDropped(
        opCtx, doc.getReshardingUUID(), doc.getSourceUUID(), doc.getDonorShards());
}

}  // namespace mongo
