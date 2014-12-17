/**
 * Copyright (C) 2012 10gen Inc.
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
    TEST_F(TestFixture, TearDownAfterThrowing ) {
        // Make sure tear down was called in the test above this.
        ASSERT_EQUALS(_num_tear_downs, 3);
    }

    // ^^^^^^^^^^^^^^^^^^^^^^^^  Do not add test above

    // New tests may be added below.

    TEST_F(TestFixture, VariableAndMethodAccessTest) {
        ASSERT_EQUALS(10, _myVar);
        ASSERT_EQUALS(11, inc());
    }

    class EmptyFixture : public mongo::unittest::Test {
    };

    TEST_F(EmptyFixture, EmptyTest) {
    }

} // unnamed namespace
