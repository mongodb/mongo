/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <limits>

#include "mongo/executor/network_interface_mock.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class BSONObj;
using executor::RemoteCommandResponse;

namespace test {
namespace mock {

// MockNetwork wraps the NetworkInterfaceMock to provide a declarative approach
// to specify expected behaviors on the network and to hide the interaction with
// the NetworkInterfaceMock.
class MockNetwork {
public:
    using MatcherFunc = std::function<bool(const BSONObj&)>;
    using ActionFunc = std::function<RemoteCommandResponse(const BSONObj&)>;

    class Matcher {
    public:
        Matcher(const char* cmdName) : Matcher(std::string(cmdName)) {}
        Matcher(const std::string& cmdName) {
            _matcherFunc = [=](const BSONObj& request) {
                return request.firstElementFieldNameStringData() == cmdName;
            };
        }

        Matcher(MatcherFunc matcherFunc) : _matcherFunc(std::move(matcherFunc)) {}

        bool operator()(const BSONObj& request) {
            return _matcherFunc(request);
        }

    private:
        MatcherFunc _matcherFunc;
    };

    class Action {
    public:
        Action(ActionFunc func) : _actionFunc(std::move(func)){};

        Action(const BSONObj& response) {
            _actionFunc = [=](const BSONObj& request) {
                return RemoteCommandResponse(response, Milliseconds(0));
            };
        }

        Action(const RemoteCommandResponse& commandResponse) {
            _actionFunc = [=](const BSONObj& request) { return commandResponse; };
        }

        RemoteCommandResponse operator()(const BSONObj& request) {
            return _actionFunc(request);
        }

    private:
        ActionFunc _actionFunc;
    };

    class Expectation {
    public:
        Expectation(Matcher matcher, Action action)
            : _matcher(std::move(matcher)), _action(std::move(action)) {}

        Expectation& times(int t) {
            _allowedTimes = t;
            return *this;
        }

        bool match(const BSONObj& request) {
            return _matcher(request);
        }

        RemoteCommandResponse run(const BSONObj& request) {
            if (!isDefault()) {
                _allowedTimes--;
            }
            return _action(request);
        }

        bool isDefault() const {
            return _allowedTimes == std::numeric_limits<int>::max();
        }

        bool isSatisfied() const {
            return _allowedTimes == 0;
        }

    private:
        Matcher _matcher;
        Action _action;
        int _allowedTimes = std::numeric_limits<int>::max();
    };

    explicit MockNetwork(executor::NetworkInterfaceMock* net) : _net(net) {}

    // Accept anything that Matcher's and Action's constructors allow.
    template <typename MatcherType>
    Expectation& expect(MatcherType&& matcher, Action action) {
        _expectations.emplace_back(Matcher(std::forward<MatcherType>(matcher)), std::move(action));
        return _expectations.back();
    }

    // Advance time to the target. Run network operations and process requests along the way.
    void runUntil(Date_t targetTime);

    // Run until both the executor and the network are idle and all expectations are satisfied.
    // Otherwise, it hangs forever.
    void runUntilExpectationsSatisfied();

private:
    void _runUntilIdle();
    bool _allExpectationsSatisfied() const;

    std::vector<Expectation> _expectations;
    executor::NetworkInterfaceMock* _net;
};

}  // namespace mock
}  // namespace test
}  // namespace mongo
