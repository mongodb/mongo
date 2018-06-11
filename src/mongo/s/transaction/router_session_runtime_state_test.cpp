/**
 *    Copyright (C) 2018 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/transaction/router_session_runtime_state.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(RouterSessionRuntimeStateTest, BasicStartTxn) {
    TxnNumber txnNum{3};

    RouterSessionRuntimeState sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(txnNum, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "autocommit"
                                  << false);

    ShardId shard1("a");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit"
                               << false),
                          newCmd);
    }
}

TEST(RouterSessionRuntimeStateTest, CannotContiueTxnWithoutStarting) {
    TxnNumber txnNum{3};

    RouterSessionRuntimeState sessionState({});
    sessionState.checkOut();
    ASSERT_THROWS_CODE(sessionState.beginOrContinueTxn(txnNum, false),
                       AssertionException,
                       ErrorCodes::NoSuchTransaction);
}

TEST(RouterSessionRuntimeStateTest, NewParticipantMustAttachTxn) {
    TxnNumber txnNum{3};

    RouterSessionRuntimeState sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(txnNum, true);

    ShardId shard1("a");
    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "autocommit"
                                  << false);

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit"
                               << false),
                          newCmd);
    }

    ShardId shard2("b");
    {
        auto& participant = sessionState.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
        participant.markAsCommandSent();
    }

    {
        auto& participant = sessionState.getOrCreateParticipant(shard2);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit"
                               << false),
                          newCmd);
    }
}

TEST(RouterSessionRuntimeStateTest, StartingNewTxnShouldClearState) {
    TxnNumber txnNum{3};

    RouterSessionRuntimeState sessionState({});
    sessionState.checkOut();
    sessionState.beginOrContinueTxn(txnNum, true);

    ShardId shard1("a");

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        participant.markAsCommandSent();
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("update"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(BSON("update"
                               << "test"
                               << "autocommit"
                               << false),
                          newCmd);
    }

    TxnNumber txnNum2{5};
    sessionState.beginOrContinueTxn(txnNum2, true);

    BSONObj expectedNewObj = BSON("insert"
                                  << "test"
                                  << "startTransaction"
                                  << true
                                  << "autocommit"
                                  << false);

    {
        auto& participant = sessionState.getOrCreateParticipant(shard1);
        auto newCmd = participant.attachTxnFieldsIfNeeded(BSON("insert"
                                                               << "test"));
        ASSERT_BSONOBJ_EQ(expectedNewObj, newCmd);
    }
}

}  // unnamed namespace
}  // namespace mongo
