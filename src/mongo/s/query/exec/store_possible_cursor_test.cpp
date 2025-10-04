/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/query/exec/store_possible_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <vector>

namespace mongo {
namespace {

const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collection");
const HostAndPort hostAndPort("testhost", 27017);
const ShardId shardId("testshard");

class StorePossibleCursorTest : public ServiceContextTest {
protected:
    StorePossibleCursorTest() : _opCtx(makeOperationContext()), _manager(&_clockSourceMock) {}

    OperationContext* opCtx() const {
        return _opCtx.get();
    }

    ClusterCursorManager* getManager() {
        return &_manager;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;

    ClockSourceMock _clockSourceMock;
    ClusterCursorManager _manager;
};

// Test that storePossibleCursor() returns a valid cursor response document.
TEST_F(StorePossibleCursorTest, ReturnsValidCursorResponse) {
    std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
    CursorResponse cursorResponse(nss, CursorId(0), batch);
    auto outgoingCursorResponse =
        storePossibleCursor(opCtx(),
                            shardId,
                            hostAndPort,
                            cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse),
                            nss,
                            nullptr,  // TaskExecutor
                            getManager(),
                            PrivilegeVector());
    ASSERT_OK(outgoingCursorResponse.getStatus());

    auto parsedOutgoingResponse = CursorResponse::parseFromBSON(outgoingCursorResponse.getValue());
    ASSERT_OK(parsedOutgoingResponse.getStatus());
    ASSERT_EQ(nss.toString_forTest(),
              parsedOutgoingResponse.getValue().getNSS().toString_forTest());
    ASSERT_EQ(0U, parsedOutgoingResponse.getValue().getCursorId());
    ASSERT_EQ(2U, parsedOutgoingResponse.getValue().getBatch().size());
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 1}"), parsedOutgoingResponse.getValue().getBatch()[0]);
    ASSERT_BSONOBJ_EQ(fromjson("{_id: 2}"), parsedOutgoingResponse.getValue().getBatch()[1]);
}

// Test that storePossibleCursor() propagates an error if it cannot parse the cursor response.
TEST_F(StorePossibleCursorTest, FailsGracefullyOnBadCursorResponseDocument) {
    auto outgoingCursorResponse = storePossibleCursor(opCtx(),
                                                      shardId,
                                                      hostAndPort,
                                                      fromjson("{ok: 1, cursor: {}}"),
                                                      nss,
                                                      nullptr,  // TaskExecutor
                                                      getManager(),
                                                      PrivilegeVector());
    ASSERT_NOT_OK(outgoingCursorResponse.getStatus());
    ASSERT_EQ(ErrorCodes::IDLFailedToParse, outgoingCursorResponse.getStatus());
}

// Test that storePossibleCursor() passes up the command response if it is not recognized as a
// cursor response.
TEST_F(StorePossibleCursorTest, PassesUpCommandResultIfItDoesNotDescribeACursor) {
    BSONObj notACursorObj = BSON("not" << "cursor");
    auto outgoingCursorResponse = storePossibleCursor(opCtx(),
                                                      shardId,
                                                      hostAndPort,
                                                      notACursorObj,
                                                      nss,
                                                      nullptr,  // TaskExecutor
                                                      getManager(),
                                                      PrivilegeVector());
    ASSERT_OK(outgoingCursorResponse.getStatus());
    ASSERT_BSONOBJ_EQ(notACursorObj, outgoingCursorResponse.getValue());
}

}  // namespace
}  // namespace mongo
