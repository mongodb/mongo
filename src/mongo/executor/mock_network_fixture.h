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

        Matcher(const BSONObj& matcherQuery);

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
        virtual ~Expectation() {}

        // We should only try to match an expectation if all its prerequisites in the sequence are
        // satisfied (or if it is not part of a sequence in the first place).
        // Default expectations should always match, as they cannot form sequences, and because we
        // generally have no restrictions on when and how many times they match.
        virtual bool prerequisitesMet() {
            return true;
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

        virtual bool isDefault() const {
            return _allowedTimes == std::numeric_limits<int>::max();
        }

        bool isSatisfied() const {
            return _allowedTimes == 0;
        }

        // May throw.
        virtual void checkSatisfied() {}

    protected:
        int _allowedTimes = std::numeric_limits<int>::max();

    private:
        Matcher _matcher;
        Action _action;
    };

    class UserExpectation : public Expectation {
    public:
        UserExpectation(Matcher matcher, Action action) : Expectation(matcher, action) {
            // Default value, may be overriden by calling times().
            _allowedTimes = 1;
        }

        // We forbid matching if we have unsatisfied prerequisites and allow it otherwise.
        bool prerequisitesMet() override {
            // No prerequisites at all - okay to match.
            if (!_prevInSequence) {
                return true;
            }
            // If this expectation's immediate prerequisite is satisfied, that implies the
            // previous ones in the chain have also been satisfied.
            return _prevInSequence->isSatisfied();
        }

        Expectation& times(int t) {
            _allowedTimes = t;
            return *this;
        }

        bool isDefault() const override {
            return false;
        }

        void checkSatisfied() override {
            uassert(5015501, "UserExpectation not satisfied", isSatisfied());
        }

        // The expectation that comes before this on in the sequence, if applicable.
        UserExpectation* _prevInSequence = nullptr;
    };

    class DefaultExpectation : public Expectation {
    public:
        DefaultExpectation(Matcher matcher, Action action) : Expectation(matcher, action) {}

        bool isDefault() const override {
            return true;
        }
    };

    class Sequence {
    public:
        void addExpectation(UserExpectation& exp) {
            exp._prevInSequence = _lastExpectation;
            _lastExpectation = &exp;
        }

    private:
        UserExpectation* _lastExpectation = nullptr;
    };

    // RAII construct to handle expectations declared as part of a sequence.
    class InSequence {
    public:
        InSequence(MockNetwork& mock) : _mock(mock) {
            invariant(!_mock._activeSequence);
            _mock._activeSequence = std::make_unique<Sequence>();
        };
        ~InSequence() {
            _mock._activeSequence.reset();
        };

    private:
        MockNetwork& _mock;
    };

    explicit MockNetwork(executor::NetworkInterfaceMock* net) : _net(net) {}

    // Accept anything that Matcher's and Action's constructors allow.
    // Use expect() to mandate the exact calls each test expects. They must all be satisfied
    // or the test fails.
    template <typename MatcherType>
    UserExpectation& expect(MatcherType&& matcher, Action action) {
        auto exp = std::make_unique<UserExpectation>(Matcher(std::forward<MatcherType>(matcher)),
                                                     std::move(action));
        auto& ref = *exp;
        _expectations.emplace_back(std::move(exp));

        if (_activeSequence) {
            _activeSequence.get()->addExpectation(ref);
        }

        return ref;
    }

    // Accept anything that Matcher's and Action's constructors allow.
    // Use defaultExpect() to specify shared behavior in test fixtures. This is best for
    // uninteresting calls common to a class of tests.
    // For these reasons, you are not allowed to declare further default expecations after
    // having already enqueued user expectations.
    template <typename MatcherType>
    DefaultExpectation& defaultExpect(MatcherType&& matcher, Action action) {
        auto order = std::none_of(_expectations.begin(), _expectations.end(), [](const auto& exp) {
            return !exp->isDefault();
        });
        uassert(
            5015502, "All default expectations must be declared before user expectations.", order);

        auto exp = std::make_unique<DefaultExpectation>(Matcher(std::forward<MatcherType>(matcher)),
                                                        std::move(action));
        auto& ref = *exp;
        _expectations.emplace_back(std::move(exp));
        return ref;
    }

    void verifyExpectations() {
        for (auto& exp : _expectations) {
            // This expectation will throw if it has not been met.
            exp->checkSatisfied();
        }
    }

    // Removes user expectations only.
    void clearExpectations() {
        _expectations.erase(std::remove_if(_expectations.begin(),
                                           _expectations.end(),
                                           [&](auto& exp) { return !exp->isDefault(); }),
                            _expectations.end());
    }

    // Carries over default expectations but clears user expectations. Checks that the latter
    // have all been satisfied in the process.
    void verifyAndClearExpectations() {
        // May throw.
        verifyExpectations();
        clearExpectations();
    }


    // Advance time to the target. Run network operations and process requests along the way.
    void runUntil(Date_t targetTime);

    // Run until both the executor and the network are idle and all expectations are satisfied.
    // Otherwise, it hangs forever.
    void runUntilExpectationsSatisfied();

private:
    void _runUntilIdle();
    bool _allExpectationsSatisfied() const;

    std::vector<std::unique_ptr<Expectation>> _expectations;
    std::unique_ptr<Sequence> _activeSequence;
    executor::NetworkInterfaceMock* _net;
};

}  // namespace mock
}  // namespace test
}  // namespace mongo
