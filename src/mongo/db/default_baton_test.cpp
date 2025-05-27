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
