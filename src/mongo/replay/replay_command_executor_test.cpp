// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/replay/replay_command_executor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/traffic_reader.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/replay/test_packet.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <string>

namespace mongo {

TEST(ReplayCommandExecutionTest, ConnectionTest) {
    ReplayTestServer server;
    ReplayCommandExecutor replayCommandExecutor;
    ASSERT_FALSE(replayCommandExecutor.isConnected());
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_TRUE(replayCommandExecutor.isConnected());
    replayCommandExecutor.reset();
    ASSERT_FALSE(replayCommandExecutor.isConnected());
}

TEST(ReplayCommandExecutionTest, TestFind) {
    std::string jsonStr = R"([{  
    "_id": "681cb423980b72695075137f",  
    "name": "Alice",  
    "age": 30,  
    "city": "New York"}])";
    ReplayTestServer server{{"find"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;

    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_TRUE(replayCommandExecutor.isConnected());

    auto command = cmds::find({}, BSON("name" << "Alice"));
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("find", response));
}

TEST(ReplayCommandExecutionTest, TestInsert) {
    std::string jsonStr = R"({"ok": 1,"n": 1})";

    ReplayTestServer server{{"insert"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_EQUALS(replayCommandExecutor.isConnected(), true);
    auto command = cmds::insert({}, BSON_ARRAY(BSON("name" << "Alice")));
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("insert", response));
}

TEST(ReplayCommandExecutionTest, TestAggregate) {
    BSONObj matchStage = BSON("$match" << BSON("name" << BSON("$regex" << "^A")));
    BSONObj sortStage = BSON("$sort" << BSON("name" << 1));
    BSONObj projectStage =
        BSON("$project" << BSON("name" << 1 << "name_uppercase" << BSON("$toUpper" << "$name")));
    BSONArray pipeline = BSON_ARRAY(matchStage << sortStage << projectStage);
    std::string jsonStr = R"([{"name_original": "Alice", "name_uppercase": "ALICE" }])";

    ReplayTestServer server{{"aggregate"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;

    auto command = cmds::aggregate({}, pipeline);
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_TRUE(replayCommandExecutor.isConnected());
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("aggregate", response));
}

TEST(ReplayCommandExecutionTest, TestDelete) {
    BSONObj filter = BSON("name" << "Alice");
    std::string jsonStr = R"({"n": 1,"ok": 1})";

    ReplayTestServer server{{"delete"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;

    auto command = cmds::del({}, filter);
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_EQUALS(replayCommandExecutor.isConnected(), true);
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("delete", response));
}

}  // namespace mongo
