/**
 *    Copyright (C) 2018 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/operation_context_session_mongod.h"
#include "mongo/db/session_catalog.h"
#include "mongo/db/transaction_coordinator_commands_impl.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const std::vector<ShardId> shardIds{{"s1"}, {"s2"}, {"s3"}};
const int kMaxNumFailedHostRetryAttempts = 3;

HostAndPort makeHostAndPort(const ShardId& shardId) {
    return HostAndPort(str::stream() << shardId << ":123");
}

/**
 * This test fixture allows testing the behavior of a shard server acting as a transaction
 * coordinator. The fixture provides:
 *
 * - helpers that simulate running voteAbortTransaction, voteCommitTransaction, and
 *   coordinateCommitTransaction
 * - methods to expect the coordinator to send commitTransaction and abortTransaction over the
 *   network and respond with success or error.
 */
class TransactionCoordinatorTestFixture : public ShardServerTestFixture {
protected:
    void setUp() final {
        ShardServerTestFixture::setUp();

        SessionCatalog::get(getServiceContext())->onStepUp(operationContext());
        auto scopedSession =
            SessionCatalog::get(operationContext())->getOrCreateSession(operationContext(), _lsid);
        TransactionCoordinator::create(scopedSession.get());

        for_each(shardIds.begin(), shardIds.end(), [this](const ShardId& shardId) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        });
    }

    void tearDown() final {
        SessionCatalog::get(getServiceContext())->reset_forTest();
        ShardServerTestFixture::tearDown();
    }

    /**
     * This is a generic helper that simulates that this shard server received a command by
     * launching a thread to run the command. The 'commandBody' argument should be a function that
     * simulates the command's run().
     */
    template <typename Lambda>
    auto simulateHandleRequest(Lambda commandBody) {
        auto future = launchAsync([this, commandBody] {
            try {
                // Set up OperationContext for this thread.
                ON_BLOCK_EXIT([&] { Client::destroy(); });
                Client::initThreadIfNotAlready();
                auto opCtxPtr = cc().makeOperationContext();
                auto opCtx = opCtxPtr.get();

                // Required in order for OperationContextSession to check out the session.
                opCtx->setLogicalSessionId(this->_lsid);

                // Check out the session.
                OperationContextSession ocs(opCtx, true);
                auto session = OperationContextSession::get(opCtx);
                invariant(session);

                // Call the command's "run".
                commandBody(opCtx);
            } catch (DBException& e) {
                log() << "Caught exception while running command: " << e.toStatus();
                MONGO_UNREACHABLE;
            }
        });
        return future;
    }

    auto receiveCoordinateCommit(std::set<ShardId> participantList) {
        auto commandFn =
            std::bind(txn::recvCoordinateCommit, std::placeholders::_1, participantList);
        return simulateHandleRequest(commandFn);
    }

    auto receiveVoteCommit(ShardId shardId, int prepareTimestamp) {
        auto commandFn =
            std::bind(txn::recvVoteCommit, std::placeholders::_1, shardId, prepareTimestamp);
        return simulateHandleRequest(commandFn);
    }

    auto receiveVoteAbort(ShardId shardId) {
        auto commandFn = std::bind(txn::recvVoteAbort, std::placeholders::_1, shardId);
        return simulateHandleRequest(commandFn);
    }

    void expectSendAbortAndReturnRetryableErrror() {
        for (int i = 0; i <= kMaxNumFailedHostRetryAttempts; i++) {
            onCommand([](const executor::RemoteCommandRequest& request) -> Status {
                ASSERT_EQUALS("abortTransaction",
                              request.cmdObj.firstElement().fieldNameStringData());
                return {ErrorCodes::HostUnreachable, ""};
            });
        }
    }

    void expectSendAbortAndReturnSuccess() {
        onCommand([](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS("abortTransaction", request.cmdObj.firstElement().fieldNameStringData());
            return BSON("ok" << 1);
        });
    }

    void expectSendCommitAndReturnRetryableError() {
        for (int i = 0; i <= kMaxNumFailedHostRetryAttempts; i++) {
            onCommand([](const executor::RemoteCommandRequest& request) -> Status {
                ASSERT_EQUALS("commitTransaction",
                              request.cmdObj.firstElement().fieldNameStringData());
                return {ErrorCodes::HostUnreachable, ""};
            });
        }
    }

    void expectSendCommitAndReturnSuccess() {
        onCommand([](const executor::RemoteCommandRequest& request) {
            ASSERT_EQUALS("commitTransaction", request.cmdObj.firstElement().fieldNameStringData());
            return BSON("ok" << 1);
        });
    }

private:
    // Override the CatalogClient to make CatalogClient::getAllShards automatically return the
    // expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
    // ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
    // DBClientMock analogous to the NetworkInterfaceMock.
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() : ShardingCatalogClientMock(nullptr) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for_each(shardIds.begin(), shardIds.end(), [&shardTypes](const ShardId& shardId) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                });
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }
        };
        return stdx::make_unique<StaticCatalogClient>();
    }

    const LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
};

//
// VoteCommit tests
//

TEST_F(TransactionCoordinatorTestFixture,
       VoteCommitDoesNotSendCommitIfParticipantListNotYetReceived) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentVoteCommitDoesNotSendCommitIfParticipantListNotYetReceived) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);
    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       VoteCommitDoesNotSendCommitIfSomeParticipantsNotYetVoted) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentVoteCommitDoesNotSendCommitIfSomeParticipantsNotYetVoted) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture, FinalVoteCommitSendsCommit) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[1], 0);
    expectSendCommitAndReturnSuccess();
    expectSendCommitAndReturnSuccess();
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentFinalVoteCommitOnlySendsCommitToNonAckedParticipants) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[1], 0);
    expectSendCommitAndReturnSuccess();
    expectSendCommitAndReturnRetryableError();
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[1], 0);
    expectSendCommitAndReturnSuccess();
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentFinalVoteCommitDoesNotSendCommitIfAllParticipantsAcked) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[1], 0);
    expectSendCommitAndReturnSuccess();
    expectSendCommitAndReturnSuccess();
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[1], 0);
    future.timed_get(kFutureTimeout);
}

//
// VoteAbort tests
//

TEST_F(TransactionCoordinatorTestFixture, VoteAbortDoesNotSendAbortIfIsOnlyVoteReceivedSoFar) {
    auto future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentVoteAbortDoesNotSendAbortIfIsOnlyVoteReceivedSoFar) {
    auto future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture, VoteAbortDoesNotSendAbortIfAllVotesSoFarWereToAbort) {
    auto future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[2]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentVoteAbortDoesNotSendAbortIfAllVotesSoFarWereToAbort) {
    auto future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[2]);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[2]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture, VoteAbortSendsAbortIfSomeParticipantsHaveVotedCommit) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    expectSendAbortAndReturnSuccess();
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       VoteAbortDoesNotSendAbortIfAlreadySentAbortToAllParticipantsWhoHaveVotedSoFar) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    expectSendAbortAndReturnSuccess();
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[2]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentVoteAbortDoesNotSendAbortIfAlreadySentAbortToAllParticipantsWhoHaveVotedSoFar) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    expectSendAbortAndReturnSuccess();
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentVoteAbortDoesNotSendAbortEvenIfMoreParticipantsHaveVotedCommit) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    expectSendAbortAndReturnSuccess();
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[2], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       VoteAbortAfterReceivingParticipantListSendsAbortToAllParticipantsWhoHaventVotedAbort) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1], shardIds[2]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveVoteAbort(shardIds[1]);
    expectSendAbortAndReturnSuccess();
    expectSendAbortAndReturnSuccess();
    future.timed_get(kFutureTimeout);
}

//
// CoordinateCommit tests
//

TEST_F(TransactionCoordinatorTestFixture, CoordinateCommitDoesNotSendCommitIfNoParticipantsVoted) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentCoordinateCommitDoesNotSendCommitIfNoParticipantsVoted) {
    auto future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       CoordinateCommitDoesNotSendCommitIfSomeParticipantsNotYetVoted) {
    auto future = receiveVoteCommit(shardIds[1], 0);
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentCoordinateCommitDoesNotSendCommitIfSomeParticipantsNotYetVoted) {
    auto future = receiveVoteCommit(shardIds[1], 0);
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       CoordinateCommitDoesNotSendAbortEvenIfSomeParticipantsVotedAbort) {
    auto future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentCoordinateCommitDoesNotSendAbortEvenIfSomeParticipantsVotedAbort) {
    auto future = receiveVoteAbort(shardIds[0]);
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

TEST_F(TransactionCoordinatorTestFixture,
       ResentCoordinateCommitDoesNotSendCommitEvenIfAllParticipantsAlreadyVotedCommit) {
    auto future = receiveVoteCommit(shardIds[0], 0);
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);

    future = receiveVoteCommit(shardIds[1], 0);
    expectSendCommitAndReturnSuccess();
    expectSendCommitAndReturnSuccess();
    future.timed_get(kFutureTimeout);

    future = receiveCoordinateCommit({shardIds[0], shardIds[1]});
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
