// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/query/exec/store_possible_cursor.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/query/exec/cluster_cursor_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <vector>

#include <boost/none.hpp>

namespace mongo {
namespace {

const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collection");
const HostAndPort hostAndPort("testhost", 27017);
const ShardId shardId("testshard");

BSONObj makeCursorResponseObj(CursorId cursorId,
                              const std::vector<BSONObj>& batch,
                              bool partialResultsReturned) {
    rpc::OpMsgReplyBuilder builder;
    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    CursorResponseBuilder responseBuilder(&builder, options);
    for (const auto& doc : batch) {
        responseBuilder.append(doc);
    }
    responseBuilder.setPartialResultsReturned(partialResultsReturned);
    responseBuilder.done(cursorId, nss, boost::none, SerializationContext::stateCommandReply());
    auto msg = builder.done();
    auto opMsg = OpMsg::parse(msg);
    auto body = opMsg.body.getOwned();
    auto okStatus = BSON("ok" << 1);
    return body.addField(okStatus.firstElement());
}

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

TEST_F(StorePossibleCursorTest, PreservesPartialResultsReturnedForExhaustedCursorResponse) {
    std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
    auto cursorResponseObj =
        makeCursorResponseObj(CursorId(0), batch, true /* partialResultsReturned */);

    auto outgoingCursorResponse = storePossibleCursor(opCtx(),
                                                      shardId,
                                                      hostAndPort,
                                                      cursorResponseObj,
                                                      nss,
                                                      nullptr,  // TaskExecutor
                                                      getManager(),
                                                      PrivilegeVector());
    ASSERT_OK(outgoingCursorResponse.getStatus());

    auto parsedOutgoingResponse = CursorResponse::parseFromBSON(outgoingCursorResponse.getValue());
    ASSERT_OK(parsedOutgoingResponse.getStatus());
    ASSERT_TRUE(parsedOutgoingResponse.getValue().getPartialResultsReturned());
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
