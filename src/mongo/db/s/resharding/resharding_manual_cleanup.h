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

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/util/uuid.h"

namespace mongo {

class OperationContext;

template <class Service, class StateMachine, class ReshardingDocument>
class ReshardingCleaner {
public:
    ReshardingCleaner(NamespaceString reshardingDocumentNss,
                      NamespaceString originalCollectionNss,
                      UUID reshardingUUID);

    virtual ~ReshardingCleaner() = default;

    void clean(OperationContext* opCtx);

protected:
    virtual void _doClean(OperationContext* opCtx, const ReshardingDocument& doc) = 0;

    virtual void _abortMachine(StateMachine& machine) = 0;

    boost::optional<ReshardingDocument> _fetchReshardingDocumentFromDisk(OperationContext* opCtx);

    void _waitOnMachineCompletionIfExists(OperationContext* opCtx);

    const NamespaceString _originalCollectionNss;

    const NamespaceString _reshardingDocumentNss;

    const UUID _reshardingUUID;

private:
    PersistentTaskStore<ReshardingDocument> _store;
};

class ReshardingCoordinatorCleaner
    : public ReshardingCleaner<ReshardingCoordinatorService,
                               ReshardingCoordinatorService::ReshardingCoordinator,
                               ReshardingCoordinatorDocument> {
public:
    ReshardingCoordinatorCleaner(NamespaceString nss, UUID reshardingUUID);

private:
    void _doClean(OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) override;

    void _abortMachine(ReshardingCoordinatorService::ReshardingCoordinator& machine) override;

    void _cleanOnParticipantShards(OperationContext* opCtx,
                                   const ReshardingCoordinatorDocument& doc);

    bool _checkExistsTempReshardingCollection(OperationContext* opCtx,
                                              const NamespaceString& tempReshardingNss);

    void _dropTemporaryReshardingCollection(OperationContext* opCtx,
                                            const NamespaceString& tempReshardingNss);
};

class ReshardingDonorCleaner : public ReshardingCleaner<ReshardingDonorService,
                                                        ReshardingDonorService::DonorStateMachine,
                                                        ReshardingDonorDocument> {
public:
    ReshardingDonorCleaner(NamespaceString nss, UUID reshardingUUID);

private:
    void _doClean(OperationContext* opCtx, const ReshardingDonorDocument& doc) override {}

    /**
     * The donor receives the abort signal through observing changes on the collection entry, so
     * there's no need for a direct abort.
     */
    void _abortMachine(ReshardingDonorService::DonorStateMachine& machine) override {}
};

class ReshardingRecipientCleaner
    : public ReshardingCleaner<ReshardingRecipientService,
                               ReshardingRecipientService::RecipientStateMachine,
                               ReshardingRecipientDocument> {
public:
    ReshardingRecipientCleaner(NamespaceString nss, UUID reshardingUUID);

private:
    void _doClean(OperationContext* opCtx, const ReshardingRecipientDocument& doc) override;

    /**
     * The recipient receives the abort signal through observing changes on the collection entry,
     * so there's no need for a direct abort.
     */
    void _abortMachine(ReshardingRecipientService::RecipientStateMachine& machine) override {}
};

}  // namespace mongo
