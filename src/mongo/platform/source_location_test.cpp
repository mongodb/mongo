/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

namespace m = unittest::match;

TEST(SourceLocation, CorrectLineNumber) {
    ASSERT_EQ(MONGO_SOURCE_LOCATION().line(), __LINE__);
}

TEST(SourceLocation, InlineVariable) {
    SourceLocation a = MONGO_SOURCE_LOCATION();
    SourceLocation b = MONGO_SOURCE_LOCATION();
    SourceLocation c = MONGO_SOURCE_LOCATION();
    ASSERT_EQ(b.line(), a.line() + 1);
    ASSERT_EQ(c.line(), a.line() + 2);
}

constexpr SourceLocation functionLocation() {
    return MONGO_SOURCE_LOCATION();
}

TEST(SourceLocation, LocalFunction) {
    auto f1 = functionLocation();
    auto f2 = functionLocation();
    ASSERT_EQ(f2.line(), f1.line());
}

TEST(SourceLocation, HeaderFunction) {
    auto h1 = makeHeaderSourceLocation_forTest();
    auto h2 = makeHeaderSourceLocation_forTest();
    ASSERT_EQ(h1.file_name(), h2.file_name());
    ASSERT_EQ(h1.line(), h2.line());
    ASSERT_NE(h1.file_name(), MONGO_SOURCE_LOCATION().file_name());
}

constexpr auto gLoc = MONGO_SOURCE_LOCATION();
constexpr auto gLocLine = __LINE__ - 1;

TEST(SourceLocation, GlobalVariable) {
    ASSERT_EQ(gLoc.file_name(), MONGO_SOURCE_LOCATION().file_name());
    ASSERT_EQ(gLoc.line(), gLocLine);
}

/*
 * The MSVC version we're dealing with right now is 14.31.31103 -
 * "Visual Studio 2019 - 14.30". It gets current source location
 * wrong in some ways. These tests confirm its incorrect behavior.
 * This condition can be adjusted when we upgrade to a better MSVC.
 * XCode has the same problem.
 */
#if defined(_MSC_VER) || defined(__apple_build_version__)
constexpr bool wrongLocation = true;
#else
constexpr bool wrongLocation = false;
#endif

TEST(SourceLocation, DefaultStructMember) {
    struct Obj {
        Obj() = default;
        unsigned ctorLine = __LINE__ - 1;
        SourceLocation loc = MONGO_SOURCE_LOCATION();
        unsigned memberLine = __LINE__ - 1;
    };
    Obj o{};
    // Some compilers incorrectly choose site of the data member definition.
    ASSERT_EQ(o.loc.line(), wrongLocation ? o.memberLine : o.ctorLine);
}

int someFunctionSourceLine = __LINE__ + 1;
SourceLocation someFunction(SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    return loc;
}

TEST(SourceLocation, FunctionReportsCaller) {
    auto reported = someFunction().line();
    auto callSiteLine = __LINE__ - 1;
    // Some compilers incorrectly choose the function source line.
    ASSERT_EQ(reported, wrongLocation ? someFunctionSourceLine : callSiteLine);
}

struct SomeClass {
    static constexpr int ctorSiteLine = __LINE__ + 1;
    explicit SomeClass(SourceLocation loc = MONGO_SOURCE_LOCATION()) : loc{loc} {}
    SourceLocation loc;
};

TEST(SourceLocation, ConstructorReportsCaller) {
    auto reported = SomeClass{}.loc.line();
    auto callSiteLine = __LINE__ - 1;
    // Some compilers incorrectly choose the ctor source line.
    ASSERT_EQ(reported, wrongLocation ? SomeClass::ctorSiteLine : callSiteLine);
}

#define CALL_MONGO_SOURCE_LOCATION() MONGO_SOURCE_LOCATION()

TEST(SourceLocation, Macro) {
    auto a = MONGO_SOURCE_LOCATION();
    auto b = CALL_MONGO_SOURCE_LOCATION();
    ASSERT_EQ(b.line(), a.line() + 1);
}

TEST(SourceLocation, Constexpr) {
    constexpr auto a = MONGO_SOURCE_LOCATION();
    [[maybe_unused]] constexpr std::tuple allConstexprs{functionLocation(),
                                                        makeHeaderSourceLocation_forTest(),
                                                        a.file_name(),
                                                        a.line(),
                                                        a.column(),
                                                        a.function_name()};
}

TEST(SourceLocation, ToString) {
    auto synth = [](auto&&... args) {
        return SyntheticSourceLocation(args...);
    };
    ASSERT_THAT(toString(MONGO_SOURCE_LOCATION()),
                m::ContainsRegex(R"re((\S+):(\d+):(\d+):(.*))re"));
    ASSERT_EQ(toString(SourceLocation{}), "(unknown location)");
    ASSERT_EQ(toString(synth("f.c", 0, "f", 34)), "(unknown location)");
    ASSERT_EQ(toString(synth("f.c", 12, "f", 34)), "f.c:12:34:f");
    ASSERT_EQ(toString(synth("f.c", 12, "f", 0)), "f.c:12:f");
    ASSERT_EQ(toString(synth("f.c", 12, "", 34)), "f.c:12:34");
    ASSERT_EQ(toString(synth("f.c", 12, "", 0)), "f.c:12");
}

TEST(SourceLocation, Formatting) {
    auto loc = MONGO_SOURCE_LOCATION();
    auto s = toString(loc);
    {
        std::ostringstream oss;
        oss << loc;
        ASSERT_EQ(oss.str(), s);
    }
    ASSERT_EQ(fmt::format("{}", loc), s);
}

TEST(SourceLocation, Logging) {
    auto loc = MONGO_SOURCE_LOCATION();
    auto s = toString(loc);
    unittest::LogCaptureGuard logs;
    LOGV2(9922300, "", "location"_attr = loc);
    auto lines = logs.getBSON();
    ASSERT_EQ(lines.size(), 1);
    ASSERT_STRING_CONTAINS(lines.front()["attr"]["location"].String(), s);
}

}  // namespace
}  // namespace mongo
