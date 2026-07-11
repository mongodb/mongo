// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/default_baton.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

// TODO: SERVER-50135 Test the rest of the behavior of DefaultBaton
class DefaultBatonTest : public ServiceContextTest {
public:
    void setUp() override {
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

}  // namespace
}  // namespace mongo
