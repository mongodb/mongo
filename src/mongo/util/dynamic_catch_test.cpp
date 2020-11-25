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

#include "mongo/util/dynamic_catch.h"

#include <boost/exception/diagnostic_information.hpp>
#include <boost/exception/exception.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/source_location.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

using namespace fmt::literals;

struct TestForeignRootException {
    explicit TestForeignRootException(std::string what) : _what{std::move(what)} {}
    const char* what() const noexcept {
        return _what.c_str();
    }
    std::string _what;
};

class SpecificStdException : public std::exception {
public:
    explicit SpecificStdException(std::string what) : _what{std::move(what)} {}
    const char* what() const noexcept override {
        return _what.c_str();
    }
    std::string _what;
};

class DynamicCatchTest : public unittest::Test {
public:
    /** This is the type our terminate handler uses, so it gets extra testing attention. */
    using StreamDynCatch = DynamicCatch<std::ostream&>;

    struct SomeException {
        std::string msg;
    };

    void installSomeHandlers(StreamDynCatch& dc) {
        dc.addCatch<std::exception>(
            [](auto&& ex, std::ostream& os) { os << "std::exception: " << redact(ex.what()); });
        dc.addCatch<boost::exception>([](auto&& ex, std::ostream& os) {
            os << "boost::exception: " << boost::diagnostic_information(ex);
        });
        dc.addCatch<DBException>(
            [](auto&& ex, std::ostream& os) { os << "DBException::toString(): " << redact(ex); });
        dc.addCatch<TestForeignRootException>(
            [](auto&& ex, std::ostream& os) { os << "TestForeignRootException: " << ex.what(); });
        dc.addCatch<SpecificStdException>(
            [](auto&& ex, std::ostream& os) { os << "SpecificStdException: " << ex.what(); });
    }
};

TEST_F(DynamicCatchTest, NoHandlers) {
    try {
        struct Uncatchable {};
        throw Uncatchable{};
    } catch (...) {
        std::ostringstream os;
        try {
            StreamDynCatch{}.doCatch(os);
            FAIL("expected a throw");
        } catch (...) {
        }
        ASSERT_EQ(os.str(), "");
    }
}

TEST_F(DynamicCatchTest, Nesting) {
    // Test that later entries in the handler chain bind more tightly.
    struct Base {};
    struct Derived : Base {};
    auto trial = [](const std::vector<std::function<void(StreamDynCatch&)>>& configChain) {
        try {
            throw Derived{};
        } catch (...) {
            StreamDynCatch dc;
            for (auto&& config : configChain)
                config(dc);
            std::ostringstream os;
            dc.doCatch(os);
            return os.str();
        }
    };
    auto catchDerived = [](StreamDynCatch& dc) {
        dc.addCatch<Derived>([](auto&&, std::ostream& os) { os << "caught:Derived"; });
    };
    auto catchBase = [](StreamDynCatch& dc) {
        dc.addCatch<Base>([](auto&&, std::ostream& os) { os << "caught:Base"; });
    };
    ASSERT_STRING_CONTAINS(trial({catchBase, catchDerived}), "caught:Derived");
    ASSERT_STRING_CONTAINS(trial({catchDerived, catchBase}), "caught:Base");
}

TEST_F(DynamicCatchTest, RealisticScenarios) {
    auto trial = [&](SourceLocationHolder loc, auto&& f, std::string expected) {
        try {
            f();
            invariant(false, "`f` didn't throw");
        } catch (...) {
            std::ostringstream os;
            StreamDynCatch dc;
            installSomeHandlers(dc);
            dc.doCatch(os);
            ASSERT_STRING_SEARCH_REGEX(os.str(), expected) << " loc: " << loc;
        }
    };
#define LOC MONGO_SOURCE_LOCATION()
    trial(LOC, [] { throw TestForeignRootException{"oops"}; }, "TestForeignRootException: oops");
    trial(LOC, [] { throw std::out_of_range{"testRange"}; }, "testRange");
    trial(LOC, [] { throw SpecificStdException{"oops"}; }, "SpecificStdException: oops");
    trial(LOC, [] { uasserted(ErrorCodes::UnknownError, "test"); }, "UnknownError.*test");
#undef LOC
}

TEST_F(DynamicCatchTest, NoExtraArgs) {
    DynamicCatch<> dc;
    std::string capture = ">";
    dc.addCatch<SomeException>([&](const auto& ex) { capture += "({})"_format(ex.msg); });
    try {
        throw SomeException{"oops"};
    } catch (const SomeException&) {
        dc.doCatch();
    }
    ASSERT_EQ(capture, ">(oops)");
}

TEST_F(DynamicCatchTest, MultipleArgs) {
    std::vector<std::string> events;
    DynamicCatch<std::vector<std::string>&, const std::string&> dc;
    dc.addCatch<SomeException>(
        [](const auto& ex, std::vector<std::string>& vec, const std::string& id) {
            vec.push_back("{{{}:{}}}"_format(id, ex.msg));
        });
    try {
        throw SomeException{"oops"};
    } catch (const SomeException&) {
        dc.doCatch(events, "here");
        dc.doCatch(events, "andHere");
    }
    ASSERT_EQ("[{}]"_format(fmt::join(events, ",")), "[{here:oops},{andHere:oops}]");
}

DEATH_TEST_REGEX(DynamicCatchDeath,
                 FatalTestAssertFromStdxThread,
                 "1 == 2.*from stdx::thread.*TestAssertionFailureException") {
    stdx::thread([] { ASSERT_EQ(1, 2) << "from stdx::thread"; }).join();
}

DEATH_TEST_REGEX(DynamicCatchDeath,
                 NoActiveException,
                 R"re(terminate\(\) called\. No exception is active)re") {
    std::ostringstream os;
    DynamicCatch<std::ostream&> dc;
    dc.doCatch(os);
}

}  // namespace
}  // namespace mongo
