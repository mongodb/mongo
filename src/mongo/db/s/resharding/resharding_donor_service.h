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

#pragma once

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/donor_document_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"

namespace mongo {

class ReshardingDonorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingDonorService"_sd;

    explicit ReshardingDonorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}
    ~ReshardingDonorService() = default;

    class DonorStateMachine;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kDonorReshardingOperationsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        // TODO Limit the size of ReshardingDonorService thread pool.
        return ThreadPool::Limits();
    }

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) const override;
};

/**
 * Represents the current state of a resharding donor operation on this shard. This class drives
 * state transitions and updates to underlying on-disk metadata.
 */
class ReshardingDonorService::DonorStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<DonorStateMachine> {
public:
    explicit DonorStateMachine(const BSONObj& donorDoc);

    ~DonorStateMachine();

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancelationToken& token) noexcept override;

    void interrupt(Status status) override;

    /**
     * Returns a Future that will be resolved when all work associated with this Instance has
     * completed running.
     */
    SharedSemiFuture<void> getCompletionFuture() const {
        return _completionPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void onReshardingFieldsChanges(const TypeCollectionReshardingFields& reshardingFields);

    SharedSemiFuture<void> awaitFinalOplogEntriesWritten();

private:
    // The following functions correspond to the actions to take at a particular donor state.
    void _transitionToPreparingToDonate();

    void _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData();

    ExecutorFuture<void> _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    ExecutorFuture<void> _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToMirror(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    void _writeTransactionOplogEntryThenTransitionToMirroring();

    ExecutorFuture<void> _awaitCoordinatorHasDecisionPersistedThenTransitionToDropping(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Drops the original collection and throws if the returned status is not either Status::OK()
    // or NamespaceNotFound.
    void _dropOriginalCollection();

    // Transitions the state on-disk and in-memory to 'endState'.
    void _transitionState(DonorStateEnum endState,
                          boost::optional<Timestamp> minFetchTimestamp = boost::none,
                          boost::optional<Status> abortReason = boost::none);

    void _transitionStateAndUpdateCoordinator(
        DonorStateEnum endState,
        boost::optional<Timestamp> minFetchTimestamp = boost::none,
        boost::optional<Status> abortReason = boost::none,
        boost::optional<ReshardingCloneSize> cloneSizeEstimate = boost::none);

    // Inserts 'doc' on-disk and sets '_donorDoc' in-memory.
    void _insertDonorDocument(const ReshardingDonorDocument& doc);

    // Updates the donor document on-disk and in-memory with the 'replacementDoc.'
    void _updateDonorDocument(ReshardingDonorDocument&& replacementDoc);

    // Removes the local donor document from disk and clears the in-memory state.
    void _removeDonorDocument();

    // The in-memory representation of the underlying document in
    // config.localReshardingOperations.donor.
    ReshardingDonorDocument _donorDoc;

    // The id both for the resharding operation and for the primary-only-service instance.
    const UUID _id;

    // Protects the promises below
    Mutex _mutex = MONGO_MAKE_LATCH("ReshardingDonor::_mutex");

    // Each promise below corresponds to a state on the donor state machine. They are listed in
    // ascending order, such that the first promise below will be the first promise fulfilled.
    SharedPromise<void> _allRecipientsDoneCloning;

    SharedPromise<void> _allRecipientsDoneApplying;

    SharedPromise<void> _finalOplogEntriesWritten;

    SharedPromise<void> _coordinatorHasDecisionPersisted;

    SharedPromise<void> _completionPromise;
};

}  // namespace mongo
