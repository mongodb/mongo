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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

class BSONObj;

using executor::RemoteCommandResponse;

namespace test {
namespace mock {

/**
 * MockNetwork wraps the NetworkInterfaceMock to provide a declarative approach to specify
 * expected behaviors on the network and to hide the interaction with NetworkInterfaceMock.
 * We refer to the behaviors we are testing as "expectations".
 *
 * Expectations:
 * All expectations are unordered by default, unless otherwise specified (see Sequences below).
 * There are two types of expectations: default expectations and user expectations.
 * The purpose of default expectations is to cover uninteresting calls that are typically
 * repeated throughout a set of tests, so that individual tests can focus on the specific
 * behaviors they are meant to evaluate. Default expectations may be matched any number of times
 * (including zero).
 * User expectations represent the concrete requirements of a test. They must all be matched
 * the exact number of times required. For example:
 *
 *      // Earlier in the test fixture
 *      mock.defaultExpect(...); // A
 *      mock.defaultExpect(...); // B
 *
 *      ...
 *
 *      // In individual tests
 *      mock.expect(...).times(...); // C
 *      mock.expect(...).times(...); // D
 *
 *      ...
 *
 *      // Later in those tests
 *      mock.runUntilExpectationsSatisfied();
 *
 * We have no requirements here on matching A or B, but C and D must be satisfied in full.
 * You may omit the times() call if you would like to use the default of 1 "times".
 *
 * To run expectations, use runUntilExpectationsSatisfied(), which will block until all
 * expectations have been fully satisfied. You may also use runUntil() if you only want to
 * advance to a particular time. If you would like to still check that your expectations have
 * been satisfied (or explicitly demonstrate that they haven't), call verifyExpectations().
 *
 * Sequences:
 * The Sequence class provides a way to specify order of expectations. A test may have all
 * expectations in a sequence, or a subset, or none at all. It may also include any number of
 * sequences. Note that sequences only specify partial ordering, which means it is acceptable to
 * interleave other expectations in between sequence members (including expectations belonging to
 * other sequences) so long as all sequences are run in their required order. We enforce ordering
 * by refusing to match an expectation if it has unsatisfied prerequisites.
 *
 * To specify a sequence, use the InSequence RAII type, which will add all Expectations in its
 * scope to an anonymous sequence. For example:
 *
 *
 *      {
 *          MockNetwork::InSequence seq(mock);
 *
 *          mock.expect(...); // A
 *          mock.expect(...); // B
 *      }
 *
 *      mock.expect(...); // C
 *
 *
 * In this example, we only require that A comes before B. We have no requirements for when C
 * is executed. Therefore, all of {ABC, ACB, CAB} are valid, but {BAC, BCA, CBA} are not valid.
 */
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
        Action(ActionFunc func) : _actionFunc(std::move(func)) {};

        Action(const BSONObj& response) {
            _actionFunc = [=](const BSONObj& request) {
                return RemoteCommandResponse::make_forTest(response, Milliseconds(0));
            };
        }

        Action(const RemoteCommandResponse& commandResponse) {
            _actionFunc = [=](const BSONObj& request) {
                return commandResponse;
            };
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

        int executionsRemaining() const {
            return _allowedTimes;
        }

        virtual void logIfUnsatisfied() {
            if (!isSatisfied()) {
                LOGV2(9467602,
                      "Unsatisfied expectation found",
                      "executionsRemaining"_attr = executionsRemaining(),
                      "expectedResponse"_attr = _action(BSONObj()));
            }
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

        void logIfUnsatisfied() override {
            // We do not want to log default expectations.
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

    // Run until both the executor and the network are idle. Otherwise, it hangs forever.
    void runUntilIdle();

    // Run until both the executor and the network are idle and all expectations are satisfied.
    // Otherwise, it fatal logs after timeoutSeconds.
    void runUntilExpectationsSatisfied(
        stdx::chrono::seconds timeoutSeconds = stdx::chrono::seconds(120));

private:
    bool _allExpectationsSatisfied() const;
    void _logUnsatisfiedExpectations() const;

    std::vector<std::unique_ptr<Expectation>> _expectations;
    std::unique_ptr<Sequence> _activeSequence;
    executor::NetworkInterfaceMock* _net;
};

}  // namespace mock
}  // namespace test
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
