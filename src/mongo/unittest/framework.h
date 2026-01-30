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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/platform/source_location.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/ctype.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

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
    template <typename T>
    void add(auto a0, auto a1, auto a2, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
        _add<T>(loc, a0, a1, a2);
    }
    template <typename T>
    void add(auto a0, auto a1, auto a2, auto a3, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
        _add<T>(loc, a0, a1, a2, a3);
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
    std::string _genCaseName(const As&... args) {
        std::string name = demangleName(typeid(T));

        // Remove redundant common prefix between this suite's typename and the
        // typename of the test case being added to it.
        std::string suite = demangleName(typeid(*this));
        auto cut = std::mismatch(suite.begin(), suite.end(), name.begin(), name.end()).second;
        // Don't chop in the middle of an identifier.
        for (auto prev = cut; cut != name.begin(); cut = prev) {
            prev = std::prev(prev);
            if (!ctype::isAlnum(*prev) && *prev != '_')
                break;
        }
        name.erase(name.begin(), cut);
        for (size_t pos; (pos = name.find("::")) != std::string::npos;)
            name.replace(pos, 2, ".");
        (name.append(fmt::format("/{}", args)), ...);
        return name;
    }

    template <typename T, typename... As>
    void _add(SourceLocation loc, As... args) {
        // Gtest requires that a suite's tests all have the same base class,
        // so they're all registered as `Test*`, since we have nothing more
        // specific to use.
        testing::RegisterTest(_name.c_str(),
                              _genCaseName<T>(args...).c_str(),
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

/**
 * Expose an interface to Googletest's universal printer,
 * which is necessary for composition of its `PrintTo` hook,
 * but is unfortunately "internal". We should always use this
 * wrapper instead of directly calling that internal function.
 */
void universalPrint(const auto& v, std::ostream& os) {
    testing::internal::UniversalTersePrint(v, &os);
}

/**
 * It's configurable what Gmock will do with an "uninteresting"
 * call. That is, a call to a function that has no `EXPECT_CALL`
 * set upon it.
 *
 * Gmock behavior is "naggy" by default. Our wrapper changes this to "nice",
 * which is actually preferred by Gmock's docs despite not being the default.
 * Temporarily setting the default mock behavior to "naggy" during development
 * can be very useful instrumentation.
 *
 * For more information on the behaviors, see
 * https://google.github.io/googletest/gmock_cook_book.html#NiceStrictNaggy
 *
 * This default can be observed and adjusted on a per-test basis with
 * `getDefaultMockBehavior` and `setDefaultMockBehavior`.
 *
 * The adjustments rely on an undocumented `GMOCK_FLAG`, so we're exporting
 * this facility to our framework wrapper as an abstraction.
 */
enum class MockBehavior {
    nice,    ///< The call is silently accepted
    naggy,   ///< The call is warned about
    strict,  ///< The call induces test failure
};

MockBehavior getDefaultMockBehavior();

/**
 * Changes to this behavior are reset after each test case.
 * Internally it's a Gmock flag, so it is saved and restored by FlagSaver.
 * It's not necessary to reset the behavior after each test case.
 */
void setDefaultMockBehavior(MockBehavior behavior);

}  // namespace mongo::unittest

namespace mongo {
/**
 * A gtest printer for `mongo::StringData`.
 * Renders it as if it was a `std::string_view`.
 * https://google.github.io/googletest/advanced.html#teaching-googletest-how-to-print-your-values
 */
inline void PrintTo(StringData s, std::ostream* os) {
    unittest::universalPrint(toStdStringViewForInterop(s), *os);
}

inline void PrintTo(const Status& s, std::ostream* os) {
    *os << s.toString();
}

template <typename T>
inline void PrintTo(const StatusWith<T>& s, std::ostream* os) {
    if (s.isOK()) {
        *os << ::testing::PrintToString(s.getValue());
    } else {
        *os << ::testing::PrintToString(s.getStatus());
    }
}
}  // namespace mongo

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
