// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo {
namespace {

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

class ReplicaSetTest : public mongo::unittest::Test {
protected:
    void setUp() override {
        auto opCtx = makeOpCtx();
        _storageInterface = std::make_unique<repl::StorageInterfaceImpl>();
        auto consistencyMarkers =
            std::make_unique<repl::ReplicationConsistencyMarkersImpl>(_storageInterface.get());
        auto recovery = std::make_unique<repl::ReplicationRecoveryImpl>(_storageInterface.get(),
                                                                        consistencyMarkers.get());
        _replicationProcess = std::make_unique<repl::ReplicationProcess>(
            _storageInterface.get(), std::move(consistencyMarkers), std::move(recovery));
        _replCoordExternalState = std::make_unique<repl::ReplicationCoordinatorExternalStateImpl>(
            opCtx->getServiceContext(), _storageInterface.get(), _replicationProcess.get());
        ASSERT_OK(_replCoordExternalState->createLocalLastVoteCollection(opCtx.get()));
    }

    void tearDown() override {
        auto opCtx = makeOpCtx();
        DBDirectClient client(opCtx.get());
        client.dropCollection(NamespaceString::kLastVoteNamespace);

        _replCoordExternalState.reset();
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
    std::unique_ptr<repl::ReplicationProcess> _replicationProcess;
};

TEST_F(ReplicaSetTest, ReplCoordExternalStateStoresLastVoteWithNewTerm) {
    auto opCtx = makeOpCtx();
    // Methods that do writes as part of elections expect the admission priority to be Immediate.
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
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
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
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
    ScopedAdmissionPriority<ExecutionAdmissionContext> priority(
        opCtx.get(), AdmissionContext::Priority::kExempt);
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
