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


#include "mongo/util/base64.h"

#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"

#include <cstdint>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

TEST(Base64Test, transcode) {
    struct {
        int line;
        std::string_view plain;
        std::string_view encoded;
        std::string_view encodedUrl;
    } const tests[] = {
        {__LINE__, ""sv, ""sv},
        {__LINE__, "a"sv, "YQ=="sv, "YQ"sv},
        {__LINE__, "aa"sv, "YWE="sv, "YWE"sv},
        {__LINE__, "aaa"sv, "YWFh"sv, "YWFh"sv},
        {__LINE__, "aaaa"sv, "YWFhYQ=="sv, "YWFhYQ"sv},

        {__LINE__, "A"sv, "QQ=="sv, "QQ"sv},
        {__LINE__, "AA"sv, "QUE="sv, "QUE"sv},
        {__LINE__, "AAA"sv, "QUFB"sv, "QUFB"sv},
        {__LINE__, "AAAA"sv, "QUFBQQ=="sv, "QUFBQQ"sv},

        {__LINE__,
         "The quick brown fox jumped over the lazy dog."sv,
         "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2cu"sv,
         "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2cu"sv},
        {__LINE__, "\0\1\2\3\4\5\6\7"sv, "AAECAwQFBgc="sv, "AAECAwQFBgc"sv},
        {__LINE__, "\0\277\1\276\2\275"sv, "AL8BvgK9"sv, "AL8BvgK9"sv},

        {__LINE__, "\x7E\x8A\x3E\xFD\xB6\xAB"sv, "foo+/bar"sv, "foo-_bar"sv},
    };

    for (const auto& t : tests) {
        ASSERT_TRUE(base64::validate(t.encoded)) << t.line;
        ASSERT_EQUALS(base64::encode(t.plain), t.encoded)
            << "line: " << t.line << ", plain: '" << t.plain << "'";
        ASSERT_EQUALS(base64::decode(t.encoded), t.plain)
            << "line: " << t.line << ", encoded: '" << t.encoded << "'";

        ASSERT_TRUE(base64url::validate(t.encodedUrl)) << t.line;
        ASSERT_EQUALS(base64url::encode(t.plain), t.encodedUrl)
            << "line: " << t.line << ", plain: '" << t.plain << "'";
        ASSERT_EQUALS(base64url::decode(t.encodedUrl), t.plain)
            << "line: " << t.line << ", encoded: '" << t.encoded << "'";
    }
}

static constexpr bool kSuperVerbose = false;  // devel instrumentation

TEST(Base64Test, encodeAllPossibleGroups) {
    std::string buf;
    for (int sz = 1; sz < 3; ++sz) {
        buf.resize(sz);
        for (std::uint32_t q = 0; q < (1u << (8 * sz)); ++q) {
            for (int k = 0; k < sz; ++k) {
                buf[k] = (q >> (8 * k)) & 0xff;
            }
            std::string s = base64::encode(buf);
            ASSERT_EQ(s.size(), 4);
            if (kSuperVerbose) {
                LOGV2(23509, "buffer", "buf"_attr = mongo::hexblob::encode(buf), "s"_attr = s);
            }
            std::string recovered = base64::decode(s);
            ASSERT_EQ(buf, recovered);

            s = base64url::encode(buf);
            ASSERT_GTE(s.size(), 2);
            ASSERT_LTE(s.size(), 4);
            if (kSuperVerbose) {
                LOGV2(6746400, "buffer", "buf"_attr = mongo::hexblob::encode(buf), "s"_attr = s);
            }
            recovered = base64url::decode(s);
            ASSERT_EQ(buf, recovered);
        }
    }
}

TEST(Base64Test, parseFail) {
    struct {
        int line;
        std::string_view encoded;
        boost::optional<int> code;
    } const tests[] = {
        {__LINE__, "BadLength"sv, 10270},
        {__LINE__, "Has Whitespace=="sv, 40537},
        {__LINE__, "Hasbadchar$="sv, 40537},
        {__LINE__, "Hasbadchar\xFF="sv, 40537},
        {__LINE__, "Hasbadchar\t="sv, 40537},
        {__LINE__, "Has-dash"sv, 40537},
        {__LINE__, "Has_Underscore=="sv, 40537},
        {__LINE__, "too=soon"sv, {}},  // fail, don't care how
    };

    for (const auto& t : tests) {
        ASSERT_FALSE(base64::validate(t.encoded)) << t.line;

        try {
            base64::decode(t.encoded);
            ASSERT_TRUE(false) << t.line;
        } catch (const AssertionException& e) {
            if (t.code) {
                ASSERT_EQ(e.code(), *t.code) << t.line << " e: " << e.toString();
            }
        }
    }
}

TEST(Base64UrlTest, parseFail) {
    struct {
        int line;
        std::string_view encoded;
        boost::optional<int> code;
    } const tests[] = {
        {__LINE__, "BadLength"sv, 10270},
        {__LINE__, "Has Whitespace=="sv, 40537},
        {__LINE__, "Hasbadchar$="sv, 40537},
        {__LINE__, "Hasbadchar\xFF="sv, 40537},
        {__LINE__, "Hasbadchar\t="sv, 40537},
        {__LINE__, "Has+plus"sv, 40537},
        {__LINE__, "Has/Solidus="sv, 40537},
        {__LINE__, "too=soon"sv, {}},  // fail, don't care how
    };

    for (const auto& t : tests) {
        ASSERT_FALSE(base64url::validate(t.encoded)) << t.line;

        try {
            base64url::decode(t.encoded);
            ASSERT_TRUE(false) << t.line;
        } catch (const AssertionException& e) {
            if (t.code) {
                ASSERT_EQ(e.code(), *t.code) << t.line << " e: " << e.toString();
            }
        }
    }
}

}  // namespace
}  // namespace mongo
