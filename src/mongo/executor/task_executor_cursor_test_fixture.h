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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/pinned_connection_task_executor_test_fixture.h"
#include "mongo/executor/task_executor_cursor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

/**
 * Defines two test fixtures for task executor cursors: one with pinned cursors and the other with
 * unpinned cursors. Both provide convenience methods to help with scheduling responses. Because
 * they define the same API, we can write tests to be run through both fixtures when applicable
 * (see task_executor_cursor_test.cpp for how this is executed in practice).
 * A Pinning TEC uses the same underlying transport session / network connection that the cursor was
 * opened on for all subsequent getMores.
 * A non-pinning TEC may send the initial find and subsequent getMores on different underlying
 * transport sessions/network connections.
 * There should be no behavior difference between the two, it's just a change of the underlying
 * implementation, which is why we want to run all the behavioral tests against both
 * implementations.
 */
namespace MONGO_MOD_PUB mongo {
namespace executor {
MONGO_MOD_FILE_PRIVATE inline BSONObj buildCursorResponse(StringData fieldName,
                                                          size_t start,
                                                          size_t end,
                                                          size_t cursorId) {
    BSONObjBuilder bob;
    {
        BSONObjBuilder cursor(bob.subobjStart("cursor"));
        {
            BSONArrayBuilder batch(cursor.subarrayStart(fieldName));

            for (size_t i = start; i <= end; ++i) {
                BSONObjBuilder doc(batch.subobjStart());
                doc.append("x", int(i));
            }
        }
        cursor.append("id", (long long)(cursorId));
        cursor.append("ns", "test.test");
    }
    bob.append("ok", int(1));
    return bob.obj();
}

MONGO_MOD_FILE_PRIVATE inline BSONObj buildMultiCursorResponse(StringData fieldName,
                                                               size_t start,
                                                               size_t end,
                                                               std::vector<size_t> cursorIds) {
    BSONObjBuilder bob;
    {
        BSONArrayBuilder cursors;
        int baseCursorValue = 1;
        for (auto cursorId : cursorIds) {
            BSONObjBuilder cursor;
            BSONArrayBuilder batch;
            ASSERT(start < end && end < INT_MAX);
            for (size_t i = start; i <= end; ++i) {
                batch.append(BSON("x" << static_cast<int>(i) * baseCursorValue).getOwned());
            }
            cursor.append(fieldName, batch.arr());
            cursor.append("id", (long long)(cursorId));
            cursor.append("ns", "test.test");
            auto cursorObj = BSON("cursor" << cursor.done() << "ok" << 1);
            cursors.append(cursorObj.getOwned());
            ++baseCursorValue;
        }
        bob.append("cursors", cursors.arr());
    }
    bob.append("ok", 1);
    return bob.obj();
}

class NonPinningTaskExecutorCursorTestFixture : public ThreadPoolExecutorTest {
public:
    void postSetUp() {
        launchExecutorThread();
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId,
                                             bool expectedPrefetch = true) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        if (expectedPrefetch) {
            ASSERT(getNet()->hasReadyRequests());
        }

        auto rcr = getNet()->scheduleSuccessfulResponse(
            buildCursorResponse(fieldName, start, end, cursorId));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds,
                                                  bool expectedPrefetch = true) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        if (expectedPrefetch) {
            ASSERT(getNet()->hasReadyRequests());
        }
        auto rcr = getNet()->scheduleSuccessfulResponse(
            buildMultiCursorResponse(fieldName, start, end, cursorIds));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId, bool expectedPrefetch = true) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        if (expectedPrefetch) {
            ASSERT(getNet()->hasReadyRequests());
        }
        auto rcr = getNet()->scheduleSuccessfulResponse(
            BSON("cursorsKilled" << BSON_ARRAY((long long)(cursorId)) << "cursorsNotFound"
                                 << BSONArray() << "cursorsAlive" << BSONArray() << "cursorsUnknown"
                                 << BSONArray() << "ok" << 1));
        getNet()->runReadyNetworkOperations();

        return rcr.cmdObj.getOwned();
    }

    void scheduleErrorResponse(Status error) {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());

        ASSERT(getNet()->hasReadyRequests());
        getNet()->scheduleErrorResponse(error);
        getNet()->runReadyNetworkOperations();
    }

    bool hasReadyRequests() {
        NetworkInterfaceMock::InNetworkGuard ing(getNet());
        return getNet()->hasReadyRequests();
    }

    /**
     * We provide this function to provide a uniform testing API for the pinning and non-pinning
     * test fixtures. This function should be used when we are expecting to receive a ready
     * request; unlike the pinning fixture though, this fixture receives requests synchronously, so
     * no wait is necessary.
     */
    bool tryWaitUntilReadyRequests() {
        return hasReadyRequests();
    }

    void blackHoleNextOutgoingRequest() {
        NetworkInterfaceMock::InNetworkGuard guard(getNet());
        getNet()->blackHole(getNet()->getFrontOfReadyQueue());
    }

    std::unique_ptr<TaskExecutorCursor> makeTec(RemoteCommandRequest rcr,
                                                TaskExecutorCursorOptions&& options) {
        options.pinConnection = false;
        return std::make_unique<TaskExecutorCursor>(getExecutorPtr(), rcr, std::move(options));
    }
};

class PinnedConnTaskExecutorCursorTestFixture : public PinnedConnectionTaskExecutorTest {
public:
    void postSetUp() {}

    BSONObj scheduleResponse(StatusWith<BSONObj> response) {
        int32_t responseToId = -1;
        BSONObj cmdObjReceived;
        auto pf = makePromiseFuture<void>();
        expectSinkMessage([&](Message m) {
            responseToId = m.header().getId();
            auto opMsg = OpMsgRequest::parse(m);
            cmdObjReceived = opMsg.body.removeField("$db").getOwned();
            pf.promise.emplaceValue();
            return Status::OK();
        });
        // Wait until we recieved the command request.
        pf.future.get();

        // Now we expect source message to be called and provide the response
        expectSourceMessage([=]() {
            rpc::OpMsgReplyBuilder replyBuilder;
            replyBuilder.setCommandReply(response);
            auto message = replyBuilder.done();
            message.header().setResponseToMsgId(responseToId);
            return message;
        });
        return cmdObjReceived;
    }

    BSONObj scheduleSuccessfulCursorResponse(StringData fieldName,
                                             size_t start,
                                             size_t end,
                                             size_t cursorId,
                                             bool expectedPrefetch = true) {
        auto cursorResponse = buildCursorResponse(fieldName, start, end, cursorId);
        return scheduleResponse(cursorResponse);
    }

    BSONObj scheduleSuccessfulMultiCursorResponse(StringData fieldName,
                                                  size_t start,
                                                  size_t end,
                                                  std::vector<size_t> cursorIds,
                                                  bool expectedPrefetch = true) {
        auto cursorResponse = buildMultiCursorResponse(fieldName, start, end, cursorIds);
        return scheduleResponse(cursorResponse);
    }

    void scheduleErrorResponse(Status error) {
        scheduleResponse(error);
    }

    BSONObj scheduleSuccessfulKillCursorResponse(size_t cursorId, bool expectedPrefetch = true) {
        auto cursorResponse =
            BSON("cursorsKilled" << BSON_ARRAY((long long)(cursorId)) << "cursorsNotFound"
                                 << BSONArray() << "cursorsAlive" << BSONArray() << "cursorsUnknown"
                                 << BSONArray() << "ok" << 1);
        return scheduleResponse(cursorResponse);
    }

    std::unique_ptr<TaskExecutorCursor> makeTec(RemoteCommandRequest rcr,
                                                TaskExecutorCursorOptions&& options) {
        options.pinConnection = true;
        return std::make_unique<TaskExecutorCursor>(getExecutorPtr(), rcr, std::move(options));
    }

    bool hasReadyRequests() {
        return PinnedConnectionTaskExecutorTest::hasReadyRequests();
    }

    bool tryWaitUntilReadyRequests() {
        return PinnedConnectionTaskExecutorTest::tryWaitUntilReadyRequests();
    }

    void blackHoleNextOutgoingRequest() {
        auto pf = makePromiseFuture<void>();
        expectSinkMessage([&](Message m) {
            pf.promise.emplaceValue();
            return Status(ErrorCodes::SocketException, "test");
        });
        pf.future.get();
    }
};
}  // namespace executor
}  // namespace MONGO_MOD_PUB mongo
