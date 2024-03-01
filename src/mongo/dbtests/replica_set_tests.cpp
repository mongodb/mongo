/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/concurrency/admission_context.h"

namespace mongo {
namespace {

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

class ReplicaSetTest : public mongo::unittest::Test {
protected:
    void setUp() {
        auto opCtx = makeOpCtx();
        _storageInterface = std::make_unique<repl::StorageInterfaceImpl>();
        _dropPendingCollectionReaper =
            std::make_unique<repl::DropPendingCollectionReaper>(_storageInterface.get());
        auto consistencyMarkers =
            std::make_unique<repl::ReplicationConsistencyMarkersImpl>(_storageInterface.get());
        auto recovery = std::make_unique<repl::ReplicationRecoveryImpl>(_storageInterface.get(),
                                                                        consistencyMarkers.get());
        _replicationProcess = std::make_unique<repl::ReplicationProcess>(
            _storageInterface.get(), std::move(consistencyMarkers), std::move(recovery));
        _replCoordExternalState = std::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            opCtx->getServiceContext(),
            _dropPendingCollectionReaper.get(),
            _storageInterface.get(),
            _replicationProcess.get());
        ASSERT_OK(_replCoordExternalState->createLocalLastVoteCollection(opCtx.get()));
    }

    void tearDown() {
        auto opCtx = makeOpCtx();
        DBDirectClient client(opCtx.get());
        client.dropCollection(NamespaceString::kLastVoteNamespace);

        _replCoordExternalState.reset();
        _dropPendingCollectionReaper.reset();
        _storageInterface.reset();
        _replicationProcess.reset();
    }

    repl::ReplicationCoordinatorExternalStateImpl* getReplCoordExternalState() {
        return _replCoordExternalState.get();
    }

    repl::StorageInterface& getStorageInterface() {
        return *_storageInterface;
    }

private:
    std::unique_ptr<repl::ReplicationCoordinatorExternalStateImpl> _replCoordExternalState;
    std::unique_ptr<repl::StorageInterface> _storageInterface;
    std::unique_ptr<repl::DropPendingCollectionReaper> _dropPendingCollectionReaper;
    std::unique_ptr<repl::ReplicationProcess> _replicationProcess;
};

TEST_F(ReplicaSetTest, ReplCoordExternalStateStoresLastVoteWithNewTerm) {
    auto opCtx = makeOpCtx();
    // Methods that do writes as part of elections expect the admission priority to be Immediate.
    ScopedAdmissionPriority priority(opCtx.get(), AdmissionContext::Priority::kImmediate);
    auto replCoordExternalState = getReplCoordExternalState();

    replCoordExternalState->storeLocalLastVoteDocument(opCtx.get(), repl::LastVote{2, 1})
        .transitional_ignore();

    auto lastVote = replCoordExternalState->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);

    replCoordExternalState->storeLocalLastVoteDocument(opCtx.get(), repl::LastVote{3, 1})
        .transitional_ignore();

    lastVote = replCoordExternalState->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 3);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);
}

TEST_F(ReplicaSetTest, ReplCoordExternalStateDoesNotStoreLastVoteWithOldTerm) {
    auto opCtx = makeOpCtx();
    // Methods that do writes as part of elections expect the admission priority to be Immediate.
    ScopedAdmissionPriority priority(opCtx.get(), AdmissionContext::Priority::kImmediate);
    auto replCoordExternalState = getReplCoordExternalState();

    replCoordExternalState->storeLocalLastVoteDocument(opCtx.get(), repl::LastVote{2, 1})
        .transitional_ignore();

    auto lastVote = replCoordExternalState->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);

    replCoordExternalState->storeLocalLastVoteDocument(opCtx.get(), repl::LastVote{1, 1})
        .transitional_ignore();

    lastVote = replCoordExternalState->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);
}

TEST_F(ReplicaSetTest, ReplCoordExternalStateDoesNotStoreLastVoteWithEqualTerm) {
    auto opCtx = makeOpCtx();
    // Methods that do writes as part of elections expect the admission priority to be Immediate.
    ScopedAdmissionPriority priority(opCtx.get(), AdmissionContext::Priority::kImmediate);
    auto replCoordExternalState = getReplCoordExternalState();

    replCoordExternalState->storeLocalLastVoteDocument(opCtx.get(), repl::LastVote{2, 1})
        .transitional_ignore();

    auto lastVote = replCoordExternalState->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);

    replCoordExternalState->storeLocalLastVoteDocument(opCtx.get(), repl::LastVote{2, 2})
        .transitional_ignore();

    lastVote = replCoordExternalState->loadLocalLastVoteDocument(opCtx.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);
}

}  // namespace
}  // namespace mongo
