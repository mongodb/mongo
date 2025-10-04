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

#include "mongo/s/query/exec/cluster_client_cursor_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/query/exec/router_stage_mock.h"
#include "mongo/unittest/unittest.h"

#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class ClusterClientCursorImplTest : public ServiceContextTest {
protected:
    ClusterClientCursorImplTest() {
        _opCtx = makeOperationContext();
    }

    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ClusterClientCursorImplTest, NumReturnedSoFar) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    for (int i = 1; i < 10; ++i) {
        mockStage->queueResult(BSON("a" << i));
    }

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 0);

    for (int i = 1; i < 10; ++i) {
        auto result = cursor.next();
        ASSERT(result.isOK());
        ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << i));
        ASSERT_EQ(cursor.getNumReturnedSoFar(), i);
    }
    // Now check that if nothing is fetched the getNumReturnedSoFar stays the same.
    auto result = cursor.next();
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEOF());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 9LL);
}

TEST_F(ClusterClientCursorImplTest, QueueResult) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 4));

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));

    cursor.queueResult(BSON("a" << 2));
    cursor.queueResult(BSON("a" << 3));

    auto secondResult = cursor.next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2));

    auto thirdResult = cursor.next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*thirdResult.getValue().getResult(), BSON("a" << 3));

    auto fourthResult = cursor.next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(fourthResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*fourthResult.getValue().getResult(), BSON("a" << 4));

    auto fifthResult = cursor.next();
    ASSERT_OK(fifthResult.getStatus());
    ASSERT(fifthResult.getValue().isEOF());

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 4LL);
}

TEST_F(ClusterClientCursorImplTest, RemotesExhausted) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueResult(BSON("a" << 2));
    mockStage->markRemotesExhausted();

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
    ASSERT_TRUE(cursor.remotesExhausted());

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto secondResult = cursor.next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*secondResult.getValue().getResult(), BSON("a" << 2));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto thirdResult = cursor.next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT_TRUE(thirdResult.getValue().isEOF());
    ASSERT_TRUE(cursor.remotesExhausted());

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 2LL);
}

TEST_F(ClusterClientCursorImplTest, RemoteTimeoutPartialResultsDisallowed) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::MaxTimeMSExpired, "timeout"));
    mockStage->markRemotesExhausted();

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
    ASSERT_TRUE(cursor.remotesExhausted());

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto thirdResult = cursor.next();
    ASSERT_EQ(thirdResult.getStatus().code(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_TRUE(cursor.remotesExhausted());
    ASSERT_FALSE(cursor.partialResultsReturned());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 1LL);
}

TEST_F(ClusterClientCursorImplTest, RemoteTimeoutPartialResultsAllowed) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    mockStage->queueResult(BSON("a" << 1));
    mockStage->queueError(Status(ErrorCodes::MaxTimeMSExpired, "timeout"));
    mockStage->markRemotesExhausted();

    auto params =
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient());
    params.isAllowPartialResults = true;

    ClusterClientCursorImpl cursor(
        _opCtx.get(), std::move(mockStage), std::move(params), boost::none);
    ASSERT_TRUE(cursor.remotesExhausted());

    auto firstResult = cursor.next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue().getResult());
    ASSERT_BSONOBJ_EQ(*firstResult.getValue().getResult(), BSON("a" << 1));
    ASSERT_TRUE(cursor.remotesExhausted());

    auto thirdResult = cursor.next();
    ASSERT_EQ(thirdResult.getStatus().code(), ErrorCodes::MaxTimeMSExpired);
    ASSERT_TRUE(cursor.remotesExhausted());
    ASSERT_TRUE(cursor.partialResultsReturned());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 1LL);
}

TEST_F(ClusterClientCursorImplTest, ForwardsAwaitDataTimeout) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);
    ASSERT_OK(cursor.setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

TEST_F(ClusterClientCursorImplTest, ChecksForInterrupt) {
    auto mockStage = std::make_unique<RouterStageMock>(nullptr);
    for (int i = 1; i < 2; ++i) {
        mockStage->queueResult(BSON("a" << i));
    }

    ClusterClientCursorImpl cursor(
        _opCtx.get(),
        std::move(mockStage),
        ClusterClientCursorParams(NamespaceString::createNamespaceString_forTest("unused"),
                                  APIParameters(),
                                  boost::none /* ReadPreferenceSetting */,
                                  boost::none /* repl::ReadConcernArgs */,
                                  OperationSessionInfoFromClient()),
        boost::none);

    // Pull one result out of the cursor.
    auto result = cursor.next();
    ASSERT(result.isOK());
    ASSERT_BSONOBJ_EQ(*result.getValue().getResult(), BSON("a" << 1));

    // Now interrupt the opCtx which the cursor is running under.
    {
        stdx::lock_guard<Client> lk(*_opCtx->getClient());
        _opCtx->markKilled(ErrorCodes::CursorKilled);
    }

    // Now check that a subsequent call to next() will fail.
    result = cursor.next();
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus(), ErrorCodes::CursorKilled);
}

TEST_F(ClusterClientCursorImplTest, LogicalSessionIdsOnCursors) {
    // Make a cursor with no lsid
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());
    ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                     APIParameters(),
                                     boost::none /* ReadPreferenceSetting */,
                                     boost::none /* repl::ReadConcernArgs */,
                                     OperationSessionInfoFromClient());
    ClusterClientCursorImpl cursor{
        _opCtx.get(), std::move(mockStage), std::move(params), boost::none};
    ASSERT(!cursor.getLsid());

    // Make a cursor with an lsid
    auto mockStage2 = std::make_unique<RouterStageMock>(_opCtx.get());
    ClusterClientCursorParams params2(NamespaceString::createNamespaceString_forTest("test"),
                                      APIParameters(),
                                      boost::none /* ReadPreferenceSetting */,
                                      boost::none /* repl::ReadConcernArgs */,
                                      OperationSessionInfoFromClient());
    auto lsid = makeLogicalSessionIdForTest();
    ClusterClientCursorImpl cursor2{_opCtx.get(), std::move(mockStage2), std::move(params2), lsid};
    ASSERT(*(cursor2.getLsid()) == lsid);
}

TEST_F(ClusterClientCursorImplTest, ShouldStoreLSIDIfSetOnOpCtx) {
    std::shared_ptr<executor::TaskExecutor> nullExecutor;

    {
        // Make a cursor with no lsid or txnNumber.
        ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                         APIParameters(),
                                         boost::none /* ReadPreferenceSetting */,
                                         boost::none /* repl::ReadConcernArgs */,
                                         [&] {
                                             if (!_opCtx->getLogicalSessionId())
                                                 return OperationSessionInfoFromClient();
                                             return OperationSessionInfoFromClient{
                                                 *_opCtx->getLogicalSessionId(),
                                                 _opCtx->getTxnNumber()};
                                         }());

        auto cursor = ClusterClientCursorImpl::make(_opCtx.get(), nullExecutor, std::move(params));
        ASSERT_FALSE(cursor->getLsid());
        ASSERT_FALSE(cursor->getTxnNumber());
    }

    const auto lsid = makeLogicalSessionIdForTest();
    _opCtx->setLogicalSessionId(lsid);

    {
        // Make a cursor with an lsid and no txnNumber.
        ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                         APIParameters(),
                                         boost::none /* ReadPreferenceSetting */,
                                         boost::none /* repl::ReadConcernArgs */,
                                         [&] {
                                             if (!_opCtx->getLogicalSessionId())
                                                 return OperationSessionInfoFromClient();
                                             return OperationSessionInfoFromClient{
                                                 *_opCtx->getLogicalSessionId(),
                                                 _opCtx->getTxnNumber()};
                                         }());

        auto cursor = ClusterClientCursorImpl::make(_opCtx.get(), nullExecutor, std::move(params));
        ASSERT_EQ(*cursor->getLsid(), lsid);
        ASSERT_FALSE(cursor->getTxnNumber());
    }

    const TxnNumber txnNumber = 5;
    _opCtx->setTxnNumber(txnNumber);

    {
        // Make a cursor with an lsid and txnNumber.
        ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                         APIParameters(),
                                         boost::none /* ReadPreferenceSetting */,
                                         boost::none /* repl::ReadConcernArgs */,
                                         [&] {
                                             if (!_opCtx->getLogicalSessionId())
                                                 return OperationSessionInfoFromClient();
                                             return OperationSessionInfoFromClient{
                                                 *_opCtx->getLogicalSessionId(),
                                                 _opCtx->getTxnNumber()};
                                         }());

        auto cursor = ClusterClientCursorImpl::make(_opCtx.get(), nullExecutor, std::move(params));
        ASSERT_EQ(*cursor->getLsid(), lsid);
        ASSERT_EQ(*cursor->getTxnNumber(), txnNumber);
    }
}

TEST_F(ClusterClientCursorImplTest, ShouldStoreAPIParameters) {
    auto mockStage = std::make_unique<RouterStageMock>(_opCtx.get());

    APIParameters apiParams = APIParameters();
    apiParams.setAPIVersion("2");
    apiParams.setAPIStrict(true);
    apiParams.setAPIDeprecationErrors(true);

    ClusterClientCursorParams params(NamespaceString::createNamespaceString_forTest("test"),
                                     apiParams,
                                     boost::none /* ReadPreferenceSetting */,
                                     boost::none /* repl::ReadConcernArgs */,
                                     OperationSessionInfoFromClient());
    ClusterClientCursorImpl cursor(
        _opCtx.get(), std::move(mockStage), std::move(params), boost::none);

    auto storedAPIParams = cursor.getAPIParameters();
    ASSERT_EQ("2", *storedAPIParams.getAPIVersion());
    ASSERT_TRUE(*storedAPIParams.getAPIStrict());
    ASSERT_TRUE(*storedAPIParams.getAPIDeprecationErrors());
}

}  // namespace
}  // namespace mongo
