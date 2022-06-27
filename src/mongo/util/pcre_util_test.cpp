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

#include "mongo/util/pcre_util.h"

#include <fmt/format.h>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/ctype.h"
#include "mongo/util/pcre.h"

namespace mongo::pcre_util {
namespace {

using namespace fmt::literals;

// Test compares `CompileOptions` as integers.
TEST(PcreUtilTest, FlagsToOptions) {
    using namespace pcre::options;
    auto parse = [](StringData flags) { return static_cast<uint32_t>(flagsToOptions(flags)); };
    auto expect = [](pcre::CompileOptions o) { return static_cast<uint32_t>(o); };
    ASSERT_EQ(parse(""), expect(UTF)) << " UTF is on by default";
    ASSERT_EQ(parse("i"), expect(UTF | CASELESS));
    ASSERT_EQ(parse("m"), expect(UTF | MULTILINE));
    ASSERT_EQ(parse("s"), expect(UTF | DOTALL));
    ASSERT_EQ(parse("u"), expect(UTF));
    ASSERT_EQ(parse("x"), expect(UTF | EXTENDED));
    ASSERT_EQ(parse("imsux"), expect(CASELESS | MULTILINE | DOTALL | UTF | EXTENDED));
    ASSERT_EQ(parse("xusmi"), expect(CASELESS | MULTILINE | DOTALL | UTF | EXTENDED));

    auto isBadFlagException = [](const DBException& ex) { return ex.code() == 51108; };
    ASSERT_THROWS_WITH_CHECK(parse("z"), DBException, isBadFlagException);
    ASSERT_THROWS_WITH_CHECK(parse("iz"), DBException, isBadFlagException);
}

// Test compares `CompileOptions` as strings of option flags.
TEST(PcreUtilTest, OptionsToFlags) {
    using namespace pcre::options;
    auto parse = [](pcre::CompileOptions flags) {
        return static_cast<std::string>(optionsToFlags(flags));
    };
    auto expect = [](std::string o) { return (o); };
    ASSERT_EQ(parse(UTF | CASELESS), expect("i"));
    ASSERT_EQ(parse(UTF | MULTILINE), expect("m"));
    ASSERT_EQ(parse(UTF | DOTALL), expect("s"));
    ASSERT_EQ(parse(UTF), expect("")) << " UTF is on by default";
    ASSERT_EQ(parse(UTF | EXTENDED), expect("x"));
    ASSERT_EQ(parse(UTF | CASELESS | MULTILINE | DOTALL | EXTENDED), expect("imsx"));
    ASSERT_EQ(parse(UTF | CASELESS | MULTILINE | DOTALL), expect("ims"));
    ASSERT_EQ(parse(UTF | CASELESS | MULTILINE | EXTENDED), expect("imx"));
    ASSERT_EQ(parse(UTF | CASELESS | DOTALL | EXTENDED), expect("isx"));
    ASSERT_EQ(parse(UTF | MULTILINE | DOTALL | EXTENDED), expect("msx"));
}

TEST(PcreUtilTest, QuoteMeta) {
    ASSERT_EQ(quoteMeta(""), "");
    ASSERT_EQ(quoteMeta("abc_def_123"_sd), "abc_def_123");
    ASSERT_EQ(quoteMeta("🍌"_sd), "🍌");
    ASSERT_EQ(quoteMeta("\0"_sd), "\\0") << "NUL";
    ASSERT_EQ(quoteMeta("\n"_sd), "\\\n") << "one escape";
    ASSERT_EQ(quoteMeta("a\n\nb"_sd), "a\\\n\\\nb") << "two adjacent escapes";
    ASSERT_EQ(quoteMeta("a\nb\nc"_sd), "a\\\nb\\\nc") << "two nonadjacent escapes";

    // All the single chars except '\0', which is already tested and behaves differently.
    for (int i = 1; i <= CHAR_MAX; ++i) {
        char c = i;
        StringData in(&c, 1);
        std::string out = quoteMeta(in);

        // [a-zA-Z0-9_] and bit7 chars are not escaped. Everything else is.
        bool shouldEscape = [&] {
            if (ctype::isAlnum(c))
                return false;
            if (c == '_')
                return false;
            if (static_cast<unsigned char>(c) >= 0x80)
                return false;
            return true;
        }();

        auto hexdump = [](StringData in) {
            std::string r = "[";
            StringData sep;
            for (unsigned char c : in) {
                static constexpr auto d = "0123456789abcdef"_sd;
                r += sep;
                r += d[(c >> 4) & 0xf];
                r += d[(c >> 0) & 0xf];
                sep = ",";
            }
            r += "]";
            return r;
        };
        auto note = "{} => {}"_format(hexdump(in), hexdump(out));
        if (shouldEscape) {
            ASSERT_EQ(out, "\\" + in) << note;
        } else {
            ASSERT_EQ(out, in) << note;
        }
    }
}

}  // namespace
}  // namespace mongo::pcre_util
