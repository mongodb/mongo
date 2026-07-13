// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/write_throttler.h"

#include "mongo/db/admission/write_throttler_admission_context.h"
#include "mongo/db/admission/write_throttler_parameters_gen.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

// Sets deterministic write-throttler server parameters for each test. The guards reset them on
// destruction so tests in the same binary do not interfere.
class WriteThrottlerTest : public ClockSourceMockServiceContextTest {
public:
    void setUp() override {
        ClockSourceMockServiceContextTest::setUp();
        _enabledGuard =
            std::make_unique<unittest::ServerParameterGuard>("writeThrottlerEnabled", true);
        _targetRateGuard = std::make_unique<unittest::ServerParameterGuard>(
            "writeThrottlerTargetRatePerSec", WriteThrottler::kMaxRate);
        _burstCapacityGuard = std::make_unique<unittest::ServerParameterGuard>(
            "writeThrottlerBurstCapacitySecs", 0.5);
        _maxQueueDepthGuard = std::make_unique<unittest::ServerParameterGuard>(
            "writeThrottlerMaxQueueDepth", 1000000LL);
        _maxCostPerOpGuard =
            std::make_unique<unittest::ServerParameterGuard>("writeThrottlerMaxCostPerOp", 0);
    }

    // Installs a fresh WriteThrottler on the test's service context and returns it. The
    // on_update handlers resolve the throttler via the ServiceContext decoration, so the
    // on_update-path tests need an installed instance.
    WriteThrottler* installThrottler() {
        auto throttler = std::make_unique<WriteThrottler>(getServiceContext()->getTickSource());
        auto* raw = throttler.get();
        WriteThrottler::set(getServiceContext(), std::move(throttler));
        return raw;
    }

    // Number of write-throttle admissions recorded for the given operation.
    static int32_t admissionsFor(OperationContext* opCtx) {
        return WriteThrottlerAdmissionContext::get(opCtx).getAdmissions();
    }

    static double tokenBalance(const WriteThrottler& throttler) {
        return throttler.tokenBalance_forTest();
    }

    void updateTargetRate(int targetRatePerSec) {
        _targetRateGuard.reset();
        _targetRateGuard = std::make_unique<unittest::ServerParameterGuard>(
            "writeThrottlerTargetRatePerSec", targetRatePerSec);
    }

    static void recordWriteCost(OperationContext* opCtx, int64_t docsWritten) {
        WriteThrottlerAdmissionContext::get(opCtx).recordWriteCostForReconciliation(docsWritten);
    }

    WriteThrottler* armForReconciliation() {
        _burstCapacityGuard.reset();
        _burstCapacityGuard = std::make_unique<unittest::ServerParameterGuard>(
            "writeThrottlerBurstCapacitySecs", 100.0);
        auto* throttler = installThrottler();
        updateTargetRate(1);
        return throttler;
    }

private:
    std::unique_ptr<unittest::ServerParameterGuard> _enabledGuard;
    std::unique_ptr<unittest::ServerParameterGuard> _targetRateGuard;
    std::unique_ptr<unittest::ServerParameterGuard> _burstCapacityGuard;
    std::unique_ptr<unittest::ServerParameterGuard> _maxQueueDepthGuard;
    std::unique_ptr<unittest::ServerParameterGuard> _maxCostPerOpGuard;
};

constexpr int kMaxRate = WriteThrottler::kMaxRate;

// ---- Mechanism: rate updates / operation admission ----

TEST_F(WriteThrottlerTest, ThrottlerIdleAdmitsImmediately) {
    auto* throttler = installThrottler();
    updateTargetRate(kMaxRate);
    // At kMaxRate admitOperation still forwards to the rate limiter (always-on, like ingress) and
    // admits immediately, recording the admission.
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), kMaxRate);

    auto opCtx = makeOperationContext();
    throttler->admitOperation(opCtx.get());
    ASSERT_EQ(admissionsFor(opCtx.get()), 1);
}

TEST_F(WriteThrottlerTest, ThrottlerActiveAdmitsOperation) {
    auto* throttler = installThrottler();
    // A rate below kMaxRate arms the throttler; admitOperation then goes through the token bucket
    // and (with burst capacity available) admits, recording the admission.
    updateTargetRate(1000);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1000);

    auto opCtx = makeOperationContext();
    throttler->admitOperation(opCtx.get());
    ASSERT_EQ(admissionsFor(opCtx.get()), 1);
}

// ---- Mechanism: generateSection observability ----

TEST_F(WriteThrottlerTest, ThrottlerGenerateSectionReportsObservations) {
    auto* throttler = installThrottler();
    updateTargetRate(1234);

    BSONObj section = throttler->generateSection();
    ASSERT_TRUE(section.hasField("enabled"));
    ASSERT_EQ(section.getIntField("targetRateLimit"), 1234);
    ASSERT_FALSE(section.hasField("queued"));
    ASSERT_FALSE(section.hasField("documentsCharged"));
    ASSERT_FALSE(section.hasField("tokensDebited"));
}

TEST_F(WriteThrottlerTest, ThrottlerGenerateSectionReportsOnUpdateTarget) {
    // The on_update path must keep the reported targetRateLimit in sync with the actual bucket
    // rate, which generateSection reads back via the rate limiter's refreshRate() accessor.
    auto* throttler = installThrottler();
    updateTargetRate(1234);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1234);
}

TEST_F(WriteThrottlerTest, OnUpdateTargetRatePerSecClampsAboveMaxRate) {
    // A target rate above kMaxRate is normalized to kMaxRate (idle) and reported as such, so an
    // out-of-band high value cannot arm the bucket above kMaxRate or report an odd targetRateLimit.
    auto* throttler = installThrottler();
    updateTargetRate(kMaxRate + 5);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), kMaxRate);
}

// ---- Batch-aware cost reconciliation (finalizeAdmission) ----

TEST_F(WriteThrottlerTest, FinalizeAdmissionDebitsExtraDocumentCost) {
    auto* throttler = armForReconciliation();

    auto opCtx = makeOperationContext();
    WriteThrottlerAdmissionContext::get(opCtx.get()).recordAdmission();
    recordWriteCost(opCtx.get(), 10);

    // 10 documents modified, 1 token already charged at admission -> 9 extra debited.
    const auto before = tokenBalance(*throttler);
    throttler->finalizeAdmission(opCtx.get());
    ASSERT_EQ(tokenBalance(*throttler), before - 9);
}

TEST_F(WriteThrottlerTest, FinalizeAdmissionSingleDocIsNoExtraDebit) {
    auto* throttler = armForReconciliation();

    auto opCtx = makeOperationContext();
    WriteThrottlerAdmissionContext::get(opCtx.get()).recordAdmission();
    recordWriteCost(opCtx.get(), 1);

    // Single-document op: docs == admissions -> no extra debit (parity with one token per op).
    const auto before = tokenBalance(*throttler);
    throttler->finalizeAdmission(opCtx.get());
    ASSERT_EQ(tokenBalance(*throttler), before);
}

TEST_F(WriteThrottlerTest, FinalizeAdmissionRespectsMaxCostPerOp) {
    unittest::ServerParameterGuard maxCost{"writeThrottlerMaxCostPerOp", 5};
    auto* throttler = armForReconciliation();

    auto opCtx = makeOperationContext();
    WriteThrottlerAdmissionContext::get(opCtx.get()).recordAdmission();
    recordWriteCost(opCtx.get(), 100);

    // 100 docs capped at 5 -> 4 extra tokens debited.
    const auto before = tokenBalance(*throttler);
    throttler->finalizeAdmission(opCtx.get());
    ASSERT_EQ(tokenBalance(*throttler), before - 4);
}

TEST_F(WriteThrottlerTest, FinalizeAdmissionIgnoresZeroDocumentWrites) {
    auto* throttler = armForReconciliation();

    auto opCtx = makeOperationContext();
    WriteThrottlerAdmissionContext::get(opCtx.get()).recordAdmission();
    recordWriteCost(opCtx.get(), 0);

    const auto before = tokenBalance(*throttler);
    throttler->finalizeAdmission(opCtx.get());
    ASSERT_EQ(tokenBalance(*throttler), before);
}

TEST_F(WriteThrottlerTest, WriteCostWithoutAdmissionIsIgnored) {
    auto* throttler = armForReconciliation();

    auto opCtx = makeOperationContext();
    recordWriteCost(opCtx.get(), 10);

    const auto before = tokenBalance(*throttler);
    throttler->finalizeAdmission(opCtx.get());
    ASSERT_EQ(tokenBalance(*throttler), before);
}

// ---- Batch-aware command-level accumulation (finalizeAdmission accounting) ----

TEST_F(WriteThrottlerTest, AdmissionContextAccumulatesWriteCost) {
    auto opCtx = makeOperationContext();
    auto& admCtx = WriteThrottlerAdmissionContext::get(opCtx.get());

    admCtx.recordWriteCostForReconciliation(10);
    ASSERT_EQ(admCtx.consumeWriteCostForReconciliation(), 0);

    admCtx.recordAdmission();
    admCtx.recordWriteCostForReconciliation(3);
    admCtx.recordWriteCostForReconciliation(4);
    admCtx.recordWriteCostForReconciliation(0);
    ASSERT_EQ(admCtx.consumeWriteCostForReconciliation(), 7);
    ASSERT_EQ(admCtx.consumeWriteCostForReconciliation(), 0);
}

TEST_F(WriteThrottlerTest, BatchedStatementsDebitTotalDocsMinusTotalAdmissions) {
    // Simulates a batched update: one command-level admission (delta=1 for the first statement,
    // each statement modifying 1 document. Command-level finalization must yield total extra tokens
    // debited = totalDocs - totalAdmissions = N - 1, not 0.
    auto* throttler = armForReconciliation();
    auto opCtx = makeOperationContext();
    WriteThrottlerAdmissionContext::get(opCtx.get()).recordAdmission();

    const int64_t kNumStatements = 5;
    const auto before = tokenBalance(*throttler);
    for (int64_t i = 0; i < kNumStatements; ++i) {
        recordWriteCost(opCtx.get(), 1);
    }
    throttler->finalizeAdmission(opCtx.get());
    ASSERT_EQ(tokenBalance(*throttler), before - (kNumStatements - 1));
}

// ---- Rate source: on_update path (mirrors ingress rate limiter) ----

TEST_F(WriteThrottlerTest, OnUpdateTargetRatePerSecAppliesRateWhenEnabled) {
    auto* throttler = installThrottler();
    // enabled (default in setUp) -> the handler pushes the new rate into the bucket, arming the
    // throttler.
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), kMaxRate);

    updateTargetRate(1000);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1000);
    auto opCtx = makeOperationContext();
    throttler->admitOperation(opCtx.get());
    ASSERT_EQ(admissionsFor(opCtx.get()), 1);
}

TEST_F(WriteThrottlerTest, OnUpdateTargetRatePerSecNoOpWhenDisabled) {
    auto* throttler = installThrottler();
    unittest::ServerParameterGuard disabled{"writeThrottlerEnabled", false};
    // Disabled -> the handler is inert; the throttler stays idle at kMaxRate.
    updateTargetRate(1000);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), kMaxRate);
}

TEST_F(WriteThrottlerTest, OnUpdateEnabledSyncsRate) {
    auto* throttler = installThrottler();
    updateTargetRate(500);

    // Toggling enabled=true pushes the current target rate into the bucket.
    ASSERT_OK(WriteThrottler::onUpdateEnabled(true));
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 500);

    // Toggling enabled=false disarms the bucket (kMaxRate).
    ASSERT_OK(WriteThrottler::onUpdateEnabled(false));
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), kMaxRate);
}

TEST_F(WriteThrottlerTest, OnUpdateBurstCapacitySecsPreservesRate) {
    auto* throttler = installThrottler();
    updateTargetRate(1000);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1000);

    // Changing burst capacity at runtime applies immediately without disturbing the current rate or
    // enabled state.
    ASSERT_OK(WriteThrottler::onUpdateBurstCapacitySecs(2.0));
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1000);
}

TEST_F(WriteThrottlerTest, OnUpdateMaxQueueDepthPreservesRate) {
    auto* throttler = installThrottler();
    updateTargetRate(1000);
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1000);

    // Changing the queue depth at runtime applies immediately without disturbing the current rate.
    ASSERT_OK(WriteThrottler::onUpdateMaxQueueDepth(50));
    ASSERT_EQ(throttler->generateSection().getIntField("targetRateLimit"), 1000);
}

}  // namespace
}  // namespace mongo
