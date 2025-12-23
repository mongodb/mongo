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

/*
 * A C++ unit testing framework.
 *
 * Most users will include the umbrella header "mongo/unittest/unittest.h".
 *
 * For examples of basic usage, see mongo/unittest/unittest_test.cpp.
 *
 * ASSERT macros and supporting definitions are in mongo/unittest/assert.h.
 *
 */

#pragma once

// IWYU pragma: private, include "mongo/unittest/unittest.h"
// IWYU pragma: friend "mongo/unittest/.*"

#include <gmock/gmock.h>  // IWYU pragma: export
#include <gtest/gtest.h>  // IWYU pragma: export

// TODO(gtest) consider making this change (or something like it) in the gtest codebase.
// We should enforce that all inclusions of gtest go through this header; if we
// don't move this redefinition to the gtest codebase, we must enforce that.
#undef GTEST_FATAL_FAILURE_

/**
 * GTEST_FATAL_FAILURE_ is defined as a return statement. When gtest assertions are made to throw
 * exceptions rather than return, the result is that the return statement of this macro is never
 * reached, and this has various usability issues described in more detail here:
 * https://github.com/google/googletest/issues/4770. To work around this issue, we redefine
 * GTEST_FATAL_FAILURE_ without a return statement. In gtest, this macro would expand to 'return'.
 */
#define GTEST_FATAL_FAILURE_RETURN_

#define GTEST_FATAL_FAILURE_(message) \
    GTEST_FATAL_FAILURE_RETURN_ GTEST_MESSAGE_(message, ::testing::TestPartResult::kFatalFailure)

#include "mongo/platform/source_location.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>

namespace mongo::unittest {

/**
 * Test fixture base class. Defined to translate old mongo-style setUp/tearDown methods to their
 * gtest counterparts, SetUp/TearDown.
 */
class MONGO_MOD_OPEN Test : public testing::Test {
public:
    virtual void setUp() {}
    virtual void tearDown() {}
    void SetUp() override {
        setUp();
    }
    void TearDown() override {
        tearDown();
    }
};

/**
 * Adaptor to set up a Suite from a dbtest-style suite.
 * Support for deprecated dbtest-style test suites. Tests are are added by overriding setupTests()
 * in a subclass of OldStyleSuiteSpecification, and defining an OldStyleSuiteInstance<T> object.
 * This approach is deprecated.
 *
 * Example:
 *     class All : public OldStyleSuiteSpecification {
 *     public:
 *         All() : OldStyleSuiteSpecification("BunchaTests") {}
 *         void setupTests() {
 *            add<TestThis>();
 *            add<TestThat>();
 *            add<TestTheOtherThing>();
 *         }
 *     };
 *     OldStyleSuiteInitializer<All> all;
 */
class MONGO_MOD_OPEN OldStyleSuiteSpecification {
public:
    explicit OldStyleSuiteSpecification(std::string name) : _name(std::move(name)) {}
    virtual ~OldStyleSuiteSpecification() = default;

    // Note: setupTests() is run by a OldStyleSuiteInitializer at static initialization time.
    // It should in most cases be just a simple sequence of add<T>() calls.
    virtual void setupTests() = 0;

    const std::string& name() const {
        return _name;
    }

    /**
     * Add an old-style test of type `T` to this Suite, saving any test constructor args
     * that would be needed at test run time.
     * The added test's name will be synthesized as the demangled typename of T.
     * At test run time, the test will be created and run with `T(args...).run()`.
     */
    template <typename T>
    void add(SourceLocation loc = MONGO_SOURCE_LOCATION()) {
        _add<T>(loc);
    }
    template <typename T>
    void add(auto a0, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
        _add<T>(loc, a0);
    }
    template <typename T>
    void add(auto a0, auto a1, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
        _add<T>(loc, a0, a1);
    }

private:
    /** A Gtest that runs an old style test as its `TestBody`. */
    template <typename T>
    class OldStyleTest : public Test {
    public:
        explicit OldStyleTest(auto... args) : _t(std::move(args)...) {}

        void TestBody() override {
            _t.run();
        }

    private:
        T _t;
    };

    template <typename T, typename... As>
    void _add(SourceLocation loc, As... args) {
        // Gtest requires that a suite's tests all have the same base class,
        // so they're all registered as `Test*`, since we have nothing more
        // specific to use.
        testing::RegisterTest(_name.c_str(),
                              demangleName(typeid(T)).c_str(),
                              nullptr,
                              nullptr,
                              loc.file_name(),
                              loc.line(),
                              [args...]() -> Test* {
                                  if constexpr (std::is_base_of_v<Test, T>) {
                                      return new T(args...);
                                  } else {
                                      return new OldStyleTest<T>(args...);
                                  }
                              });
    }

    std::string _name;
};

/**
 * Define a namespace-scope instance of `OldStyleSuiteInitializer<T>` to properly create and
 * initialize an instance of `T` (an `OldStyleSuiteSpecification`). See
 * `OldStyleSuiteSpecification`.
 */
template <typename T>
struct OldStyleSuiteInitializer {
    template <typename... Args>
    explicit OldStyleSuiteInitializer(Args&&... args) {
        T(std::forward<Args>(args)...).setupTests();
    }
};

using TestAssertionFailureException = testing::AssertionException;

/**
 * Returns the test info of the test currently executing.
 */
inline const testing::TestInfo* getTestInfo() {
    return testing::UnitTest::GetInstance()->current_test_info();
}

/**
 * Returns the suite name of the test currently executing.
 */
inline StringData getSuiteName() {
    return getTestInfo()->test_suite_name();
}

/**
 * Returns the name of the test currently executing.
 */
inline StringData getTestName() {
    return getTestInfo()->name();
}

/** Invocation info (used e.g. by death test to exec). */
struct SpawnInfo {
    /** Copy of the original `argv` from main. */
    std::vector<std::string> argVec;
    /** If nonempty, this process is a death test respawn. */
    std::string internalRunDeathTest;
    /**
     * A unit test main has to turn this on to indicate that it can be brought to
     * the death test from a fresh exec with `--suite` and `--filter` options.
     * Otherwise death tests simply fork. See death_test.cpp.
     */
    bool deathTestExecAllowed = false;
};
SpawnInfo& getSpawnInfo();

struct AutoUpdateConfig {
    bool updateFailingAsserts = false;
    bool revalidateAll = false;
    boost::filesystem::path executablePath;
};

AutoUpdateConfig& getAutoUpdateConfig();

}  // namespace mongo::unittest

/**
 * Defines a gtest-compatible printer for boost::optional in the boost namespace so that it's
 * discoverable where used.
 * TODO(gtest): We should consider putting this in gtest/internal/custom/gtest-printers.h.
 */
namespace boost {
template <typename T>
void PrintTo(const boost::optional<T>& value, std::ostream* os) {
    *os << '(';
    if (!value) {
        *os << "none";
    } else {
        *os << ::testing::PrintToString(*value);
    }
    *os << ')';
}
inline void PrintTo(decltype(boost::none), std::ostream* os) {
    *os << "(none)";
}
}  // namespace boost
