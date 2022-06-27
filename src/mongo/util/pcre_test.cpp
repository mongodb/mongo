/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/util/pcre.h"

#include <fmt/format.h>

#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"

namespace mongo::pcre {
namespace {
using namespace fmt::literals;
using namespace std::string_literals;
using namespace unittest::match;

/**
 * In C++20, u8 literals yield char8_t[N].
 * These require explicit conversion to `std::string` and `StringData`.
 */
template <typename Out, typename Ch, size_t N>
Out u8Cast(const Ch (&in)[N]) {
    const Ch* inp = in;
    auto cp = reinterpret_cast<const char*>(inp);
    return Out{cp, cp + N - 1};
}

TEST(PcreTest, GoodPatterns) {
    const char* goodPatterns[] = {
        "hi",
        "h(i)",
    };
    for (auto p : goodPatterns)
        ASSERT_TRUE(!!Regex{p});
}

TEST(PcreTest, BadPatterns) {
    struct {
        std::string in;
        std::error_code err;
    } badPatterns[]{
        {"h(", Errc::ERROR_MISSING_CLOSING_PARENTHESIS},
        {"h)", Errc::ERROR_UNMATCHED_CLOSING_PARENTHESIS},
        {"h\\", Errc::ERROR_END_BACKSLASH},
    };
    for (auto [in, err] : badPatterns) {
        Regex re{in};
        ASSERT_FALSE(!!re);
        ASSERT_EQ(re.error(), err);
    }
}

TEST(PcreTest, RegexCopyConstruct) {
    Regex abc("ab*c");
    Regex re = abc;
    ASSERT(re);
    ASSERT_EQ(re.pattern(), "ab*c");
    ASSERT_TRUE(re.matchView("abbbc"));
    ASSERT_FALSE(re.matchView("def"));
}

TEST(PcreTest, RegexCopyAssign) {
    Regex re("ab*c");
    Regex def("de*f");
    ASSERT_TRUE(re);
    ASSERT_TRUE(re.matchView("abbbc"));
    ASSERT_FALSE(re.matchView("deeef"));
    re = def;
    ASSERT_TRUE(def);
    ASSERT_TRUE(re);
    ASSERT_FALSE(re.matchView("abbbc"));
    ASSERT_TRUE(re.matchView("deeef"));
}

TEST(PcreTest, RegexMoveConstruct) {
    Regex src("ab*c");
    Regex dst = std::move(src);
    ASSERT_TRUE(dst);
    ASSERT_TRUE(dst.matchView("abbbc"));
    ASSERT_FALSE(dst.matchView("def"));
}

TEST(PcreTest, RegexMoveAssign) {
    Regex dst("ab*c");
    Regex src("de*f");
    dst = std::move(src);  // move-assign
    ASSERT_TRUE(dst);
    ASSERT_FALSE(dst.matchView("abbbc"));
    ASSERT_TRUE(dst.matchView("deeef"));
    {
        // Moved-from Regex can be assigned to, and can be destroyed.
        Regex other("gh*i");
        src = std::move(other);
        ASSERT_TRUE(src);
        ASSERT_FALSE(src.matchView("deeef"));
        ASSERT_TRUE(src.matchView("ghhhi"));
    }
}

TEST(PcreTest, CodeSize) {
    auto reSize = [](std::string p) { return Regex{std::move(p)}.codeSize(); };
    ASSERT_LT(reSize(""), reSize("hi"));
    ASSERT_LT(reSize("hi"), reSize("^(hi)*|(\\d{45})$"));
}

TEST(PcreTest, MatchView) {
    Regex re{"hi"};
    ASSERT_EQ(re.matchView("hi").error(), std::error_code{});
    ASSERT_EQ(re.matchView("hello").error(), Errc::ERROR_NOMATCH);
    ASSERT_EQ(re.matchView("thigh").error(), std::error_code{});
}

// While `match` copies the input and results refer to the copy, `matchView`
// results refer to the input directly.
TEST(PcreTest, MatchDataInputStorage) {
    Regex re{"hi"};
    const std::string in = "i";
    ASSERT_NE(re.match(in).input().rawData(), in.data());
    ASSERT_EQ(re.matchView(in).input().rawData(), in.data());
}

TEST(PcreTest, StartPos) {
    Regex hiRe{"hi"};
    Regex hiRePrefix{"^hi"};
    StringData ohi = "ohi"_sd;
    StringData hi = ohi.substr(1);

    ASSERT_TRUE(hiRe.matchView(hi, {}, 0));
    ASSERT_FALSE(hiRe.matchView(hi, {}, 1));

    ASSERT_TRUE(hiRe.matchView(ohi, {}, 0));
    ASSERT_TRUE(hiRe.matchView(ohi, {}, 1));

    // PCRE2 checks the startPos range internally.
    ASSERT_EQ(hiRe.matchView(ohi, {}, 3).error(), Errc::ERROR_NOMATCH);
    ASSERT_EQ(hiRe.matchView(ohi, {}, 4).error(), Errc::ERROR_BADOFFSET);

    // Using startPos=1 is different from startPos=0 on a substring.
    ASSERT_TRUE(hiRePrefix.matchView(hi, {}, 0));
    ASSERT_FALSE(hiRePrefix.matchView(ohi, {}, 1));

    // `MatchData` retains the `startPos` from the match call.
    for (size_t i = 0; i != ohi.size(); ++i)
        ASSERT_EQ(hiRe.matchView(ohi, {}, i).startPos(), i) << " i="_format(i);
}

TEST(PcreTest, CompileOptions) {
    std::string pattern = "a.b";
    std::array subjects{"a\nb"s, "A_b"s, "A\nb"s};
    struct Spec {
        CompileOptions opt;
        std::array<bool, 3> outMatch;
    };
    for (auto&& [opt, outMatch] : {
             Spec{{}, {0, 0, 0}},                 //
             Spec{DOTALL, {1, 0, 0}},             //
             Spec{CASELESS, {0, 1, 0}},           //
             Spec{DOTALL | CASELESS, {1, 1, 1}},  //
         }) {
        Regex re{pattern, opt};
        for (size_t i = 0; i < subjects.size(); ++i)
            ASSERT_EQ(!!re.matchView(subjects[i], pcre::ANCHORED | pcre::ENDANCHORED), outMatch[i])
                << "opt={}, subject={}"_format(uint32_t(opt), subjects[i]);
    }
}

TEST(PcreTest, CaptureCount) {
    auto count = [](std::string p) {
        Regex re(std::move(p));
        ASSERT_TRUE(!!re) << errorMessage(re.error());
        return re.captureCount();
    };
    ASSERT_EQ(count("hi"), 0) << "none";
    ASSERT_EQ(count("()"), 1) << "empty";
    ASSERT_EQ(count("a(b*)c"), 1) << "single";
    ASSERT_EQ(count("(\\d*):(\\w*)"), 2) << "sequential";
    ASSERT_EQ(count("(\\d*|(b*))c"), 2) << "nested";
    ASSERT_EQ(count("a(?:b|c)d"), 0) << "solely non-capturing group";
    ASSERT_EQ(count("a(?:b|(?:c*))d"), 0) << "multiple non-capturing groups";
    ASSERT_EQ(count("a(?:b|(c*))d"), 1) << "mix of capturing and non-capturing groups";
}

TEST(PcreTest, Captures) {
    Regex re("a(b*)c");
    ASSERT_EQ(re.captureCount(), 1);
    auto subject = "123abbbc456"_sd;
    auto m = re.matchView(subject);
    ASSERT_EQ(m.captureCount(), 1);
    ASSERT_TRUE(!!m);
    ASSERT_EQ(m[0], "abbbc");
    ASSERT_EQ(m[0].rawData(), subject.rawData() + 3);
    ASSERT_EQ(m[1], "bbb");
    ASSERT_EQ(m[1].rawData(), subject.rawData() + 4);
    ASSERT_THROWS(m[2], ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST(PcreTest, SkippedCapture) {
    Regex re("the ((red|white) (king|queen))");
    ASSERT_THAT(re.matchView("the red queen").getMatchList(),
                ElementsAre(Eq("the red queen"), Eq("red queen"), Eq("red"), Eq("queen")));
    // Same, but second capture group is skipped.
    Regex reWithSkip("the ((?:red|white) (king|queen))");
    ASSERT_THAT(reWithSkip.matchView("the white queen").getMatchList(),
                ElementsAre(Eq("the white queen"), Eq("white queen"), Eq("queen")));
    ASSERT_THAT(reWithSkip.matchView("the red queen").getMatchList(),
                ElementsAre(Eq("the red queen"), Eq("red queen"), Eq("queen")));
}

TEST(PcreTest, UnusedLastCapture) {
    Regex re("(a)|(b)");
    auto m = re.match("a");
    ASSERT_THAT(m.getMatchList(), ElementsAre(Eq("a"), Eq("a"), Eq("")));
}

TEST(PcreTest, NullCapture) {
    static constexpr auto sb = "b"_sd;
    ASSERT_THAT(Regex("(a*)b").matchView(sb)[1].rawData(), Eq(sb.rawData())) << "Empty";
    ASSERT_THAT(Regex("(?:b|(a))").matchView(sb)[1].rawData(), Eq(nullptr)) << "Null";
}

TEST(PcreTest, CapturesByName) {
    Regex re("a(?P<bees>b*)c");
    ASSERT_EQ(re.captureCount(), 1);
    auto subject = "123abbbc456"_sd;
    auto m = re.matchView(subject);
    ASSERT_TRUE(!!m);
    ASSERT_EQ(m[1], "bbb");
    ASSERT_THROWS(m[2], ExceptionFor<ErrorCodes::NoSuchKey>);
    ASSERT_EQ(m["bees"], "bbb");
    ASSERT_THROWS(m["seas"], ExceptionFor<ErrorCodes::NoSuchKey>);
}

TEST(PcreTest, Utf) {
    StringData subject = u8Cast<StringData>(u8"é");
    ASSERT_EQ(subject, "\xc3\xa9"_sd);
    ASSERT_TRUE(Regex("^..$").matchView(subject)) << "é is 2 bytes";
    ASSERT_TRUE(Regex("^.$", UTF).matchView(subject)) << "é is 1 UTF-8 character";
}

TEST(PcreTest, BadUtfEncoding) {
    // The UTF_ERR codes are obscure.
    // See https://www.pcre.org/current/doc/html/pcre2unicode.html
    Regex re("^.$", UTF);
    ASSERT_TRUE(!!re);
    struct Spec {
        std::string in;
        std::error_code err;
    } specs[] = {
        {"\xbf", Errc::ERROR_UTF8_ERR20},                  // isolated bit7 code point
        {"\x80", Errc::ERROR_UTF8_ERR20},                  // isolated bit7 code point
        {"\xfe", Errc::ERROR_UTF8_ERR21},                  // invalid byte value
        {"\xff", Errc::ERROR_UTF8_ERR21},                  // invalid byte value
        {"\xef\xbf\xbf", {}},                              // (U+ffff)
        {"\xf8\xa0\x8f\xbf\xbf", Errc::ERROR_UTF8_ERR11},  // (U+10ffff) no 5-byte codes
        {"\xd0\x80", {}},
        {"\xe8\x80\x80", {}},
        {"\xf4\x80\x80\x80", {}},
        // missing 1 code point
        {"\xc0", Errc::ERROR_UTF8_ERR1},
        {"\xe0\x80", Errc::ERROR_UTF8_ERR1},
        {"\xf0\x80\x80", Errc::ERROR_UTF8_ERR1},
        {"\xf8\x80\x80\x80", Errc::ERROR_UTF8_ERR1},
        {"\xfc\x80\x80\x80\x80", Errc::ERROR_UTF8_ERR1},
        // missing 2 code points
        {"\xe0", Errc::ERROR_UTF8_ERR2},
        {"\xf0\x80", Errc::ERROR_UTF8_ERR2},
        {"\xf8\x80\x80", Errc::ERROR_UTF8_ERR2},
        {"\xfc\x80\x80\x80", Errc::ERROR_UTF8_ERR2},
        // missing 3 code points
        {"\xf0", Errc::ERROR_UTF8_ERR3},
        {"\xf8\x80", Errc::ERROR_UTF8_ERR3},
        {"\xfc\x80\x80", Errc::ERROR_UTF8_ERR3},
        // missing 4 code points
        {"\xf8", Errc::ERROR_UTF8_ERR4},
        {"\xfc\x80", Errc::ERROR_UTF8_ERR4},
        // missing 5 code points
        {"\xfc", Errc::ERROR_UTF8_ERR5},
        // Emoji ?
        {u8Cast<std::string>(u8"🍌"), {}},
    };
    for (auto&& [in, err] : specs) {
        ASSERT_EQ(re.matchView(in).error(), err);
    }
}

std::string subst(std::string re, StringData rep, std::string subject, MatchOptions options = {}) {
    Regex{std::move(re)}.substitute(rep, &subject, options);
    return subject;
}

TEST(PcreTest, Substitute) {
    ASSERT_EQ(subst("funky", "pretty", "I feel funky funky."),  //
              "I feel pretty funky.");
    ASSERT_EQ(subst("funky", "pretty", "I feel funky funky.", SUBSTITUTE_GLOBAL),  //
              "I feel pretty pretty.");
    ASSERT_EQ(subst("a(b*)c", "A${1}C", "_abbbc_"), "_AbbbC_");
}

TEST(PcreTest, SubstituteFlags) {
    std::string re = R"re(\[(\w+):(\w+):(\w+)\])re";
    StringData repl = "$3 $2 $1";
    std::string str = "The [fox:brown:quick] jumped over [dog:lazy:the].";
    ASSERT_EQ(subst(re, repl, str),  //
              "The quick brown fox jumped over [dog:lazy:the].");
    ASSERT_EQ(subst(re, repl, str, SUBSTITUTE_LITERAL),  //
              "The $3 $2 $1 jumped over [dog:lazy:the].");
    ASSERT_EQ(subst(re, repl, str, SUBSTITUTE_GLOBAL),  //
              "The quick brown fox jumped over the lazy dog.");
    ASSERT_EQ(subst(re, repl, str, SUBSTITUTE_GLOBAL | SUBSTITUTE_LITERAL),  //
              "The $3 $2 $1 jumped over $3 $2 $1.");
}

}  // namespace
}  // namespace mongo::pcre
