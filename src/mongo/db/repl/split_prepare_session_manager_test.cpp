/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/split_prepare_session_manager.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/internal_session_pool.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

class SplitPrepareSessionManagerTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;
        auto sessPool = InternalSessionPool::get(getServiceContext());
        _splitSessManager = std::make_unique<SplitPrepareSessionManager>(sessPool);
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

protected:
    std::unique_ptr<SplitPrepareSessionManager> _splitSessManager;
};

TEST_F(SplitPrepareSessionManagerTest, SplitSessionsBasic) {
    const auto& topLevelSessId1 = makeSystemLogicalSessionId();
    const auto& topLevelSessId2 = makeSystemLogicalSessionId();
    const TxnNumber txnNumber1(100), txnNumber2(200);
    const int numSplits1 = 1, numSplits2 = 5;

    const auto& sessions1 =
        _splitSessManager->splitSession(topLevelSessId1, txnNumber1, numSplits1);
    const auto& sessions2 =
        _splitSessManager->splitSession(topLevelSessId2, txnNumber2, numSplits2);

    ASSERT_EQ(numSplits1, sessions1.size());
    ASSERT_EQ(numSplits2, sessions2.size());

    std::vector<LogicalSessionId> sessionIds1(sessions1.size()), sessionIds2(sessions2.size());
    auto sessionToId = [](const PooledSession& session) { return session.getSessionId(); };
    std::transform(sessions1.begin(), sessions1.end(), sessionIds1.begin(), sessionToId);
    std::transform(sessions2.begin(), sessions2.end(), sessionIds2.begin(), sessionToId);

    // Make sure the split sessions have unique sessionIds.
    ASSERT_EQ(numSplits1, LogicalSessionIdSet(sessionIds1.begin(), sessionIds1.end()).size());
    ASSERT_EQ(numSplits2, LogicalSessionIdSet(sessionIds2.begin(), sessionIds2.end()).size());

    ASSERT_EQ(numSplits1, _splitSessManager->getSplitSessions(topLevelSessId1, txnNumber1)->size());
    ASSERT_EQ(numSplits2, _splitSessManager->getSplitSessions(topLevelSessId2, txnNumber2)->size());
    ASSERT_EQ(true, _splitSessManager->isSessionSplit(topLevelSessId1, txnNumber1));
    ASSERT_EQ(true, _splitSessManager->isSessionSplit(topLevelSessId2, txnNumber2));

    _splitSessManager->releaseSplitSessions(topLevelSessId1, txnNumber1);
    _splitSessManager->releaseSplitSessions(topLevelSessId2, txnNumber2);

    ASSERT_EQ(boost::none, _splitSessManager->getSplitSessions(topLevelSessId1, txnNumber1));
    ASSERT_EQ(boost::none, _splitSessManager->getSplitSessions(topLevelSessId2, txnNumber2));
    ASSERT_EQ(false, _splitSessManager->isSessionSplit(topLevelSessId1, txnNumber1));
    ASSERT_EQ(false, _splitSessManager->isSessionSplit(topLevelSessId2, txnNumber2));
}

DEATH_TEST_F(SplitPrepareSessionManagerTest, SplitAlreadySplitSessions, "invariant") {
    const auto& topLevelSessId = makeSystemLogicalSessionId();
    const TxnNumber txnNumber(100);
    const int numSplits = 3;

    const auto& sessions = _splitSessManager->splitSession(topLevelSessId, txnNumber, numSplits);
    ASSERT_EQ(numSplits, sessions.size());

    // Attempting to split an already split top-level session should fail.
    _splitSessManager->splitSession(topLevelSessId, txnNumber, numSplits + 1);
}

DEATH_TEST_F(SplitPrepareSessionManagerTest, ReleaseNonSplitSessions, "invariant") {
    const auto& topLevelSessId = makeSystemLogicalSessionId();

    // Attempting to release a non-split top-level session should fail.
    _splitSessManager->releaseSplitSessions(topLevelSessId, TxnNumber(100));
}

DEATH_TEST_F(SplitPrepareSessionManagerTest, ChangeTxnNumberAfterSessionSplit, "invariant") {
    const auto& topLevelSessId = makeSystemLogicalSessionId();
    const TxnNumber txnNumber(100);
    const int numSplits = 3;

    const auto& sessions = _splitSessManager->splitSession(topLevelSessId, txnNumber, numSplits);
    ASSERT_EQ(numSplits, sessions.size());

    // Attempting to release a top-level session with different txnNumber should fail.
    _splitSessManager->releaseSplitSessions(topLevelSessId, txnNumber + 1);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
