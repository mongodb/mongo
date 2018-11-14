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

/**
 * Unit tests of the unittest framework itself.
 */

#include "mongo/unittest/unittest.h"

namespace {

class TestFixture : public mongo::unittest::Test {
protected:
    int _myVar;
    static int _num_set_ups;
    static int _num_tear_downs;

    void setUp() {
        _num_set_ups++;
        _myVar = 10;
    }

    void tearDown() {
        _num_tear_downs++;
        _myVar = 0;
    }

    int inc() {
        return ++_myVar;
    }

    void throwSpecialException() {
        throw FixtureExceptionForTesting();
    }
};

int TestFixture::_num_set_ups = 0;
int TestFixture::_num_tear_downs = 0;

// NOTE:
// Test cases should not be designed that depend on the order they appear. But because
// we're testing the test framework itself, we do not follow this rule here and require the
// following four tests to be in that order.

// vvvvvvvvvvvvvvvvvvvvvvvv Do not add tests below

// This needs to be the very first test. Please, see NOTE above.
TEST_F(TestFixture, SetUpTest) {
    ASSERT_EQUALS(_num_set_ups, 1);
    ASSERT_EQUALS(_num_tear_downs, 0);
}

// This needs to be the second test. Please, see NOTE above.
TEST_F(TestFixture, TearDownTest) {
    ASSERT_EQUALS(_num_set_ups, 2);
    ASSERT_EQUALS(_num_tear_downs, 1);
}

// This needs to be the third/fourth test. Please, see NOTE above. We are
// finishing a test case by throwing an exception. Normally, the framework
// would treat this as an error. But what we'd like here is to make sure
// that the fixture tear down routines were called in that case.
TEST_F(TestFixture, Throwing) {
    throwSpecialException();
}
TEST_F(TestFixture, TearDownAfterThrowing) {
    // Make sure tear down was called in the test above this.
    ASSERT_EQUALS(_num_tear_downs, 3);
}

// ^^^^^^^^^^^^^^^^^^^^^^^^  Do not add test above

// New tests may be added below.

TEST_F(TestFixture, VariableAndMethodAccessTest) {
    ASSERT_EQUALS(10, _myVar);
    ASSERT_EQUALS(11, inc());
}

class EmptyFixture : public mongo::unittest::Test {};

TEST_F(EmptyFixture, EmptyTest) {}

}  // unnamed namespace
