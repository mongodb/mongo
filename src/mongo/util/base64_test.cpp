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


#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/base64.h"
#include "mongo/util/hex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

TEST(Base64Test, transcode) {
    struct {
        int line;
        StringData plain;
        StringData encoded;
    } const tests[] = {
        {__LINE__, "", ""},
        {__LINE__, "a", "YQ=="},
        {__LINE__, "aa", "YWE="},
        {__LINE__, "aaa", "YWFh"},
        {__LINE__, "aaaa", "YWFhYQ=="},

        {__LINE__, "A", "QQ=="},
        {__LINE__, "AA", "QUE="},
        {__LINE__, "AAA", "QUFB"},
        {__LINE__, "AAAA", "QUFBQQ=="},

        {__LINE__,
         "The quick brown fox jumped over the lazy dog.",
         "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2cu"},
        {__LINE__, "\0\1\2\3\4\5\6\7"_sd, "AAECAwQFBgc="},
        {__LINE__, "\0\277\1\276\2\275"_sd, "AL8BvgK9"},
    };

    for (const auto& t : tests) {
        ASSERT_TRUE(base64::validate(std::string{t.encoded})) << t.line;
        ASSERT_EQUALS(base64::encode(std::string{t.plain}), t.encoded)
            << "line: " << t.line << ", plain: '" << t.plain << "'";
        ASSERT_EQUALS(base64::decode(std::string{t.encoded}), t.plain)
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
                LOGV2(23509,
                      "buf=[{buf}] s=`{s}`",
                      "buf"_attr = mongo::hexblob::encode(buf),
                      "s"_attr = s);
            }
            std::string recovered = base64::decode(s);
            ASSERT_EQ(buf, recovered);
        }
    }
}

TEST(Base64Test, parseFail) {
    struct {
        int line;
        StringData encoded;
        boost::optional<int> code;
    } const tests[] = {
        {__LINE__, "BadLength", 10270},
        {__LINE__, "Has Whitespace==", 40537},
        {__LINE__, "Hasbadchar$=", 40537},
        {__LINE__, "Hasbadchar\xFF=", 40537},
        {__LINE__, "Hasbadcahr\t=", 40537},
        {__LINE__, "too=soon", {}},  // fail, don't care how
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

}  // namespace
}  // namespace mongo
