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

#include "mongo/platform/basic.h"

#include <deque>

#include "mongo/db/transaction_coordinator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using ParticipantList = TransactionCoordinator::ParticipantList;

TEST(ParticipantList, ReceiveSameParticipantListMultipleTimesSucceeds) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
}

TEST(ParticipantList, ReceiveConflictingParticipantListsNoOverlapThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0002"), ShardId("shard0003")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveConflictingParticipantListsFirstListIsSupersetOfSecondThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0000")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveConflictingParticipantListsFirstListIsSubsetOfSecondThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000"), ShardId("shard0001")});
    ASSERT_THROWS_CODE(participantList.recordFullList(
                           {ShardId("shard0000"), ShardId("shard0001"), ShardId("shard0002")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveVoteAbortFromParticipantNotInListThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000")});
    ASSERT_THROWS_CODE(participantList.recordVoteAbort(ShardId("shard0001")),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveVoteCommitFromParticipantNotInListThrows) {
    ParticipantList participantList;
    participantList.recordFullList({ShardId("shard0000")});
    ASSERT_THROWS_CODE(participantList.recordVoteCommit(ShardId("shard0001"), 0),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveParticipantListMissingParticipantThatAlreadyVotedAbortThrows) {
    ParticipantList participantList;
    participantList.recordVoteAbort(ShardId("shard0000"));
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0001")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ReceiveParticipantListMissingParticipantThatAlreadyVotedCommitThrows) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), 0);
    ASSERT_THROWS_CODE(participantList.recordFullList({ShardId("shard0001")}),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ParticipantResendsVoteAbortSucceeds) {
    ParticipantList participantList;
    participantList.recordVoteAbort(ShardId("shard0001"));
    participantList.recordVoteAbort(ShardId("shard0001"));
}

TEST(ParticipantList, ParticipantResendsVoteCommitSucceeds) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), 0);
    participantList.recordVoteCommit(ShardId("shard0000"), 0);
}

TEST(ParticipantList, ParticipantChangesVoteFromAbortToCommitThrows) {
    ParticipantList participantList;
    participantList.recordVoteAbort(ShardId("shard0000"));
    ASSERT_THROWS_CODE(participantList.recordVoteCommit(ShardId("shard0000"), 0),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ParticipantChangesVoteFromCommitToAbortThrows) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), 0);
    ASSERT_THROWS_CODE(participantList.recordVoteAbort(ShardId("shard0000")),
                       AssertionException,
                       ErrorCodes::InternalError);
}

TEST(ParticipantList, ParticipantChangesPrepareTimestampThrows) {
    ParticipantList participantList;
    participantList.recordVoteCommit(ShardId("shard0000"), 0);
    ASSERT_THROWS_CODE(participantList.recordVoteCommit(ShardId("shard0000"), 1),
                       AssertionException,
                       ErrorCodes::InternalError);
}

}  // namespace mongo
