/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/last_vote.h"
#include "mongo/db/repl/replication_coordinator_external_state_impl.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

class ReplicaSetTest : public mongo::unittest::Test {
protected:
    void setUp() {
        auto txn = makeOpCtx();
        _storageInterface = stdx::make_unique<repl::StorageInterfaceMock>();
        _replCoordExternalState.reset(
            new repl::ReplicationCoordinatorExternalStateImpl(_storageInterface.get()));
    }

    void tearDown() {
        auto txn = makeOpCtx();
        DBDirectClient client(txn.get());
        client.dropCollection("local.replset.election");

        _replCoordExternalState.reset();
        _storageInterface.reset();
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
};

TEST_F(ReplicaSetTest, ReplCoordExternalStateStoresLastVoteWithNewTerm) {
    auto txn = makeOpCtx();
    auto replCoordExternalState = getReplCoordExternalState();

    replCoordExternalState->storeLocalLastVoteDocument(txn.get(), repl::LastVote{2, 1});

    auto lastVote = replCoordExternalState->loadLocalLastVoteDocument(txn.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);

    replCoordExternalState->storeLocalLastVoteDocument(txn.get(), repl::LastVote{3, 1});

    lastVote = replCoordExternalState->loadLocalLastVoteDocument(txn.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 3);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);
}

TEST_F(ReplicaSetTest, ReplCoordExternalStateDoesNotStoreLastVoteWithOldTerm) {
    auto txn = makeOpCtx();
    auto replCoordExternalState = getReplCoordExternalState();

    replCoordExternalState->storeLocalLastVoteDocument(txn.get(), repl::LastVote{2, 1});

    auto lastVote = replCoordExternalState->loadLocalLastVoteDocument(txn.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);

    replCoordExternalState->storeLocalLastVoteDocument(txn.get(), repl::LastVote{1, 1});

    lastVote = replCoordExternalState->loadLocalLastVoteDocument(txn.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);
}

TEST_F(ReplicaSetTest, ReplCoordExternalStateDoesNotStoreLastVoteWithEqualTerm) {
    auto txn = makeOpCtx();
    auto replCoordExternalState = getReplCoordExternalState();

    replCoordExternalState->storeLocalLastVoteDocument(txn.get(), repl::LastVote{2, 1});

    auto lastVote = replCoordExternalState->loadLocalLastVoteDocument(txn.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);

    replCoordExternalState->storeLocalLastVoteDocument(txn.get(), repl::LastVote{2, 2});

    lastVote = replCoordExternalState->loadLocalLastVoteDocument(txn.get());
    ASSERT_OK(lastVote.getStatus());
    ASSERT_EQ(lastVote.getValue().getTerm(), 2);
    ASSERT_EQ(lastVote.getValue().getCandidateIndex(), 1);
}

}  // namespace
}  // namespace mongo
