/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/transport/session_manager_common.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session_manager_common_mock.h"
#include "mongo/unittest/unittest.h"

#include <cerrno>

#include <sys/resource.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

class SessionManagerCommonTest : public ServiceContextTest {};

TEST_F(SessionManagerCommonTest, VerifyMaxOpenSessionsBasedOnRlimit) {
    struct rlimit originalLimit, newLimit;
    auto rlimitReturnCode = getrlimit(RLIMIT_NOFILE, &originalLimit);
    const auto savedErrno1 = errno;
    ASSERT_EQ(rlimitReturnCode, 0) << savedErrno1;

    ASSERT_GTE(originalLimit.rlim_max, 10);

    newLimit = originalLimit;
    newLimit.rlim_cur = 10;
    rlimitReturnCode = setrlimit(RLIMIT_NOFILE, &newLimit);
    const auto savedErrno2 = errno;
    ASSERT_EQ(rlimitReturnCode, 0) << savedErrno2;

    // 80% of half of 10 is 4, which is the arithmetic we want to verify in the
    // `getSupportedMax` function via the `maxOpenSessions` getter.
    MockSessionManagerCommon sm(getServiceContext());
    ASSERT_EQ(sm.maxOpenSessions(), 4);

    rlimitReturnCode = setrlimit(RLIMIT_NOFILE, &originalLimit);
    const auto savedErrno3 = errno;
    ASSERT_EQ(rlimitReturnCode, 0) << savedErrno3;
}

}  // namespace
}  // namespace mongo::transport
