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


#include "mongo/replay/session_pool.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/replay/raw_op_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

class SessionPoolTest : public unittest::Test {
public:
    using Commands = std::vector<std::pair<std::string, BSONObj>>;

    void setUp() override {
        // basic setup for the session pool tests.
        BSONObj filter = BSON("name" << "Alice");
        BSONObj findCommand = BSON("find" << "test" << "$db" << "test" << "filter" << filter);
        _jsonStr = R"([{  
        "_id": "681cb423980b72695075137f",  
        "name": "Alice",  
        "age": 30,  
        "city": "New York"}])";
        // only one command for now.
        addBSONCommand("find", findCommand, _jsonStr);
    }

    void addBSONCommand(const std::string& name, BSONObj command, const std::string& response) {
        _server.setupServerResponse(name, response);
        _commands.push_back({name, command});
    }

    const Commands& getCommands() const {
        return _commands;
    }

    std::string getServerConnectionString() const {
        return _server.getConnectionString();
    }

    bool checkServerResponse(BSONObj response) const {
        return _server.checkResponse("find", response);
    }

    void sessionExecTask(std::unique_ptr<ReplayCommandExecutor>&& executor,
                         synchronized_value<BSONObj>& out) {
        ASSERT_TRUE(executor);
        ASSERT_TRUE(executor->isConnected());
        for (const auto& [name, c] : getCommands()) {
            RawOpDocument opDoc{name, c};
            auto response = executor->runCommand(ReplayCommand{opDoc.getBinaryCommand()});
            out = response;
        }
    }

    void submitToSessionPool(SessionPool&& sessionPool,
                             std::vector<std::unique_ptr<ReplayCommandExecutor>>&& execs,
                             std::vector<synchronized_value<BSONObj>>& outs) {
        ASSERT_TRUE(execs.size() == outs.size());
        const size_t N = execs.size();
        ASSERT_TRUE(N > 0);
        std::vector<stdx::future<void>> futures;
        auto funct = [this, &execs, &outs](size_t i) {
            sessionExecTask(std::move(execs[i]), outs[i]);
        };
        for (size_t i = 0; i < N; i++) {
            auto task = sessionPool.submit(funct, i);
            futures.push_back(std::move(task));
        }
        for (auto& f : futures) {
            f.get();
        }
    }

private:
    Commands _commands;
    ReplayTestServer _server;
    std::string _jsonStr;
};

TEST_F(SessionPoolTest, SubmitQueryOneSession) {
    auto executor = std::make_unique<ReplayCommandExecutor>();
    executor->connect(getServerConnectionString());
    ASSERT_TRUE(executor->isConnected());
    SessionPool sessionPool(1);
    std::vector<std::unique_ptr<ReplayCommandExecutor>> executors;
    std::vector<synchronized_value<BSONObj>> outputs(1);
    executors.push_back(std::move(executor));
    submitToSessionPool(std::move(sessionPool), std::move(executors), outputs);
    auto& resp = outputs.back();
    ASSERT_FALSE(resp->isEmptyPrototype());
    ASSERT_TRUE(checkServerResponse(*resp));
}

TEST_F(SessionPoolTest, SubmitQueryMultipleSessions) {
    // mix a bit things, N sessions in parallel hammering the same server.
    const size_t N = 10;
    std::vector<std::unique_ptr<ReplayCommandExecutor>> executors;
    // connect each executor to the server.
    for (size_t i = 0; i < N; ++i) {
        executors.push_back(std::make_unique<ReplayCommandExecutor>());
        auto&& ex = executors.back();
        ex->connect(getServerConnectionString());
        ASSERT_TRUE(ex->isConnected());
    }
    std::vector<synchronized_value<BSONObj>> outputs(N);
    SessionPool sessionPool(N);
    submitToSessionPool(std::move(sessionPool), std::move(executors), outputs);
    for (auto&& out : outputs) {
        ASSERT_FALSE(out->isEmptyPrototype());
        ASSERT_TRUE(checkServerResponse(*out));
    }
}

}  // namespace mongo
