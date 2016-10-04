/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

/**
 * Constructs a single death test named "TEST_NAME" within the test case "CASE_NAME".
 *
 * The test only succeeds if the process terminates abnormally, e.g. through an fassert
 * failure or deadly signal.
 *
 * Death tests are incompatible with already-running threads. If you need multiple threads
 * in your death test, start them in the test body, or use DEATH_TEST_F and start them
 * in the setUp() method of the fixture.
 */
#define DEATH_TEST(CASE_NAME, TEST_NAME, MATCH_EXPR)                               \
    class _TEST_TYPE_NAME(CASE_NAME, TEST_NAME) : public ::mongo::unittest::Test { \
    private:                                                                       \
        virtual void _doTest();                                                    \
                                                                                   \
        static const RegistrationAgent<                                            \
            ::mongo::unittest::DeathTest<_TEST_TYPE_NAME(CASE_NAME, TEST_NAME)>>   \
            _agent;                                                                \
    };                                                                             \
    const ::mongo::unittest::Test::RegistrationAgent<                              \
        ::mongo::unittest::DeathTest<_TEST_TYPE_NAME(CASE_NAME, TEST_NAME)>>       \
        _TEST_TYPE_NAME(CASE_NAME, TEST_NAME)::_agent(#CASE_NAME, #TEST_NAME);     \
    std::string getDeathTestPattern(_TEST_TYPE_NAME(CASE_NAME, TEST_NAME)*) {      \
        return MATCH_EXPR;                                                         \
    }                                                                              \
    void _TEST_TYPE_NAME(CASE_NAME, TEST_NAME)::_doTest()

/**
 * Constructs a single test named TEST_NAME that has access to a common fixture
 * named "FIXTURE_NAME".
 *
 * See description of DEATH_TEST for more details on death tests.
 */
#define DEATH_TEST_F(FIXTURE_NAME, TEST_NAME, MATCH_EXPR)                            \
    class _TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME) : public FIXTURE_NAME {           \
    private:                                                                         \
        virtual void _doTest();                                                      \
                                                                                     \
        static const RegistrationAgent<                                              \
            ::mongo::unittest::DeathTest<_TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)>>  \
            _agent;                                                                  \
    };                                                                               \
    const ::mongo::unittest::Test::RegistrationAgent<                                \
        ::mongo::unittest::DeathTest<_TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)>>      \
        _TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)::_agent(#FIXTURE_NAME, #TEST_NAME); \
    std::string getDeathTestPattern(_TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)*) {     \
        return MATCH_EXPR;                                                           \
    }                                                                                \
    void _TEST_TYPE_NAME(FIXTURE_NAME, TEST_NAME)::_doTest()

namespace mongo {
namespace unittest {

class DeathTestImpl : public Test {
    MONGO_DISALLOW_COPYING(DeathTestImpl);

protected:
    DeathTestImpl(std::unique_ptr<Test> test);

private:
    void _doTest() override;
    virtual std::string getPattern() = 0;
    std::unique_ptr<Test> _test;
};

template <typename T>
class DeathTest : public DeathTestImpl {
public:
    static const std::string pattern;

    template <typename... Args>
    DeathTest(Args&&... args) : DeathTestImpl(stdx::make_unique<T>(std::forward<Args>(args)...)) {}

private:
    std::string getPattern() override {
        return getDeathTestPattern(static_cast<T*>(nullptr));
    }
};

}  // namespace unittest
}  // namespace mongo
