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
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/sharded_rename_collection_gen.h"

namespace mongo {

class RenameCollectionParticipantService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "RenameCollectionParticipantService"_sd;

    explicit RenameCollectionParticipantService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}

    ~RenameCollectionParticipantService() = default;

    static RenameCollectionParticipantService* getService(OperationContext* opCtx);

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kShardingRenameParticipantsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        return ThreadPool::Limits();
    }

    // The service implemented its own conflict check before this method was added.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override{};

    std::shared_ptr<Instance> constructInstance(BSONObj initialState) override;
};

/*
 * POS instance managing a rename operation on a single node.
 *
 * At a higher level, a rename operation corresponds to 2 steps.
 * - STEP 1 (upon receiving `ShardsvrRenameCollectionParticipantCommand`):
 * -- Block CRUD operations, drop target and rename source collection.
 *
 * - STEP 2 (Upon receiving `ShardsvrRenameCollectionUnblockParticipantCommand`):
 * --  Unblock CRUD operations.
 *
 */
class RenameParticipantInstance
    : public repl::PrimaryOnlyService::TypedInstance<RenameParticipantInstance> {
public:
    using StateDoc = RenameCollectionParticipantDocument;
    using Phase = RenameCollectionParticipantPhaseEnum;

    explicit RenameParticipantInstance(const BSONObj& participantDoc)
        : _doc(RenameCollectionParticipantDocument::parse(
              IDLParserErrorContext("RenameCollectionParticipantDocument"), participantDoc)) {}

    ~RenameParticipantInstance();

    /*
     * Check if the given participant document has the same options as the current instance.
     * If it does, the participant can be joined.
     */
    bool hasSameOptions(const BSONObj& participantDoc);

    BSONObj doc() {
        return _doc.toBSON();
    }

    const NamespaceString& fromNss() {
        return _doc.getFromNss();
    }

    const UUID& sourceUUID() {
        return _doc.getSourceUUID();
    }

    const NamespaceString& toNss() {
        return _doc.getTo();
    }

    /*
     * Returns a future that will be ready when the local rename is completed.
     */
    SharedSemiFuture<void> getBlockCRUDAndRenameCompletionFuture() {
        return _blockCRUDAndRenameCompletionPromise.getFuture();
    }

    /*
     * Flags CRUD operations as ready to be served and returns a future that will be ready right
     * after releasing the critical section on source and target collection.
     */
    SharedSemiFuture<void> getUnblockCrudFuture() {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_canUnblockCRUDPromise.getFuture().isReady()) {
            _canUnblockCRUDPromise.setFrom(Status::OK());
        }

        return _unblockCRUDPromise.getFuture();
    }

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override {}

private:
    RenameCollectionParticipantDocument _doc;

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override final;

    SemiFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                              const CancellationToken& token) noexcept;

    void interrupt(Status status) noexcept override final;

    template <typename Func>
    auto _executePhase(const Phase& newPhase, Func&& func) {
        return [=] {
            const auto& currPhase = _doc.getPhase();

            if (currPhase > newPhase) {
                // Do not execute this phase if we already reached a subsequent one.
                return;
            }
            if (currPhase < newPhase) {
                // Persist the new phase if this is the first time we are executing it.
                _enterPhase(newPhase);
            }
            return func();
        };
    }

    void _removeStateDocument(OperationContext* opCtx);
    void _enterPhase(Phase newPhase);
    void _invalidateFutures(const Status& errStatus);

    Mutex _mutex = MONGO_MAKE_LATCH("RenameParticipantInstance::_mutex");

    // Ready when step 1 (drop target && rename source) has been completed: once set, a successful
    // response to `ShardsvrRenameCollectionParticipantCommand` can be returned to the coordinator.
    SharedPromise<void> _blockCRUDAndRenameCompletionPromise;

    // Ready when the "unblock CRUD" command has been received: once set, the participant POS can
    // proceed to unblock CRUD operations.
    SharedPromise<void> _canUnblockCRUDPromise;

    // Ready when step 2 (unblock CRUD operations) have been completed: once set, a successful
    // response to `ShardsvrRenameCollectionUnblockParticipantCommand` can be returned to the
    // coordinator.
    SharedPromise<void> _unblockCRUDPromise;
};

}  // namespace mongo
