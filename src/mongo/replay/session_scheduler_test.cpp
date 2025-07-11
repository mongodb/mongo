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


#include "mongo/replay/session_scheduler.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_command.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/synchronized_value.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

class SessionSchedulerTest : public unittest::Test {
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
            out = executor->runCommand(ReplayCommand{opDoc.getDocument()});
        }
    }

    template <typename Callable>
    void submit(SessionScheduler& sessionScheduler, size_t nSessions, Callable callable) {
        for (size_t i = 0; i < nSessions; i++) {
            sessionScheduler.submit(callable, i);
        }
    }

    void addError(std::exception_ptr ptr) {
        std::lock_guard<std::mutex> lg{_m};
        _errors.push_back(ptr);
    }

    std::vector<std::exception_ptr> getErrors() {
        std::lock_guard<std::mutex> lg{_m};
        return _errors;
    }

private:
    Commands _commands;
    ReplayTestServer _server;
    std::string _jsonStr;
    stdx::mutex _m;
    std::vector<std::exception_ptr> _errors;
};

TEST_F(SessionSchedulerTest, SubmitQueryOneSession) {
    auto executor = std::make_unique<ReplayCommandExecutor>();
    executor->connect(getServerConnectionString());
    ASSERT_TRUE(executor->isConnected());
    std::vector<std::unique_ptr<ReplayCommandExecutor>> executors;
    std::vector<synchronized_value<BSONObj>> outputs(1);
    executors.push_back(std::move(executor));
    auto callable = [this, &executors, &outputs](size_t i) {
        sessionExecTask(std::move(executors[i]), outputs[i]);
    };
    constexpr size_t totalNumberOfSessions = 1;
    {
        SessionScheduler sessionScheduler(1);
        submit(sessionScheduler, totalNumberOfSessions, callable);
    }
    auto& resp = outputs.back();
    ASSERT_FALSE(resp->isEmptyPrototype());
    ASSERT_TRUE(checkServerResponse(*resp));
}

TEST_F(SessionSchedulerTest, SubmitTaskThatIsCrashingAndVerifyCorrectShutdown) {
    constexpr size_t N = 10;
    auto callable = [this](size_t) {
        try {
            throw std::runtime_error("Fake error.");
        } catch (...) {
            addError(std::current_exception());
        }
    };
    {
        SessionScheduler sessionScheduler(N);
        submit(sessionScheduler, N, callable);
    }
    ASSERT_EQ(getErrors().size(), 10);
}

TEST_F(SessionSchedulerTest, SubmitQueryMultipleSessions) {
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
    auto callable = [this, &executors, &outputs](size_t i) {
        sessionExecTask(std::move(executors[i]), outputs[i]);
    };
    {
        SessionScheduler sessionScheduler(N);
        submit(sessionScheduler, N, callable);
    }
    for (auto&& out : outputs) {
        ASSERT_FALSE(out->isEmptyPrototype());
        ASSERT_TRUE(checkServerResponse(*out));
    }
}

}  // namespace mongo
