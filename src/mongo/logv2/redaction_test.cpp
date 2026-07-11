// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/logv2/redaction.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/logv2/log_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <initializer_list>
#include <iostream>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

const std::string kRedactionDefaultMask = "###";
const std::string kMsg = "Not initialized";
using BSONStringPair = std::pair<BSONObj, std::string>;

TEST(RedactStringTest, NoRedact) {
    logv2::setShouldRedactLogs(false);

    std::string toRedact[] = {"", "abc", "*&$@!_\\\\\\\"*&$@!_\"*&$@!_\"*&$@!_"};
    for (const auto& s : toRedact) {
        ASSERT_EQ(redact(s), s);
    }
}

TEST(RedactStringTest, BasicStrings) {
    logv2::setShouldRedactLogs(true);

    std::string toRedact[] = {"", "abc", "*&$@!_\\\\\\\"*&$@!_\"*&$@!_\"*&$@!_"};
    for (const auto& s : toRedact) {
        ASSERT_EQ(redact(s), kRedactionDefaultMask);
    }
}

TEST(RedactStatusTest, NoRedact) {
    logv2::setShouldRedactLogs(false);
    Status status(ErrorCodes::InternalError, kMsg);
    ASSERT_EQ(redact(status), status.toString());
}

TEST(RedactStatusTest, BasicStatus) {
    logv2::setShouldRedactLogs(true);
    Status status(ErrorCodes::InternalError, kMsg);
    ASSERT_EQ(redact(status), "InternalError: " + kRedactionDefaultMask);
}

TEST(RedactStatusTest, StatusOK) {
    logv2::setShouldRedactLogs(true);
    ASSERT_EQ(redact(Status::OK()), "OK");
}

TEST(RedactExceptionTest, NoRedact) {
    logv2::setShouldRedactLogs(false);
    ASSERT_THROWS_WITH_CHECK(
        [] {
            uasserted(ErrorCodes::InternalError, kMsg);
        }(),
        DBException,
        [](const DBException& ex) { ASSERT_EQ(redact(ex), ex.toString()); });
}

TEST(RedactExceptionTest, BasicException) {
    logv2::setShouldRedactLogs(true);
    ASSERT_THROWS_WITH_CHECK(
        [] {
            uasserted(ErrorCodes::InternalError, kMsg);
        }(),
        DBException,
        [](const DBException& ex) { ASSERT_EQ(redact(ex), "InternalError ###"); });
}

TEST(RedactBSONTest, NoRedact) {
    logv2::setShouldRedactLogs(false);
    BSONObj obj = BSON("a" << 1);
    ASSERT_BSONOBJ_EQ(redact(obj), obj);
}

void testBSONCases(std::initializer_list<BSONStringPair> testCases) {
    for (const auto& m : testCases) {
        ASSERT_EQ(redact(m.first).toString(), m.second);
    }
}

TEST(RedactBSONTest, BasicBSON) {
    logv2::setShouldRedactLogs(true);

    testBSONCases({BSONStringPair(BSONObj(), "{}"),
                   BSONStringPair(BSON("" << 1), "{ : \"###\" }"),
                   BSONStringPair(BSON("a" << 1), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << 1.0), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << "a"), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << 1 << "b"
                                           << "str"),
                                  "{ a: \"###\", b: \"###\" }"),
                   BSONStringPair(BSON("a" << 1 << "a"
                                           << "1"),
                                  "{ a: \"###\", a: \"###\" }")});
}

unsigned char zero[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

TEST(RedactEncryptedStringTest, BasicStrings) {
    logv2::setShouldRedactBinDataEncrypt(true);
    logv2::setShouldRedactLogs(false);

    BSONObjBuilder builder{};
    builder.appendBinData("type6", sizeof(zero), BinDataType::Encrypt, zero);
    builder.append("string", "string");
    {
        BSONObjBuilder sub(builder.subobjStart("nestedobj"));
        sub.appendBinData("subobj", sizeof(zero), BinDataType::Encrypt, zero);
    }
    BSONObj obj = builder.done();

    auto redactedStr = R"({ type6: "###", string: "string", nestedobj: { subobj: "###" } })";
    ASSERT_EQ(redact(obj).toString(), redactedStr);

    logv2::setShouldRedactBinDataEncrypt(false);
    ASSERT_EQ(redact(obj).toString(), obj.toString());
}

TEST(RedactSensitiveStringTest, BasicStrings) {
    BSONObjBuilder builder{};
    builder.appendBinData("type8", sizeof(zero), BinDataType::Sensitive, zero);
    builder.append("string", "string");
    {
        BSONObjBuilder sub(builder.subobjStart("nestedobj"));
        sub.appendBinData("subobj", sizeof(zero), BinDataType::Sensitive, zero);
    }
    const BSONObj obj = builder.done();

    {
        logv2::setShouldRedactBinDataEncrypt(true);
        logv2::setShouldRedactLogs(true);

        // Fully-redacted logs should just redact everything
        const auto redactedStr = R"({ type8: "###", string: "###", nestedobj: { subobj: "###" } })";
        ASSERT_EQ(redact(obj).toString(), redactedStr);
    }

    {
        const auto redactedStr =
            R"({ type8: "###", string: "string", nestedobj: { subobj: "###" } })";
        // The setting for redacting logs shouldn't affect sensitive BinData.
        logv2::setShouldRedactLogs(false);
        ASSERT_EQ(redact(obj).toString(), redactedStr);

        // The setting for redacting encrypted BinData shouldn't affect sensitive BinData, either.
        logv2::setShouldRedactBinDataEncrypt(false);
        ASSERT_EQ(redact(obj).toString(), redactedStr);
    }
}

TEST(RedactSensitiveStringTest, NestedStrings) {
    // The setting for redacting logs shouldn't affect sensitive BinData.
    logv2::setShouldRedactBinDataEncrypt(false);
    // The setting for redacting encrypted BinData shouldn't affect sensitive BinData, either.
    logv2::setShouldRedactLogs(false);

    BSONObjBuilder builder{};

    // Test for [ "###", { ...: "###" }, ... ] shape cases.
    {
        auto subarray = BSONObjBuilder(builder.subarrayStart("subarray"));
        subarray.appendBinData("0", sizeof(zero), BinDataType::Sensitive, zero);

        for (auto nSubobjs = 0; nSubobjs < 3; ++nSubobjs) {
            BSONObjBuilder(subarray.subobjStart("subobj"))
                .appendBinData("type8", sizeof(zero), BinDataType::Sensitive, zero);
        }
    }

    // Test for { ...: "###", ...: [ "###", ... ] } shape cases.
    {
        auto subobj = BSONObjBuilder(builder.subobjStart("subobj"));
        subobj.appendBinData("type8", sizeof(zero), BinDataType::Sensitive, zero);

        auto subarray = BSONObjBuilder(subobj.subarrayStart("subarray"));
        for (auto nSubobjs = 0; nSubobjs < 3; ++nSubobjs) {
            subarray.appendBinData("0", sizeof(zero), BinDataType::Sensitive, zero);
        }
    }

    // Test for [ [ [ "###", ... ] ] ] shape cases.
    {
        auto subarray1 = BSONObjBuilder(builder.subarrayStart("subarrays"));
        auto subarray2 = BSONObjBuilder(subarray1.subarrayStart("subarray"));
        auto subarray3 = BSONObjBuilder(subarray2.subarrayStart("subarray"));
        for (auto nSubobjs = 0; nSubobjs < 3; ++nSubobjs) {
            subarray3.appendBinData("0", sizeof(zero), BinDataType::Sensitive, zero);
        }
    }

    // Test for { ...: { ...: { ...: "###" } } } shape cases.
    {
        auto subobj1 = BSONObjBuilder(builder.subobjStart("subobjs"));
        auto subobj2 = BSONObjBuilder(subobj1.subobjStart("subobj"));
        auto subobj3 = BSONObjBuilder(subobj2.subobjStart("subobj"));
        subobj3.appendBinData("type8", sizeof(zero), BinDataType::Sensitive, zero);
    }

    const BSONObj obj = builder.done();

    // Type 8 values should all be redacted.
    const BSONObj expected = fromjson(R"({
        subarray: [ "###", { type8: "###" }, { type8: "###" }, { type8: "###" } ],
        subobj: { type8: "###", subarray: [ "###", "###", "###" ] },
        subarrays: [ [ [ "###", "###", "###" ] ] ],
        subobjs: { subobj: { subobj: { type8: "###" } } }
    })");
    ASSERT_EQ(redact(obj).toString(), expected.toString());
}

void testBSONCases(std::vector<BSONStringPair>& testCases) {
    for (const auto& m : testCases) {
        ASSERT_EQ(redact(m.first).toString(), m.second);
    }
}

TEST(RedactBSONTest, NestedBSON) {
    logv2::setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testCases.push_back(BSONStringPair(BSON("a" << BSONObj()), "{ a: {} }"));
    testCases.push_back(BSONStringPair(
        BSON("a" << BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj())))))))),
        "{ a: {} }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON("a" << 1)), "{ a: { a: \"###\" } }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON("a" << 1 << "b" << 1)),
                                       "{ a: { a: \"###\", b: \"###\" } }"));
    testBSONCases(testCases);
}

TEST(RedactBSONTest, BSONWithArrays) {
    logv2::setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testCases.push_back(BSONStringPair(BSON("a" << BSONArray()), "{ a: [] }"));
    testCases.push_back(
        BSONStringPair(BSON("a" << BSON_ARRAY("abc" << 1)), "{ a: [ \"###\", \"###\" ] }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1))),
                                       "{ a: [ { a: \"###\" }, { b: \"###\" } ] }"));

    testBSONCases(testCases);
}

TEST(RedactBSONTest, RedactedObjectShouldBeSmallerOrEqualInSizeToOriginal) {
    logv2::setShouldRedactLogs(true);
    BSONObjBuilder bob;
    for (int i = 0; i < 1024 * 1024; i++) {
        auto fieldName = "abcdefg";
        // The value of each field is smaller than the size of the kRedactionDefaultMask.
        bob.append(fieldName, 1);
    }
    const auto obj = bob.obj();
    const auto redactedObj = redact(obj);
    ASSERT_LTE(redactedObj.objsize(), obj.objsize());
}
}  // namespace
}  // namespace mongo
