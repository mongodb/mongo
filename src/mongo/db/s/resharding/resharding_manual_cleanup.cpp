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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/db/s/resharding/resharding_manual_cleanup.h"

#include "mongo/logv2/log.h"

namespace mongo {

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
          "namespace"_attr = _originalCollectionNss,
          "reshardingUUID"_attr = _reshardingUUID,
          "serviceType"_attr = Service::kServiceName);

    auto reshardingDocument = _fetchReshardingDocumentFromDisk(opCtx);
    if (!reshardingDocument) {
        return;
    }

    opCtx->setAlwaysInterruptAtStepDownOrUp();

    _waitOnMachineCompletionIfExists(opCtx);

    _doClean(opCtx, *reshardingDocument);

    // TODO SERVER-54035 Remove resharding document on local disk.
}

template <class Service, class StateMachine, class ReshardingDocument>
boost::optional<ReshardingDocument>
ReshardingCleaner<Service, StateMachine, ReshardingDocument>::_fetchReshardingDocumentFromDisk(
    OperationContext* opCtx) {
    return boost::none;
}

template <class Service, class StateMachine, class ReshardingDocument>
void ReshardingCleaner<Service, StateMachine, ReshardingDocument>::_waitOnMachineCompletionIfExists(
    OperationContext* opCtx) {}

template class ReshardingCleaner<ReshardingCoordinatorService,
                                 ReshardingCoordinatorService::ReshardingCoordinator,
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
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
}

void ReshardingCoordinatorCleaner::_doClean(OperationContext* opCtx,
                                            const ReshardingCoordinatorDocument& doc) {
    _cleanOnParticipantShards(opCtx, doc);
    _dropTemporaryReshardingCollection(opCtx);
}

void ReshardingCoordinatorCleaner::_cleanOnParticipantShards(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) {}

void ReshardingCoordinatorCleaner::_dropTemporaryReshardingCollection(OperationContext* opCtx) {}

ReshardingDonorCleaner::ReshardingDonorCleaner(NamespaceString nss, UUID reshardingUUID)
    : ReshardingCleaner(NamespaceString::kDonorReshardingOperationsNamespace,
                        std::move(nss),
                        std::move(reshardingUUID)) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
}

ReshardingRecipientCleaner::ReshardingRecipientCleaner(NamespaceString nss, UUID reshardingUUID)
    : ReshardingCleaner(NamespaceString::kRecipientReshardingOperationsNamespace,
                        std::move(nss),
                        std::move(reshardingUUID)) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
}

void ReshardingRecipientCleaner::_doClean(OperationContext* opCtx,
                                          const ReshardingRecipientDocument& doc) {
    // TODO SERVER-54035 Call into shared recipient metadata cleanup function.
}

}  // namespace mongo
