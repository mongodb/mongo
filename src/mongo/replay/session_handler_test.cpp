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

#include "mongo/replay/session_handler.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/replay/test_packet.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <ratio>

namespace mongo {
TEST(SessionHandlerTest, StartAndStopSession) {
    ReplayTestServer server;

    auto startRecording = cmds::start({.offset = Milliseconds(0)});
    auto stopRecording = cmds::stop({.offset = Milliseconds(5)});

    {
        const auto uri = server.getConnectionString();
        SessionHandler sessionHandler{uri};
        sessionHandler.onSessionStart(startRecording);
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 1);
        sessionHandler.onSessionStop(stopRecording);
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
        // session is not deleted. Should be ready to be reused.
        sessionHandler.clear();
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
    }
}

TEST(SessionHandlerTest, StartSessionSameSessionIDError) {
    ReplayTestServer server;
    const auto uri = server.getConnectionString();

    auto commandStart1 = cmds::start({.offset = Microseconds(0)});

    {
        SessionHandler sessionHandler{uri};
        sessionHandler.onSessionStart(commandStart1);
        auto commandStart2 = cmds::start({.offset = Milliseconds(100)});
        // this will throw. we can't have different sessions with same session id.
        ASSERT_THROWS_CODE(sessionHandler.onSessionStart(commandStart2),
                           DBException,
                           ErrorCodes::ReplayClientSessionSimulationError);

        // closing the first session and starting again with the same sessionId will work.
        auto commandStop1 = cmds::stop({.offset = Microseconds(0)});
        sessionHandler.onSessionStop(commandStop1);
        // there should be 0 active sessions and 1 free session simulator to be re-used
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
        sessionHandler.onSessionStart(commandStart2);
        // not there should be 1 active session and 0 free sessions.
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 1);
        sessionHandler.clear();
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
    }
}

TEST(SessionHandlerTest, StartTwoSessionsDifferentSessionIDSameKey) {
    ReplayTestServer server;
    const auto uri = server.getConnectionString();

    // start command from session 1
    auto commandStart1 = cmds::start({.id = 1, .offset = Microseconds(0)});

    // start command from session2
    auto commandStart2 = cmds::start({.id = 2, .offset = Microseconds(50)});

    // stop command from session1
    auto commandStop1 = cmds::stop({.offset = Microseconds(100)});

    {
        SessionHandler sessionHandler{uri};
        sessionHandler.onSessionStart(commandStart1);

        sessionHandler.onSessionStop(commandStop1);

        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
        ASSERT_THROWS_CODE(sessionHandler.onBsonCommand(commandStart1),
                           DBException,
                           ErrorCodes::ReplayClientSessionSimulationError);

        sessionHandler.onSessionStart(commandStart2);
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 1);
        sessionHandler.clear();
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
    }
}

TEST(SessionHandlerTest, ExecuteCommand) {
    // start
    auto startRecording = cmds::start({.offset = Microseconds(0)});

    // stop
    auto stopRecording = cmds::stop({.offset = Milliseconds(20)});

    // find
    BSONObj filterBSON = BSON("name" << "Alice");
    auto findCommand = cmds::find({.offset = Milliseconds(10)}, filterBSON);


    std::string jsonStr = R"([{
    "_id": "681cb423980b72695075137f",
    "name": "Alice",
    "age": 30,
    "city": "New York"}])";
    // server
    ReplayTestServer server{{"find"}, {jsonStr}};
    const auto uri = server.getConnectionString();


    {
        SessionHandler sessionHandler{uri};
        sessionHandler.onSessionStart(startRecording);
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 1);

        sessionHandler.onBsonCommand(findCommand);

        sessionHandler.onSessionStop(stopRecording);
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);

        //  session has now been closed, no connections, so trying to submit the same command should
        //  throw.
        ASSERT_THROWS_CODE(sessionHandler.onBsonCommand(findCommand),
                           DBException,
                           ErrorCodes::ReplayClientSessionSimulationError);

        // clear the state
        sessionHandler.clear();
        ASSERT_TRUE(sessionHandler.fetchTotalRunningSessions() == 0);
    }
}

}  // namespace mongo
