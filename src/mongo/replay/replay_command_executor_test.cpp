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
#include "mongo/replay/replay_command_executor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/replay/raw_op_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_test_server.h"
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
    BSONObj filter = BSON("name" << "Alice");
    BSONObj findCommand = BSON("find" << "test" << "$db" << "test" << "filter" << filter);
    std::string jsonStr = R"([{  
    "_id": "681cb423980b72695075137f",  
    "name": "Alice",  
    "age": 30,  
    "city": "New York"}])";
    ReplayTestServer server{{"find"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;
    RawOpDocument opDoc{"find", findCommand};
    ReplayCommand command{opDoc.getBinaryCommand()};
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_TRUE(replayCommandExecutor.isConnected());
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("find", response));
}

TEST(ReplayCommandExecutionTest, TestInsert) {
    BSONObj insertDocument = BSON("name" << "Alice");
    BSONObj insertCommand =
        BSON("insert" << "test" << "$db" << "test" << "documents" << BSON_ARRAY(insertDocument));
    std::string jsonStr = R"({"ok": 1,"n": 1})";

    ReplayTestServer server{{"insert"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_EQUALS(replayCommandExecutor.isConnected(), true);
    RawOpDocument opDoc{"insert", insertCommand};
    ReplayCommand command{opDoc.getBinaryCommand()};
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("insert", response));
}

TEST(ReplayCommandExecutionTest, TestAggregate) {
    BSONObj matchStage = BSON("$match" << BSON("name" << BSON("$regex" << "^A")));
    BSONObj sortStage = BSON("$sort" << BSON("name" << 1));
    BSONObj projectStage =
        BSON("$project" << BSON("name" << 1 << "name_uppercase" << BSON("$toUpper" << "$name")));
    BSONObj pipeline = BSON("pipeline" << BSON_ARRAY(matchStage << sortStage << projectStage));
    BSONObj aggregationCommand =
        BSON("aggregate" << "test" << "$db" << "test" << "pipeline"
                         << BSON_ARRAY(matchStage << sortStage << projectStage) << "cursor"
                         << BSON("batchSize" << 100));
    std::string jsonStr = R"([{"name_original": "Alice", "name_uppercase": "ALICE" }])";

    ReplayTestServer server{{"aggregate"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;
    RawOpDocument opDoc{"aggregate", aggregationCommand};
    ReplayCommand command{opDoc.getBinaryCommand()};
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_TRUE(replayCommandExecutor.isConnected());
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("aggregate", response));
}

TEST(ReplayCommandExecutionTest, TestDelete) {
    BSONObj filter = BSON("name" << "Alice");
    BSONObj deleteOp = BSON("q" << filter << "limit" << 1);
    BSONObj deleteCommand =
        BSON("delete" << "test" << "$db" << "test" << "deletes" << BSON_ARRAY(deleteOp));
    std::string jsonStr = R"({"n": 1,"ok": 1})";

    ReplayTestServer server{{"delete"}, {jsonStr}};
    ReplayCommandExecutor replayCommandExecutor;
    RawOpDocument opDoc{"delete", deleteCommand};
    ReplayCommand command{opDoc.getBinaryCommand()};
    replayCommandExecutor.connect(server.getConnectionString());
    ASSERT_EQUALS(replayCommandExecutor.isConnected(), true);
    auto response = replayCommandExecutor.runCommand(command);
    ASSERT_TRUE(server.checkResponse("delete", response));
}

}  // namespace mongo
