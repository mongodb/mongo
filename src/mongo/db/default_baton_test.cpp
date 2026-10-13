// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/default_baton.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

// TODO: SERVER-50135 Test the rest of the behavior of DefaultBaton
class DefaultBatonTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtx = cc().makeOperationContext();
    }

    DefaultBaton& getBaton() {
        auto baton = _opCtx->getBaton();
        invariant(baton);

        auto defaultBaton = dynamic_cast<DefaultBaton*>(baton.get());
        invariant(defaultBaton);
        return *defaultBaton;
    }

    OperationContext* getOpCtx() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(DefaultBatonTest, WaitUntil) {
    auto deadline = getServiceContext()->getFastClockSource()->now() + Milliseconds(50);
    auto fut = getBaton().waitUntil(deadline, CancellationToken::uncancelable());
    ASSERT_OK(fut.getNoThrow(getOpCtx()));
    ASSERT_GTE(getServiceContext()->getFastClockSource()->now(), deadline);
}

TEST_F(DefaultBatonTest, CancelTimer) {
    CancellationSource source;

    auto deadline = getServiceContext()->getFastClockSource()->now() + Seconds(10);
    auto fut = getBaton().waitUntil(deadline, source.token());
    source.cancel();
    ASSERT_THROWS_CODE(fut.get(getOpCtx()), DBException, ErrorCodes::CallbackCanceled);
    ASSERT_LTE(getServiceContext()->getFastClockSource()->now(), deadline);
}

TEST_F(DefaultBatonTest, ReentrantRunFromRunnerThreadIsAllowed) {
    auto& baton = getBaton();
    auto* clkSource = getServiceContext()->getFastClockSource();

    bool reentered = false;
    baton.schedule([&](Status) {
        // A nested run_until() from the runner thread itself (e.g., from within a scheduled job)
        // must remain allowed.
        baton.run_until(clkSource, clkSource->now() + Milliseconds(1));
        reentered = true;
    });

    baton.run(clkSource);
    ASSERT_TRUE(reentered);
}

using DefaultBatonDeathTest = DefaultBatonTest;
DEATH_TEST_F(DefaultBatonDeathTest, ConcurrentRunners, "Detected concurrent runners") {
    auto& baton = getBaton();
    auto* clkSource = getServiceContext()->getFastClockSource();

    auto work = [&] {
        baton.run_until(clkSource, clkSource->now() + Minutes(10));
    };
    unittest::JoinThread runner(work);
    work();
}

}  // namespace
}  // namespace mongo
