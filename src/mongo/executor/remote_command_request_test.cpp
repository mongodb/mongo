/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/executor/remote_command_request.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {
namespace {

const HostAndPort kTestTarget("localhost", 27017);
const DatabaseName kTestDb = DatabaseName::createDatabaseName_forTest(boost::none, "test");
const BSONObj kTestCmd = BSON("find" << "testcoll");

class RemoteCommandRequestDeadlineTest : public ServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        _client = getServiceContext()->getService()->makeClient("RemoteCommandRequestTest");
    }

    ServiceContext::UniqueClient _client;
};

// When no timeout and no opCtx deadline, deadline should be kNoDeadline.
TEST_F(RemoteCommandRequestDeadlineTest, NoTimeoutNoOpCtxDeadlineYieldsNoDeadline) {
    RemoteCommandRequest request(
        kTestTarget, kTestDb, kTestCmd, BSONObj(), nullptr, RemoteCommandRequest::kNoTimeout);
    ASSERT_EQ(request.deadline, RemoteCommandRequest::kNoDeadline);
    ASSERT_EQ(request.timeout, RemoteCommandRequest::kNoTimeout);
}

// When an explicit timeout is provided but no opCtx, deadline should be derived from timeout.
TEST_F(RemoteCommandRequestDeadlineTest, ExplicitTimeoutSetsDeadline) {
    auto beforeConstruction = Date_t::now();
    RemoteCommandRequest request(
        kTestTarget, kTestDb, kTestCmd, BSONObj(), nullptr, Milliseconds(5000));
    auto afterConstruction = Date_t::now();

    ASSERT_NE(request.deadline, RemoteCommandRequest::kNoDeadline);
    ASSERT_EQ(request.timeout, Milliseconds(5000));

    // Deadline should be approximately now + 5000ms (within the construction window).
    ASSERT_GTE(request.deadline, beforeConstruction + Milliseconds(5000));
    ASSERT_LTE(request.deadline, afterConstruction + Milliseconds(5000));
}

// When opCtx has a deadline shorter than explicit timeout, opCtx deadline wins.
TEST_F(RemoteCommandRequestDeadlineTest, OpCtxDeadlineShorterThanTimeout) {
    auto opCtx = _client->makeOperationContext();
    auto opCtxDeadline = Date_t::now() + Seconds(2);
    opCtx->setDeadlineByDate(opCtxDeadline, ErrorCodes::MaxTimeMSExpired);

    RemoteCommandRequest request(
        kTestTarget, kTestDb, kTestCmd, BSONObj(), opCtx.get(), Milliseconds(10000));

    // The opCtx deadline (2s) is shorter than the explicit timeout (10s), so it should win.
    ASSERT_EQ(request.deadline, opCtxDeadline);
    ASSERT_EQ(request.timeoutCode, ErrorCodes::MaxTimeMSExpired);
}

// When opCtx has a deadline longer than explicit timeout, explicit timeout wins.
TEST_F(RemoteCommandRequestDeadlineTest, ExplicitTimeoutShorterThanOpCtxDeadline) {
    auto opCtx = _client->makeOperationContext();
    opCtx->setDeadlineByDate(Date_t::now() + Seconds(30), ErrorCodes::MaxTimeMSExpired);

    auto beforeConstruction = Date_t::now();
    RemoteCommandRequest request(
        kTestTarget, kTestDb, kTestCmd, BSONObj(), opCtx.get(), Milliseconds(500));
    auto afterConstruction = Date_t::now();

    // The explicit timeout (500ms) is shorter than opCtx deadline (30s), so timeout wins.
    ASSERT_EQ(request.timeout, Milliseconds(500));
    // Deadline should be derived from the explicit timeout.
    ASSERT_GTE(request.deadline, beforeConstruction + Milliseconds(500));
    ASSERT_LTE(request.deadline, afterConstruction + Milliseconds(500));
}

// When opCtx has no deadline and no explicit timeout, kNoDeadline.
TEST_F(RemoteCommandRequestDeadlineTest, NoOpCtxDeadlineNoTimeoutYieldsNoDeadline) {
    auto opCtx = _client->makeOperationContext();
    RemoteCommandRequest request(kTestTarget, kTestDb, kTestCmd, BSONObj(), opCtx.get());

    ASSERT_EQ(request.deadline, RemoteCommandRequest::kNoDeadline);
    ASSERT_EQ(request.timeout, RemoteCommandRequest::kNoTimeout);
}

// When cmdObj contains maxTimeMSOpOnly, deadline should be derived from it.
TEST_F(RemoteCommandRequestDeadlineTest, MaxTimeMSOpOnlyInCmdObjSetsDeadline) {
    auto cmd = BSON("find" << "testcoll"
                           << "maxTimeMSOpOnly" << 3000);
    auto beforeConstruction = Date_t::now();
    RemoteCommandRequest request(kTestTarget, kTestDb, cmd, BSONObj(), nullptr);
    auto afterConstruction = Date_t::now();

    ASSERT_EQ(request.timeout, Milliseconds(3000));
    ASSERT_GTE(request.deadline, beforeConstruction + Milliseconds(3000));
    ASSERT_LTE(request.deadline, afterConstruction + Milliseconds(3000));
}

// When cmdObj maxTimeMSOpOnly is shorter than explicit timeout, maxTimeMSOpOnly wins.
TEST_F(RemoteCommandRequestDeadlineTest, MaxTimeMSOpOnlyShorterThanExplicitTimeout) {
    auto cmd = BSON("find" << "testcoll"
                           << "maxTimeMSOpOnly" << 1000);
    auto beforeConstruction = Date_t::now();
    RemoteCommandRequest request(kTestTarget, kTestDb, cmd, BSONObj(), nullptr, Milliseconds(5000));
    auto afterConstruction = Date_t::now();

    // maxTimeMSOpOnly (1000ms) is shorter than explicit timeout (5000ms), so it wins.
    ASSERT_EQ(request.timeout, Milliseconds(1000));
    ASSERT_GTE(request.deadline, beforeConstruction + Milliseconds(1000));
    ASSERT_LTE(request.deadline, afterConstruction + Milliseconds(1000));
}

// When cmdObj maxTimeMSOpOnly is longer than explicit timeout, explicit timeout wins.
TEST_F(RemoteCommandRequestDeadlineTest, MaxTimeMSOpOnlyLongerThanExplicitTimeout) {
    auto cmd = BSON("find" << "testcoll"
                           << "maxTimeMSOpOnly" << 10000);
    auto beforeConstruction = Date_t::now();
    RemoteCommandRequest request(kTestTarget, kTestDb, cmd, BSONObj(), nullptr, Milliseconds(2000));
    auto afterConstruction = Date_t::now();

    // Explicit timeout (2000ms) is shorter than maxTimeMSOpOnly (10000ms), so it wins.
    ASSERT_EQ(request.timeout, Milliseconds(2000));
    ASSERT_GTE(request.deadline, beforeConstruction + Milliseconds(2000));
    ASSERT_LTE(request.deadline, afterConstruction + Milliseconds(2000));
}

// OpCtx deadline shorter than maxTimeMSOpOnly in cmdObj, opCtx wins for deadline.
TEST_F(RemoteCommandRequestDeadlineTest, OpCtxDeadlineShorterThanMaxTimeMSOpOnly) {
    auto opCtx = _client->makeOperationContext();
    auto opCtxDeadline = Date_t::now() + Milliseconds(500);
    opCtx->setDeadlineByDate(opCtxDeadline, ErrorCodes::MaxTimeMSExpired);

    auto cmd = BSON("find" << "testcoll"
                           << "maxTimeMSOpOnly" << 5000);
    RemoteCommandRequest request(kTestTarget, kTestDb, cmd, BSONObj(), opCtx.get());

    // opCtx deadline (500ms from now) is shorter than maxTimeMSOpOnly (5000ms).
    ASSERT_EQ(request.deadline, opCtxDeadline);
    ASSERT_EQ(request.timeoutCode, ErrorCodes::MaxTimeMSExpired);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
