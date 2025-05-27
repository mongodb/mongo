/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bson_utf8.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>

namespace mongo {

/**
 * The Unicode REPLACEMENT_CHARACTER (U+FFFD).
 * https://en.wikipedia.org/wiki/Specials_(Unicode_block)#Replacement_character
 */
const std::string replacementCharacter = u8"\ufffd"_as_char_ptr;

/** Repeat the `s` string, `x` times. */
std::string repeat(StringData s, size_t x) {
    std::string result;
    result.reserve(x * s.size());
    auto it = std::back_inserter(result);
    for (size_t i = 0; i < x; ++i)
        it = std::copy(s.begin(), s.end(), it);
    return result;
}

/** Function to convert a Unicode code point to a UTF-8 encoded string */
std::string codePointToUTF8(unsigned int codePoint) {
    std::string result;
    if (codePoint <= 0x7F) {
        result += static_cast<char>(codePoint);
    } else if (codePoint <= 0x7FF) {
        result += static_cast<char>(0xC0 | (codePoint >> 6));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        result += static_cast<char>(0xE0 | (codePoint >> 12));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0x10FFFF) {
        result += static_cast<char>(0xF0 | (codePoint >> 18));
        result += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
    return result;
}

/** Double-check the encoding of the replacementCharacter */
TEST(UnicodeReplacementCharacter, Bytes) {
    ASSERT_EQ(replacementCharacter, (std::string{'\xef', '\xbf', '\xbd'}));
}

const std::vector<std::string> validUTF8Strings{
    "A",
    "\xc2\xa2",          // CENT SIGN: ¢
    "\xe2\x82\xac",      // Euro: €
    "\xf0\x9d\x90\x80",  // Blackboard A: 𝐀
    "\n",
    u8"こんにちは"_as_char_ptr,
    u8"😊"_as_char_ptr,
    "",
};

// Maps invalid UTF-8 to the appropriate scrubbed version.
const std::map<std::string, std::string> scrubMap{
    // Abrupt end
    {"\xc2", repeat(replacementCharacter, 1)},
    {"\xe2\x82", repeat(replacementCharacter, 2)},
    {"\xf0\x9d\x90", repeat(replacementCharacter, 3)},

    // Test having spaces at the end and beginning of scrubbed lines
    {" \xc2 ", " \xef\xbf\xbd "},
    {"\xe2\x82 ", "\xef\xbf\xbd\xef\xbf\xbd "},
    {"\xf0\x9d\x90 ", "\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd "},

    // Too long
    {"\xf8\x80\x80\x80\x80", repeat(replacementCharacter, 5)},
    {"\xfc\x80\x80\x80\x80\x80", repeat(replacementCharacter, 6)},
    {"\xfe\x80\x80\x80\x80\x80\x80", repeat(replacementCharacter, 7)},
    {"\xff\x80\x80\x80\x80\x80\x80\x80", repeat(replacementCharacter, 8)},

    {"\x80", repeat(replacementCharacter, 1)},         // Can't start with continuation byte.
    {"\xc3\x28", "\xef\xbf\xbd\x28"},                  // First byte indicates 2-byte sequence, but
                                                       // second byte is not in form 10xxxxxx
    {"\xe2\x28\xa1", "\xef\xbf\xbd\x28\xef\xbf\xbd"},  // first byte indicates 3-byte sequence, but
                                                       // second byte is not in form 10xxxxxx
    {"\xde\xa0\x80",
     "\xde\xa0\xef\xbf\xbd"},  // Surrogate pairs are not valid for UTF-8 (high surrogate)
    {"\xf0\x9d\xdc\x80", "\xef\xbf\xbd\xef\xbf\xbd\xdc\x80"},  // Surrogate pairs are not valid
};

void unchangedStrInput(StringData fieldName, const std::string& s) {
    auto originalBSONObj = BSON(fieldName << s);
    auto newBSONObj = checkAndScrubInvalidUTF8(originalBSONObj);
    ASSERT_BSONOBJ_EQ(newBSONObj, BSON(fieldName << s));
}

void scrubbedStrInput(StringData fieldName, const std::string& s, const std::string& scrubbedS) {
    auto originalBSONObj = BSON(fieldName << s);
    auto scrubbedBSONObj = checkAndScrubInvalidUTF8(originalBSONObj);
    ASSERT_BSONOBJ_EQ(scrubbedBSONObj, BSON(fieldName << scrubbedS));
}

void unchangedBSONInput(BSONObj originalBSONObj) {
    BSONObj newBSONObj = checkAndScrubInvalidUTF8(originalBSONObj);
    ASSERT_BSONOBJ_EQ(originalBSONObj, newBSONObj);
}

void scrubbedBSONInput(BSONObj inputBSONObj, const BSONObj scrubbedCmpBSONObj) {
    auto scrubbedBSONObj = checkAndScrubInvalidUTF8(inputBSONObj);
    ASSERT_BSONOBJ_EQ(scrubbedBSONObj, scrubbedCmpBSONObj);
}

// For some more complicated nesting, some of the scrubbing of the fieldNames doesn't fit the
// original fieldName so we just want to check that the output is validUTF8.
BSONObj scrubAndAssertUTF8Valid(BSONObj obj) {
    auto scrubbedBSONObj = checkAndScrubInvalidUTF8(obj);
    ASSERT(isValidUTF8(scrubbedBSONObj));
    // Return the scrubbedBSONObj so we can do more analysis of the scrubbedObj.
    return scrubbedBSONObj;
}

template <typename T>
BSONObj makeBSONArrayObject(StringData fieldName, const std::vector<T>& values) {
    BSONObjBuilder builder;
    {
        BSONArrayBuilder arr(builder.subarrayStart(fieldName));
        for (const T& v : values)
            arr.append(v);
    }
    return builder.obj();
}

BSONObj makeBSONCodeObject(StringData fieldName, const std::string codeStr) {
    return BSON(fieldName << BSONCode(codeStr));
}

BSONObj makeBSONCodeWScopeObject(StringData fieldName,
                                 const std::string codeStr,
                                 const BSONObj codeScope) {
    return BSON(fieldName << BSONCodeWScope(codeStr, codeScope));
}

// All BSONTypes that should not be scrubbed (no String data).
// Declared here because these BSONOBj can be used in multiple tests.
BSONObj undefinedObj = BSON("undefined" << BSONUndefined);
BSONObj objectIdObj = BSON("oid" << OID("dbdbdbdbdbdbdbdbdbdbdbdb"));
BSONObj boolObj = BSON("bool" << true);
BSONObj dateTimeObj = BSON("datetime" << DATENOW);
BSONObj nullObj = BSON("null" << BSONNULL);
BSONObj intObj = BSON("int" << 360);
BSONObj timestampObj = BSON("timestamp" << Timestamp(3, 6));
BSONObj longIntObj = BSON("longInt" << 0x0fedcba987654321ll);
BSONObj decimalObj = BSON("decimal" << Decimal128("0.365"));

TEST(checkAndScrubInvalidUTF8, SimpleScrub) {
    // Tests that valid one-layer BSON that is valid UTF-8 doesn't get scrubbed with an empty and
    // non-empty field name.
    for (auto& value : validUTF8Strings) {
        unchangedStrInput("", value);
        unchangedStrInput("hello", value);
    }

    for (unsigned i = 0; i < 0x1'0000; ++i) {
        std::string s = codePointToUTF8(i);
        unchangedStrInput("", s);
        unchangedStrInput("hello", s);
    }

    unchangedBSONInput(fromjson("{'a': '\xc2\xa2', 'b': '\xf0\x9d\x90\x80'}"));
    unchangedBSONInput(fromjson("{'\xc2\xa2': 'a', '\xf0\x9d\x90\x80': 'b'}"));

    // BSONCode with valid UTF-8 with a empty and non-empty field name.
    BSONObj validCodeWithEmptyField = makeBSONCodeObject("", "(function(){})();");
    unchangedBSONInput(validCodeWithEmptyField);

    BSONObj validCodeWithField = makeBSONCodeObject("code", "(function(){})();");
    unchangedBSONInput(validCodeWithField);

    // BSONSymbol with valid UTF-8 with an empty and non-empty field name.
    BSONObj validSymbolWithEmptyField = BSON("" << BSONSymbol("crash"));
    unchangedBSONInput(validSymbolWithEmptyField);

    BSONObj validSymbolWithField = BSON("Symbol" << BSONSymbol("crash"));
    unchangedBSONInput(validSymbolWithField);

    // BSONDBRef with valid UTF-8 with an empty and non-empty field name.
    BSONObj validDBRefWithEmptyField =
        BSON("" << BSONDBRef("lock it, unlock it", OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    unchangedBSONInput(validDBRefWithEmptyField);

    BSONObj validDBRefWithField =
        BSON("DBRef" << BSONDBRef("lock it, unlock it", OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    unchangedBSONInput(validDBRefWithField);

    // BSONRegEx with valid UTF-8 with an empty and non-empty field name.
    BSONObj validRegExWithEmptyField = BSON("" << BSONRegEx("vroom vroom", "flag nation"));
    unchangedBSONInput(validRegExWithEmptyField);

    BSONObj validRegExWithField = BSON("RegEx" << BSONRegEx("vroom vroom", "flag nation"));
    unchangedBSONInput(validRegExWithField);

    // Tests that invalid UTF-8 gets correctly scrubbed with a empty and non-empty field name.
    for (const auto& [invalidStr, scrubbedStr] : scrubMap) {
        scrubbedStrInput("", invalidStr, scrubbedStr);
        scrubbedStrInput("hello", invalidStr, scrubbedStr);
    }

    // BSONCode with invalid UTF-8 gets correctly scrubbed with a empty and non-empty field name.
    std::string invalidCode1 = "(\xc2 function(){})();";
    std::string scrubbedInvalidCode1 = "(\ufffd function(){})();";

    BSONObj invalidCodeWithEmptyField1 = makeBSONCodeObject("", invalidCode1);
    BSONObj scrubbedInvalidCodeWithEmptyField1 = makeBSONCodeObject("", scrubbedInvalidCode1);
    scrubbedBSONInput(invalidCodeWithEmptyField1, scrubbedInvalidCodeWithEmptyField1);

    BSONObj invalidCodeWithField1 = makeBSONCodeObject("code", invalidCode1);
    BSONObj scrubbedInvalidCodeWithField1 = makeBSONCodeObject("code", scrubbedInvalidCode1);
    scrubbedBSONInput(invalidCodeWithField1, scrubbedInvalidCodeWithField1);

    BSONObj invalidCodeWithEmptyField2 = makeBSONCodeObject("", "var x = \xf8\x80\x80\x80\x80;");
    BSONObj scrubbedInvalidCodeWithEmptyField2 =
        makeBSONCodeObject("", "var x = \ufffd\ufffd\ufffd\ufffd\ufffd;");
    scrubbedBSONInput(invalidCodeWithEmptyField2, scrubbedInvalidCodeWithEmptyField2);

    BSONObj invalidCodeWithField2 = makeBSONCodeObject("code", "var x = \xf8\x80\x80\x80\x80;");
    BSONObj scrubbedInvalidCodeWithField2 =
        makeBSONCodeObject("code", "var x = \ufffd\ufffd\ufffd\ufffd\ufffd;");
    scrubbedBSONInput(invalidCodeWithField2, scrubbedInvalidCodeWithField2);

    // BSONSymbol with invalid UTF-8 with an empty and non-empty field name.
    BSONObj invalidSymbolWithEmptyField =
        BSON("" << BSONSymbol("crash into the \xfc\x80\x80\x80\x80\x80"));
    BSONObj scrubbedSymbolWithEmptyField =
        BSON("" << BSONSymbol("crash into the \ufffd\ufffd\ufffd\ufffd\ufffd\ufffd"));
    scrubbedBSONInput(invalidSymbolWithEmptyField, scrubbedSymbolWithEmptyField);

    BSONObj invalidSymbolWithField =
        BSON("Symbol" << BSONSymbol("crash into the \xfc\x80\x80\x80\x80\x80"));
    BSONObj scrubbedSymbolWithField =
        BSON("Symbol" << BSONSymbol("crash into the \ufffd\ufffd\ufffd\ufffd\ufffd\ufffd"));
    scrubbedBSONInput(invalidSymbolWithField, scrubbedSymbolWithField);

    // BSONDBRef with invalid UTF-8 with an empty and non-empty field name.
    BSONObj invalidDBRefWithEmptyField =
        BSON("" << BSONDBRef("\xe2\x28\xa1 lock it, unlock it", OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    BSONObj scrubbedDBRefWithEmptyField = BSON(
        "" << BSONDBRef("\ufffd\x28\ufffd lock it, unlock it", OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    scrubbedBSONInput(invalidDBRefWithEmptyField, scrubbedDBRefWithEmptyField);

    BSONObj invalidDBRefWithField = BSON(
        "DBRef" << BSONDBRef("\xe2\x28\xa1 lock it, unlock it", OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    BSONObj scrubbedDBRefWithField =
        BSON("DBRef" << BSONDBRef("\ufffd\x28\ufffd lock it, unlock it",
                                  OID("dbdbdbdbdbdbdbdbdbdbdbdb")));
    scrubbedBSONInput(invalidDBRefWithField, scrubbedDBRefWithField);

    // BSONRegEx with invalid UTF-8 with an empty and non-empty field name for the expression.
    BSONObj invalidRegExExprWithEmptyField =
        BSON("" << BSONRegEx("vroom \xf0\x9d\x90 vroom", "flag nation"));
    BSONObj scrubbedRegExExprWithEmptyField =
        BSON("" << BSONRegEx("vroom \ufffd\ufffd\ufffd vroom", "flag nation"));
    scrubbedBSONInput(invalidRegExExprWithEmptyField, scrubbedRegExExprWithEmptyField);

    BSONObj invalidRegExExprWithField =
        BSON("RegEx" << BSONRegEx("vroom \xf0\x9d\x90 vroom", "flag nation"));
    BSONObj scrubbedRegExExprWithField =
        BSON("RegEx" << BSONRegEx("vroom \ufffd\ufffd\ufffd vroom", "flag nation"));
    scrubbedBSONInput(invalidRegExExprWithField, scrubbedRegExExprWithField);

    // BSONRegEx with valid UTF-8 with an empty and non-empty field name for the flags.
    BSONObj invalidRegExFlagsWithEmptyField =
        BSON("" << BSONRegEx("vroom vroom", "flag nation \xc2"));
    BSONObj scrubbedRegExFlagsWithEmptyField =
        BSON("" << BSONRegEx("vroom vroom", "flag nation \ufffd"));
    scrubbedBSONInput(invalidRegExFlagsWithEmptyField, scrubbedRegExFlagsWithEmptyField);

    BSONObj invalidRegExFlagsWithField =
        BSON("RegEx" << BSONRegEx("vroom vroom", "flag nation \xc2"));
    BSONObj scrubbedRegExFlagsWithField =
        BSON("RegEx" << BSONRegEx("vroom vroom", "flag nation \ufffd"));
    scrubbedBSONInput(invalidRegExFlagsWithField, scrubbedRegExFlagsWithField);

    // BSONRegEx with invalid UTF-8 with an empty and non-empty field name for the flags and the
    // expression.
    BSONObj invalidRegExWithEmptyField =
        BSON("" << BSONRegEx("vroom  vr \x80m", "flag \xe2\x82 nation"));
    BSONObj scrubbeddRegExWithEmptyField =
        BSON("" << BSONRegEx("vroom  vr \ufffdm", "flag \ufffd\ufffd nation"));
    scrubbedBSONInput(invalidRegExWithEmptyField, scrubbeddRegExWithEmptyField);

    BSONObj invalidRegExWithField =
        BSON("RegEx" << BSONRegEx("vroom  vr \x80m", "flag \xe2\x82 nation"));
    BSONObj scrubbedRegExWithField =
        BSON("RegEx" << BSONRegEx("vroom  vr \ufffdm", "flag \ufffd\ufffd nation"));
    scrubbedBSONInput(invalidRegExWithField, scrubbedRegExWithField);
}

TEST(checkAndScrubInvalidUTF8, SimpleScrubWithNonStringBSONTypes) {
    // Tests that BSONTypes that should not be scrubbed (don't contain string data) shouldn't be
    // scrubbed.
    // BSONUndefined
    BSONObj undefinedWithEmptyField = BSON("" << BSONUndefined);
    unchangedBSONInput(undefinedWithEmptyField);

    BSONObj undefinedWithField = BSON("undefined" << BSONUndefined);
    unchangedBSONInput(undefinedWithField);

    // BSONObjectId
    BSONObj objectIdWithEmptyField = BSON("" << OID("dbdbdbdbdbdbdbdbdbdbdbdb"));
    unchangedBSONInput(objectIdWithEmptyField);

    BSONObj objectIdWithField = BSON("oid" << OID("dbdbdbdbdbdbdbdbdbdbdbdb"));
    unchangedBSONInput(objectIdWithField);

    // Bool
    BSONObj boolWithEmptyField = BSON("" << true);
    unchangedBSONInput(boolWithEmptyField);
    // Bool with field declared for all tests at top of file.
    unchangedBSONInput(boolObj);

    // UTC datetime
    BSONObj dateTimeWithEmptyField = BSON("" << DATENOW);
    unchangedBSONInput(dateTimeWithEmptyField);
    // UTC datetime with field declared for all tests at top of file.
    unchangedBSONInput(dateTimeObj);

    // Null value
    BSONObj nullWithEmptyField = BSON("" << BSONNULL);
    unchangedBSONInput(nullWithEmptyField);
    // Null value with field declared for all tests at top of file.
    unchangedBSONInput(nullObj);

    // 32-bit integer
    BSONObj intWithEmptyField = BSON("" << 365);
    unchangedBSONInput(intWithEmptyField);
    // Int with field declared for all tests at top of file.
    unchangedBSONInput(intObj);

    // Timestamp
    BSONObj timestampWithEmptyField = BSON("" << Timestamp(3, 6));
    unchangedBSONInput(timestampWithEmptyField);
    // Timestamp with field declared for all tests at top of file..
    unchangedBSONInput(timestampObj);

    // 64-bit integer
    BSONObj longIntWithEmptyField = BSON("" << 0x0fedcba987654321ll);
    unchangedBSONInput(longIntWithEmptyField);
    // 64-bit integer with field declared for all tests at top of file.
    unchangedBSONInput(longIntObj);

    // 128-bit decimal floating point
    BSONObj decimalWithEmptyField = BSON("" << Decimal128("0.365"));
    unchangedBSONInput(decimalWithEmptyField);
    // 128-bit decimal floating point with field declared for all tests at top of file.
    unchangedBSONInput(decimalObj);
}

TEST(checkAndScrubInvalidUTF8, SimpleCodeWScope) {
    // BSONCodeWScope with valid UTF-8 with a empty and non-empty field name.
    std::string validCode1 = "(function(){print(x);})();";
    BSONObj validCodeWScopeWithEmptyField =
        makeBSONCodeWScopeObject("", validCode1, BSON("x" << 1));
    unchangedBSONInput(validCodeWScopeWithEmptyField);

    BSONObj validCodeWScopeWithField = makeBSONCodeWScopeObject("code", validCode1, BSON("x" << 1));
    unchangedBSONInput(validCodeWScopeWithField);

    // BSONCodeWScope with invalid UTF-8 gets correctly scrubbed with a empty and non-empty field
    // name.
    // NOTE: There are some mismatches with \ufffd and the replacement character directly being
    // compared, so we are just checking that the output doesn't have invalid UTF-8 strings
    // rather than comparing the scrubbed invalid BSONObj with a scrubbed version that we declare.
    std::string invalidCode1 = "(\xc2 function(){print(x);})();";
    BSONObj validCodeObj = BSON("x" << 1);

    BSONObj invalidCodeWithEmptyField1 = makeBSONCodeWScopeObject("", invalidCode1, validCodeObj);
    scrubAndAssertUTF8Valid(invalidCodeWithEmptyField1);

    BSONObj invalidCodeWScopeWithField1 =
        makeBSONCodeWScopeObject("code", invalidCode1, validCodeObj);
    scrubAndAssertUTF8Valid(invalidCodeWScopeWithField1);

    // The invalid code is in the BSONObj of the CodeWScope, not the codeStr.
    std::string validCode2 = "var x;";
    BSONObj invalidCodeObj1 = BSON("x" << "\xf8\x80\x80\x80\x80");

    BSONObj invalidCodeWScopeWithEmptyField2 =
        makeBSONCodeWScopeObject("", validCode2, invalidCodeObj1);
    scrubAndAssertUTF8Valid(invalidCodeWScopeWithEmptyField2);

    BSONObj invalidCodeWScopeWithField2 =
        makeBSONCodeWScopeObject("code", validCode2, invalidCodeObj1);
    scrubAndAssertUTF8Valid(invalidCodeWScopeWithField2);

    // The invalid code is both in the codeStr and BSONObj of the CodeWScope.
    std::string invalidCode2 = "var x; print('\xc3\x28');";
    BSONObj invalidCodeObj2 = BSON("x" << "\xde\xa0\x80");
    BSONObj invalidCodeWScopeWithEmptyField3 =
        makeBSONCodeWScopeObject("", invalidCode2, invalidCodeObj2);
    scrubAndAssertUTF8Valid(invalidCodeWScopeWithEmptyField3);

    BSONObj invalidCodeWScopeWithField3 =
        makeBSONCodeWScopeObject("code", invalidCode2, invalidCodeObj2);
    scrubAndAssertUTF8Valid(invalidCodeWScopeWithField3);
}

TEST(checkAndScrubInvalidUTF8, SimpleArrays) {
    // BSONArrays with all valid UTF-8 strings.
    // Empty array
    auto validBSONObj1 = makeBSONArrayObject<std::string>("arr", {});
    unchangedBSONInput(validBSONObj1);

    // Mixed bag of UTF-8 valid strings
    auto validBSONObj2 = makeBSONArrayObject<std::string>(
        "arr", {"A", "\xc2\xa2", "\xe2\x82\xac", "\xf0\x9d\x90\x80", "こんにちは", "😊"});
    unchangedBSONInput(validBSONObj2);

    // One valid UTF-8 string
    auto validBSONObj3 = makeBSONArrayObject<std::string>("arr", {"A"});
    unchangedBSONInput(validBSONObj3);

    // Multiple empty (valid UTF-8) strings
    auto validBSONObj4 = makeBSONArrayObject<std::string>("arr", {"", ""});
    unchangedBSONInput(validBSONObj4);

    // Code that is all valid UTF-8 in arrays
    // {codeSnippets: [BSONCode{"x = 0"}, BSONCode{(function(){})();}]}
    BSONCode validCode1{StringData("(function(){})();")};
    BSONCode validCode2{StringData("x = 0")};
    BSONObj codeSnippetArr =
        makeBSONArrayObject<BSONCode>("codeSnippets", {validCode1, validCode2});
    unchangedBSONInput(codeSnippetArr);

    BSONObj noScrubbableElemArr = makeBSONArrayObject<BSONObj>("noScrubs",
                                                               {undefinedObj,
                                                                objectIdObj,
                                                                boolObj,
                                                                dateTimeObj,
                                                                nullObj,
                                                                intObj,
                                                                timestampObj,
                                                                longIntObj,
                                                                decimalObj});
    unchangedBSONInput(noScrubbableElemArr);

    BSONObj mixedValidArr = makeBSONArrayObject<BSONObj>("TLC",
                                                         {BSON("validCode1" << validCode1),
                                                          dateTimeObj,
                                                          nullObj,
                                                          BSON("validCode2" << validCode2),
                                                          longIntObj,
                                                          boolObj});
    unchangedBSONInput(mixedValidArr);

    // BSONArrays with invalid UTF-8 strings.
    // Abrupt end invalid UTF-8 string as first array element
    auto invalidBSONObj1 = makeBSONArrayObject<std::string>("arr", {"\xc2", "B", "C"});
    std::vector<std::string> scrubbedArr1 = {scrubMap.find("\xc2")->second, "B", "C"};
    auto scrubbedBSONObj1 = makeBSONArrayObject<std::string>("arr", scrubbedArr1);
    scrubbedBSONInput(invalidBSONObj1, scrubbedBSONObj1);

    // Abrupt end as second/last invalid UTF-8 string.
    auto invalidBSONObj2 = makeBSONArrayObject<std::string>("arr", {"A", "\xe2\x82"});
    auto scrubbedBSONObj2 =
        makeBSONArrayObject<std::string>("arr", {"A", scrubMap.find("\xe2\x82")->second});
    scrubbedBSONInput(invalidBSONObj2, scrubbedBSONObj2);

    // Spaces in the invalid UTF-8 string.
    auto invalidBSONObj3 = makeBSONArrayObject<std::string>("arr", {" \xc2 ", "\xe2\x82"});
    auto scrubbedBSONObj3 = makeBSONArrayObject<std::string>(
        "arr", {scrubMap.find(" \xc2 ")->second, scrubMap.find("\xe2\x82")->second});
    scrubbedBSONInput(invalidBSONObj3, scrubbedBSONObj3);

    // Mixed invalid UTF-8 strings (includes not starting with continuation byte, too long,
    // surrogate pairs (low and high), etc.) with some valid UTF-8 strings to ensure they are
    // un-scrubbed.
    auto invalidBSONObj4 = makeBSONArrayObject<std::string>(
        "arr", {"\xf8\x80\x80\x80\x80", "\x80", "A", "\xde\xa0\x80", "", "\xf0\x9d\xdc\x80"});
    auto scrubbedBSONObj4 =
        makeBSONArrayObject<std::string>("arr",
                                         {scrubMap.find("\xf8\x80\x80\x80\x80")->second,
                                          scrubMap.find("\x80")->second,
                                          "A",
                                          scrubMap.find("\xde\xa0\x80")->second,
                                          "",
                                          scrubMap.find("\xf0\x9d\xdc\x80")->second});
    scrubbedBSONInput(invalidBSONObj4, scrubbedBSONObj4);
}

TEST(checkAndScrubInvalidUTF8, DoesNotScrubNestedValidUTF8) {
    // Tests that there isn't scrubbing of nested valid UTF-8.
    // {a: "😊"}
    BSONObj oneNest = BSON("a" << "😊");
    unchangedBSONInput(oneNest);

    // {c: {'rewind': "こんにちは"}}
    BSONObj twoNest = BSON("c" << BSON("rewind" << "こんにちは"));
    unchangedBSONInput(twoNest);

    // Test valid BSONCode.
    // {code: {code1: BSONCode{(function(){})());}}}
    BSONCode code1{StringData("(function(){})());")};
    BSONObj bsonCodeOneNest = BSON("code" << BSON("code1" << code1));
    unchangedBSONInput(bsonCodeOneNest);

    // Test nested arrays with BSONCode.
    // {codeSnippets: [{"codeSnippet1": BSONCode{"(function(){})());"}}, {"codeSnippet2":
    // BSONCode{"var x = 0;"}}}]
    BSONObj codeObj1 = makeBSONCodeObject("codeSnippet1", "(function(){})());");
    BSONObj codeObj2 = makeBSONCodeObject("codeSnippet2", "var x = 0;");
    auto codeSnippetArr = makeBSONArrayObject<BSONObj>("codeSnippets", {codeObj1, codeObj2});
    unchangedBSONInput(codeSnippetArr);

    // Test nested arrays.
    unchangedBSONInput(fromjson("{a:[{b:['apple','rotten','right', 'to', 'the', 'core']}]}"));
    unchangedBSONInput(fromjson("{a:[{b:[1,2,3]}, {c: ['hello', 'goodbye']}]}"));
    unchangedBSONInput(
        fromjson("{a:[{b:{c:['spring','breakers']}},{d:{e:['sympathy','is','a','knife']}}]}"));
}

// Used for NestedInvalidUTF8[...] tests, declared globally because of the mixedBag# tests.
BSONObj oneNestInvalid = BSON("a" << "\xc2");
BSONObj oneNestFieldNameInvalid = BSON("\xde\xa0\x80" << "talk talk");
BSONObj twoNestInvalid1 = BSON("c" << BSON("\x80" << "\xff\x80\x80\x80\x80\x80\x80\x80"));
BSONObj twoNestInvalid2 = BSON("\xc3\x28" << BSON("\xf0\x9d\xdc\x80 " << "B2B"));
BSONObj invalidNestedCode = BSON("code" << BSON("code1" << "(function(){\xe2\x28\xa1})());"));
std::string invalidCode1 = "(\xc2 function(){print(x);})();";
BSONObj validCodeObj1 = BSON("x" << 1);
BSONObj invalidCodeWScope1 = makeBSONCodeWScopeObject("codeWScope1", invalidCode1, validCodeObj1);

BSONObj invalidCodeObj1 = makeBSONCodeObject("codeSnippet1", "(function(){\xe2\x28\xa1})());");
BSONObj invalidCodeObj2 = makeBSONCodeObject("codeSnippet2", "var x = \xde\xa0\x80;");

std::string validCode1 = "var y;";
BSONObj invalidCodeObj3 = BSON("y" << "\xe0");
BSONObj invalidCodeWScope2 = makeBSONCodeWScopeObject("codeWScope2", validCode1, invalidCodeObj3);

// The invalid code is both in the codeStr and BSONObj of the CodeWScope.
std::string invalidCode2 = "var x; print('\xc0\x80');";
BSONObj invalidCodeObj4 = BSON("x" << "\xe0\x80\x80");
BSONObj invalidCodeWScope3 = makeBSONCodeWScopeObject("codeWScope3", invalidCode2, invalidCodeObj4);

TEST(checkAndScrubInvalidUTF8, SimpleNestedInvalidUTF8) {
    // First level only has invalid UTF-8.
    // {a: "\xc2"} (invalid UTF-8 for the second field)
    scrubAndAssertUTF8Valid(oneNestInvalid);

    // {"\xde\xa0\x80": "talk talk"} (invalid UTF-8 for the first field)
    scrubAndAssertUTF8Valid(oneNestFieldNameInvalid);

    // {c: {"\x80": "\xff\x80\x80\x80\x80\x80\x80\x80"}} (invalid UTF-8 for the second and third
    // field)
    scrubAndAssertUTF8Valid(twoNestInvalid1);

    // {"\xc3\x28": {"\xf0\x9d\xdc\x80 ": "B2B"}} (invalid UTF-8 for the first and second field)
    scrubAndAssertUTF8Valid(twoNestInvalid2);
}

TEST(checkAndScrubInvalidUTF8, NestedInvalidBSONCode) {
    // Test with nested invalid BSONCode.
    // {code: {code1: "(function(){\xe2\x28\xa1})());"}}
    // The value inside {} for the code snippet is not UTF-8 valid.
    scrubAndAssertUTF8Valid(invalidNestedCode);

    // Test nested arrays with BSONCode.
    // {codeSnippets: [
    // {"codeSnippet1": BSONCode{"(function(){\xe2\x28\xa1})());"}},
    // {"codeSnippet2": BSONCode{"var x = \xde\xa0\x80;"}},
    // }]
    // The value inside {} for codeSnippet1 and the assignment of x in codeSnippet2 are both not
    // UTF-8 valid.
    auto invalidBSONCodeArr =
        makeBSONArrayObject<BSONObj>("codeSnippets", {invalidCodeObj1, invalidCodeObj2});
    scrubAndAssertUTF8Valid(invalidBSONCodeArr);
}

TEST(checkAndScrubInvalidUTF8, NestedInvalidBSONCodeWScope) {
    // Test nested arrays with BSONCodeWScope.
    // {"codeWScopes": [
    // {"codeWScope1": BSONCodeWScope{"(\xc2 function(){print(x);})();", (x:1)}},
    // {"codeWScope2": BSONCodeWScope{"var y;", (y:\xe0)}},
    // {"codeWScope3": BSONCodeWScope{"var x; print('\xc0\x80');", (x:\xe0\x80\x80")}},
    // ]}
    BSONObj invalidCodeWScopeArr = makeBSONArrayObject<BSONObj>(
        "codeWScopes", {invalidCodeWScope1, invalidCodeWScope2, invalidCodeWScope3});
    scrubAndAssertUTF8Valid(invalidCodeWScopeArr);
}

TEST(checkAndScrubInvalidUTF8, NestedInvalidMixedBag) {
    // Test nested arrays with various BSONObj types that have invalid UTF-8 strings.
    // {"mixedBag1": [
    // {"\xc3\x28": {"\xf0\x9d\xdc\x80 ": "B2B"}},
    // {"codeSnippet1": BSONCode{"(function(){\xe2\x28\xa1})());"}},
    // {"codeWScope1": BSONCodeWScope{"(\xC2 function(){print(x);})();", (x:1)}},
    // {"codeWScope3": BSONCodeWScope{"var x; print('\xc0\x80');", (x:\xe0\x80\x80")}},
    // ]}
    BSONObj mixedBag1 = makeBSONArrayObject<BSONObj>(
        "mixedBag1", {twoNestInvalid2, invalidCodeObj1, invalidCodeWScope1, invalidCodeWScope3});
    scrubAndAssertUTF8Valid(mixedBag1);

    // {"mixedBag2": [
    // {"codeWScope2": BSONCodeWScope{"var y;", (y:\xe0)}},
    // {"\xde\xa0\x80": "talk talk"},
    // {"codeSnippet2": BSONCode{"var x = \xde\xa0\x80;"}}
    // ]}
    BSONObj mixedBag2 = makeBSONArrayObject<BSONObj>(
        "mixedBag2", {invalidCodeWScope2, oneNestFieldNameInvalid, invalidCodeObj2});
    scrubAndAssertUTF8Valid(mixedBag2);

    // Test scrubbing invalid BSON with non-String BSONTypes (BSONTypes that shouldn't be scrubbed)
    // and valid BSON.
    // {"mixedBag3": [
    // {"c": {"\x80": "\xff\x80\x80\x80\x80\x80\x80\x80"}},
    // {"codeWScope1": BSONCodeWScope{"(\xc2 function(){print(x);})();", (x:1)}},
    // {"bool": BSONBool {true}}, /* Cannot contain invalid UTF-8 */
    // {"a": "\xc2"},
    // {"datetime": DATENOW}, /* Cannot contain invalid UTF-8 */
    // {"null": BSONNull()}, /* Cannot contain invalid UTF-8 */
    // {"validCode": BSONCode("(function(){})();))"} /* BSONCode with valid UTF-8 strings */
    // ]}
    auto validCodeObj2 = makeBSONCodeObject("validCode", "(function(){})();");
    BSONObj mixedBag3 = makeBSONArrayObject<BSONObj>("mixedBag3",
                                                     {twoNestInvalid1,
                                                      invalidCodeWScope1,
                                                      boolObj,
                                                      oneNestInvalid,
                                                      dateTimeObj,
                                                      nullObj,
                                                      validCodeObj2});
    auto scrubbedMixedBag3 = scrubAndAssertUTF8Valid(mixedBag3);
    // Check the valid and the not-String BSONObj are not scrubbed.
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag3["mixedBag3"]["2"].Obj(), boolObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag3["mixedBag3"]["4"].Obj(), dateTimeObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag3["mixedBag3"]["5"].Obj(), nullObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag3["mixedBag3"]["6"].Obj(), validCodeObj2);

    // {"mixedBag4": [
    // {"int": 360}, /* Cannot contain invalid UTF-8 */
    // {"\xc3\x28": {"\xf0\x9d\xdc\x80": "B2B"}},
    // {"codeWScope1": BSONCodeWScope{"(\xc2 function(){print(x);})();", (x:1)}},
    // {"longInt": 0x0fedcba987654321ll}, /* Cannot contain invalid UTF-8 */
    // {"fiona": "apple"}, /* Valid BSON */
    // {c: {"\x80": "\xff\x80\x80\x80\x80\x80\x80\x80"}},
    // {"codeWScope3": BSONCodeWScope{"var x; print('\xc0\x80');", (x:\xe0\x80\x80")}}
    // ]}
    auto validStringObj = BSON("fiona" << "apple");
    BSONObj mixedBag4 = makeBSONArrayObject<BSONObj>("mixedBag4",
                                                     {
                                                         intObj,
                                                         twoNestInvalid2,
                                                         invalidCodeWScope1,
                                                         longIntObj,
                                                         validStringObj,
                                                         twoNestInvalid1,
                                                         invalidCodeWScope3,
                                                     });
    auto scrubbedMixedBag4 = scrubAndAssertUTF8Valid(mixedBag4);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag4["mixedBag4"]["0"].Obj(), intObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag4["mixedBag4"]["3"].Obj(), longIntObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag4["mixedBag4"]["4"].Obj(), validStringObj);

    // {"mixedBag5": [
    // {"codeWScope2": BSONCodeWScope{"var y;", (y:\xe0)}},
    // {"oid": OID("dbdbdbdbdbdbdbdbdbdbdbdb")}, /* Cannot contain invalid UTF-8 */
    // {"RegEx": ("lemonade", "product")}, /* Valid BSON */
    // {"codeSnippet2": BSONCode{"(function(){\xe2\x28\xa1})());"}},
    // {"undefined": BSONUndefined}, /* Cannot contain invalid UTF-8 */
    // {"decimal": Decimal128{0.365}} /* Cannot contain invalid UTF-8 */
    // ]}
    auto validRegExObj = BSON("RegEx" << BSONRegEx("lemonade", "product"));
    BSONObj mixedBag5 = makeBSONArrayObject<BSONObj>("mixedBag5",
                                                     {
                                                         invalidCodeWScope2,
                                                         objectIdObj,
                                                         validRegExObj,
                                                         invalidCodeObj2,
                                                         undefinedObj,
                                                         decimalObj,
                                                     });
    auto scrubbedMixedBag5 = scrubAndAssertUTF8Valid(mixedBag5);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag5["mixedBag5"]["1"].Obj(), objectIdObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag5["mixedBag5"]["2"].Obj(), validRegExObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag5["mixedBag5"]["4"].Obj(), undefinedObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag5["mixedBag5"]["5"].Obj(), decimalObj);

    // mixedBag, but with an invalid UTF-8 string fieldName for the obj.
    // {"\xe2\x28\xa1": [
    // {"null": BSONNull()}, /* Cannot contain invalid UTF-8 */
    // {"codeSnippet1": BSONCode{"(function(){\xe2\x28\xa1})());"}},
    // {"codeWScope3": BSONCodeWScope{"var x; print('\xc0\x80');", (x:\xe0\x80\x80")}}
    // {"longInt": 0x0fedcba987654321ll}, /* Cannot contain invalid UTF-8 */
    // {"Symbol": "how i'm feeling now"}, /* Valid BSON */
    // {"oid": OID("dbdbdbdbdbdbdbdbdbdbdbdb")}, /* Cannot contain invalid UTF-8 */
    // ]}
    BSONObj validSymbolObj = BSON("Symbol" << BSONSymbol("how i'm feeling now"));
    BSONObj mixedBag6 = makeBSONArrayObject<BSONObj>("\xe2\x28\xa1",
                                                     {
                                                         nullObj,
                                                         invalidCodeObj1,
                                                         invalidCodeWScope3,
                                                         longIntObj,
                                                         validSymbolObj,
                                                         objectIdObj,
                                                     });
    auto scrubbedMixedBag6 = scrubAndAssertUTF8Valid(mixedBag6);
    auto scrubbedFieldName = "\xef\xbf\xbd\x28\xef\xbf\xbd";
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag6[scrubbedFieldName]["0"].Obj(), nullObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag6[scrubbedFieldName]["3"].Obj(), longIntObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag6[scrubbedFieldName]["4"].Obj(), validSymbolObj);
    ASSERT_BSONOBJ_EQ(scrubbedMixedBag6[scrubbedFieldName]["5"].Obj(), objectIdObj);
}
}  // namespace mongo
