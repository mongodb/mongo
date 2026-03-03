/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/repl/auto_get_rstl_for_stepup_stepdown.h"

#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {

/**
 * Tests for AutoGetRstlForStepUpStepDown::OpsAndSessionsKiller.
 *
 * This test class uses ServiceContextMongoDTest to get a fully initialized service context with
 * storage infrastructure needed for the OpsAndSessionsKiller object.
 */
class OpsAndSessionsKillerTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
    }

    void tearDown() override {
        // End any prepare conflicts before destroying opCtxs.
        for (auto* opCtx : _prepareConflictOpCtxs) {
            auto& tracker = StorageExecutionContext::get(opCtx)->getPrepareConflictTracker();
            tracker.endPrepareConflict(*getServiceContext()->getTickSource());
        }
        _prepareConflictOpCtxs.clear();
        // Clear opCtxs before clients to ensure proper destruction order.
        _opCtxs.clear();
        _clients.clear();
        ServiceContextMongoDTest::tearDown();
    }

protected:
    /**
     * Creates a client and an associated operation context.
     */
    auto makeClientAndOpCtx(std::string clientName, bool killable = true) {
        auto client = getServiceContext()->getService()->makeClient(
            std::move(clientName), nullptr, ClientOperationKillableByStepdown{killable});
        auto opCtx = client->makeOperationContext();
        return make_pair(std::move(client), std::move(opCtx));
    }

    /**
     * Creates a client and an associated operation context, storing both internally
     * to keep it alive for the lifetime of the test.
     */
    OperationContext* makeOpCtx(std::string clientName, bool killable = true) {
        auto [client, opCtx] = makeClientAndOpCtx(std::move(clientName), killable);
        _clients.push_back(std::move(client));
        _opCtxs.push_back(std::move(opCtx));
        return _opCtxs.back().get();
    }

    /**
     * Creates a client and operation context that is marked to always
     * be interrupted during step up/step down.
     */
    OperationContext* makeInterruptibleOpCtx(std::string clientName, bool killable = true) {
        auto opCtx = makeOpCtx(std::move(clientName), killable);
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        return opCtx;
    }

    /**
     * Creates a client and operation context that is marked as a retryable write.
     * Retryable writes are killed during stepdown because they may have side effects
     * that need to be rolled back.
     */
    OperationContext* makeRetryableWriteOpCtx(std::string clientName, bool killable = true) {
        auto opCtx = makeOpCtx(std::move(clientName), killable);
        opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
        opCtx->setTxnNumber(1);
        ASSERT_TRUE(opCtx->isRetryableWrite());
        return opCtx;
    }

    /**
     * Creates a client and operation context that has taken a global lock in
     * a mode that conflicts with writes. The lock is acquired and released immediately, but the
     * wasGlobalLockTakenInModeConflictingWithWrites flag remains set.
     */
    OperationContext* makeGlobalLockConflictOpCtx(std::string clientName, bool killable = true) {
        auto opCtx = makeOpCtx(std::move(clientName), killable);
        {
            Lock::GlobalLock lock(opCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
            ASSERT_TRUE(lock.isLocked());
        }
        ASSERT_TRUE(
            shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites());
        return opCtx;
    }

    /**
     * Creates a client and operation context that is waiting on a prepare conflict.
     * The prepare conflict is ended during tearDown.
     */
    OperationContext* makePrepareConflictOpCtx(std::string clientName, bool killable = true) {
        auto opCtx = makeOpCtx(std::move(clientName), killable);
        auto& tracker = StorageExecutionContext::get(opCtx)->getPrepareConflictTracker();
        tracker.beginPrepareConflict(*getServiceContext()->getTickSource());
        ASSERT_TRUE(tracker.isWaitingOnPrepareConflict());
        tracker.updatePrepareConflict(*getServiceContext()->getTickSource());
        _prepareConflictOpCtxs.push_back(opCtx);
        return opCtx;
    }

    /**
     * Creates an OpsAndSessionsKiller object to be tested.
     */
    std::unique_ptr<OpsAndSessionsKiller> makeKiller(
        std::vector<const OperationContext*> opsToIgnore = {},
        ErrorCodes::Error killReason = ErrorCodes::InterruptedDueToReplStateChange) {
        return std::unique_ptr<OpsAndSessionsKiller>(new OpsAndSessionsKiller(
            getServiceContext(), killReason, std::move(opsToIgnore), Date_t::max()));
    }

private:
    // Stores clients to keep them alive for the lifetime of the test.
    std::vector<ServiceContext::UniqueClient> _clients;
    // Stores opCtxs to keep them alive for the lifetime of the test.
    std::vector<ServiceContext::UniqueOperationContext> _opCtxs;
    // Tracks opCtxs with prepare conflicts for cleanup.
    std::vector<OperationContext*> _prepareConflictOpCtxs;
};

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_NoOps) {
    auto killer = makeKiller();
    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
    ASSERT_EQ(killer->getTotalOpsKilled(), 0);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_DoesNotKillNormalOp) {
    auto killer = makeKiller();
    auto targetOpCtx = makeOpCtx("targetClient");

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 0);
    ASSERT_EQ(killer->getTotalOpsRunning(), 1);
    ASSERT_FALSE(targetOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_KillsAlwaysInterruptOp) {
    auto killer = makeKiller();
    auto targetOpCtx = makeInterruptibleOpCtx("targetClient");

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(targetOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_KillsOpWithGlobalLockConflictingWithWrites) {
    auto killer = makeKiller();
    auto targetOpCtx = makeGlobalLockConflictOpCtx("targetClient");

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(targetOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_KillsOpWaitingOnPrepareConflict) {
    auto killer = makeKiller();
    auto targetOpCtx = makePrepareConflictOpCtx("targetClient");

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(targetOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_KillsRetryableWrite) {
    auto killer = makeKiller();
    auto retryableWriteOpCtx = makeRetryableWriteOpCtx("retryableWriteClient");

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(retryableWriteOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_IgnoresOpsInIgnoreList) {
    auto targetOpCtx = makeInterruptibleOpCtx("targetClient");
    auto killer = makeKiller({targetOpCtx});

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 0);
    // To ignore ops are not counted.
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_FALSE(targetOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_KillsMultipleOps) {
    auto targetOpCtx1 = makeInterruptibleOpCtx("targetClient1");
    auto targetOpCtx2 = makeInterruptibleOpCtx("targetClient2");
    auto targetOpCtx3 = makeRetryableWriteOpCtx("targetClient3");
    auto targetOpCtx4 = makePrepareConflictOpCtx("targetClient4");
    auto targetOpCtx5 = makeGlobalLockConflictOpCtx("targetClient5");
    auto toIgnoreOpCtx = makeGlobalLockConflictOpCtx("toIgnore");
    auto notToKillOpCtx = makeOpCtx("notToKill");

    auto killReason = ErrorCodes::InterruptedDueToReplStateChange;
    auto killer = makeKiller({toIgnoreOpCtx}, killReason);

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 5);
    ASSERT_EQ(killer->getTotalOpsRunning(), 1);
    ASSERT_TRUE(targetOpCtx1->isKillPending());
    ASSERT_TRUE(targetOpCtx2->isKillPending());
    ASSERT_TRUE(targetOpCtx3->isKillPending());
    ASSERT_TRUE(targetOpCtx4->isKillPending());
    ASSERT_TRUE(targetOpCtx5->isKillPending());
    ASSERT_EQ(targetOpCtx1->getKillStatus(), killReason);
    ASSERT_EQ(targetOpCtx2->getKillStatus(), killReason);
    ASSERT_EQ(targetOpCtx3->getKillStatus(), killReason);
    ASSERT_EQ(targetOpCtx4->getKillStatus(), killReason);
    ASSERT_EQ(targetOpCtx5->getKillStatus(), killReason);
    ASSERT_FALSE(notToKillOpCtx->isKillPending());
    ASSERT_FALSE(toIgnoreOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_NonKillableOpNotKilled) {
    auto killer = makeKiller();
    auto nonKillableOpCtx = makeInterruptibleOpCtx("nonKillableClient", false /* killable */);

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();

    ASSERT_EQ(killer->getTotalOpsKilled(), 0);
    // Non-killable ops are not counted.
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_FALSE(nonKillableOpCtx->isKillPending());
}

using OpsAndSessionsKillerDeathTest = OpsAndSessionsKillerTest;
DEATH_TEST_F(OpsAndSessionsKillerDeathTest,
             killConflictingOps_NonKillableOpWithPreparedConflict,
             "9699100") {
    auto killer = makeKiller();
    makePrepareConflictOpCtx("nonKillableClient", false /* killable */);

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_SkipsAlreadyKillPending) {
    auto killer = makeKiller();
    auto targetOpCtx1 = makeInterruptibleOpCtx("targetClient1");

    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(targetOpCtx1->isKillPending());

    // Kill again - should not increment count since op is already pending kill
    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(targetOpCtx1->isKillPending());

    auto notToKillOpCtx = makeOpCtx("notToKill");
    auto targetOpCtx2 = makeInterruptibleOpCtx("targetClient2");

    // The new ops should be taken into account
    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
    ASSERT_EQ(killer->getTotalOpsKilled(), 2);
    ASSERT_EQ(killer->getTotalOpsRunning(), 1);
    ASSERT_TRUE(targetOpCtx1->isKillPending());
    ASSERT_TRUE(targetOpCtx2->isKillPending());
    ASSERT_FALSE(notToKillOpCtx->isKillPending());

    // Nothing should change
    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
    ASSERT_EQ(killer->getTotalOpsKilled(), 2);
    ASSERT_EQ(killer->getTotalOpsRunning(), 1);
    ASSERT_TRUE(targetOpCtx1->isKillPending());
    ASSERT_TRUE(targetOpCtx2->isKillPending());
    ASSERT_FALSE(notToKillOpCtx->isKillPending());
}

TEST_F(OpsAndSessionsKillerTest, killConflictingOps_OpsRunningIsReset) {
    auto killer = makeKiller();
    auto toBeKilledOpCtx = makeGlobalLockConflictOpCtx("toBeKilled");

    {
        auto [notToKillClient, notToKillOpCtx] = makeClientAndOpCtx("notToKill");

        killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
        ASSERT_EQ(killer->getTotalOpsKilled(), 1);
        ASSERT_EQ(killer->getTotalOpsRunning(), 1);
        ASSERT_TRUE(toBeKilledOpCtx->isKillPending());
        ASSERT_FALSE(notToKillOpCtx->isKillPending());

        // Nothing should change
        killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
        ASSERT_EQ(killer->getTotalOpsKilled(), 1);
        ASSERT_EQ(killer->getTotalOpsRunning(), 1);
        ASSERT_TRUE(toBeKilledOpCtx->isKillPending());
        ASSERT_FALSE(notToKillOpCtx->isKillPending());
    }

    // Total ops running should be reset to 0
    killer->killConflictingOpsAndSessionsOnStepUpAndStepDown();
    ASSERT_EQ(killer->getTotalOpsKilled(), 1);
    ASSERT_EQ(killer->getTotalOpsRunning(), 0);
    ASSERT_TRUE(toBeKilledOpCtx->isKillPending());
}

}  // namespace repl
}  // namespace mongo
