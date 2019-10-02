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

#pragma once

#include <functional>
#include <memory>
#include <string>

#include "mongo/unittest/unittest.h"

/**
 * Constructs a single death test named `TEST_NAME` within the test suite `SUITE_NAME`.
 *
 * The test only succeeds if the process terminates abnormally, e.g. through an fassert
 * failure or deadly signal.
 *
 * Death tests are incompatible with already-running threads. If you need multiple threads
 * in your death test, start them in the test body, or use DEATH_TEST_F and start them
 * in the setUp() method of the fixture.
 */
#define DEATH_TEST(SUITE_NAME, TEST_NAME, MATCH_EXPR) \
    DEATH_TEST_DEFINE_(SUITE_NAME, TEST_NAME, MATCH_EXPR, ::mongo::unittest::Test)

/**
 * Constructs a single test named TEST_NAME that has access to a common fixture
 * named `FIXTURE_NAME`, which will be used as the Suite name.
 *
 * See description of DEATH_TEST for more details on death tests.
 */
#define DEATH_TEST_F(FIXTURE_NAME, TEST_NAME, MATCH_EXPR) \
    DEATH_TEST_DEFINE_(FIXTURE_NAME, TEST_NAME, MATCH_EXPR, FIXTURE_NAME)

#define DEATH_TEST_DEFINE_(SUITE_NAME, TEST_NAME, MATCH_EXPR, TEST_BASE)                 \
    DEATH_TEST_DEFINE_PRIMITIVE_(SUITE_NAME,                                             \
                                 TEST_NAME,                                              \
                                 MATCH_EXPR,                                             \
                                 UNIT_TEST_DETAIL_TEST_TYPE_NAME(SUITE_NAME, TEST_NAME), \
                                 TEST_BASE)

#define DEATH_TEST_DEFINE_PRIMITIVE_(SUITE_NAME, TEST_NAME, MATCH_EXPR, TEST_TYPE, TEST_BASE)  \
    class TEST_TYPE : public TEST_BASE {                                                       \
    public:                                                                                    \
        static std::string getPattern() {                                                      \
            return MATCH_EXPR;                                                                 \
        }                                                                                      \
                                                                                               \
    private:                                                                                   \
        void _doTest() override;                                                               \
        static inline const RegistrationAgent<::mongo::unittest::DeathTest<TEST_TYPE>> _agent{ \
            #SUITE_NAME, #TEST_NAME};                                                          \
    };                                                                                         \
    void TEST_TYPE::_doTest()

namespace mongo::unittest {

class DeathTestBase : public Test {
protected:
    DeathTestBase() = default;

private:
    // Forks, executes _doMakeTest() in the child process to create a Test, then runs that Test.
    void _doTest() final;

    // Customization points for derived DeathTest classes.
    virtual std::unique_ptr<Test> _doMakeTest() = 0;
    virtual std::string _doGetPattern() = 0;
};

template <typename T>
class DeathTest : public DeathTestBase {
public:
    template <typename... Args>
    explicit DeathTest(Args&&... args)
        : _makeTest([args...] { return std::make_unique<T>(args...); }) {}

private:
    std::string _doGetPattern() override {
        return T::getPattern();
    }

    std::unique_ptr<Test> _doMakeTest() override {
        return _makeTest();
    }

    std::function<std::unique_ptr<Test>()> _makeTest;
};

}  // namespace mongo::unittest
