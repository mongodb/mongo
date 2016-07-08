/**
 *  Copyright (C) 2015 MongoDB Inc.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/logger/redaction.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const std::string kRedactionDefaultMask = "###";
const std::string kMsg = "Not initialized";
using BSONStringPair = std::pair<BSONObj, std::string>;

TEST(RedactStringTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);

    std::string toRedact[] = {"", "abc", "*&$@!_\\\\\\\"*&$@!_\"*&$@!_\"*&$@!_"};
    for (auto s : toRedact) {
        ASSERT_EQ(redact(s), s);
    }
}

TEST(RedactStringTest, BasicStrings) {
    logger::globalLogDomain()->setShouldRedactLogs(true);

    std::string toRedact[] = {"", "abc", "*&$@!_\\\\\\\"*&$@!_\"*&$@!_\"*&$@!_"};
    for (auto s : toRedact) {
        ASSERT_EQ(redact(s), kRedactionDefaultMask);
    }
}

TEST(RedactStatusTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);
    Status status(ErrorCodes::InternalError, kMsg);
    ASSERT_EQ(redact(status), status.toString());
}

TEST(RedactStatusTest, BasicStatus) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    Status status(ErrorCodes::InternalError, kMsg);
    ASSERT_EQ(redact(status), "InternalError: " + kRedactionDefaultMask);
}

TEST(RedactStatusTest, StatusWithLocation) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    Status status(ErrorCodes::InternalError, kMsg, 777);
    ASSERT_EQ(redact(status), "InternalError: " + kRedactionDefaultMask + " @ 777");
}

TEST(RedactStatusTest, StatusOK) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    ASSERT_EQ(redact(Status::OK()), "OK");
}

TEST(RedactExceptionTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);
    DBException ex(kMsg, ErrorCodes::InternalError);
    ASSERT_EQ(redact(ex), ex.toString());
}

TEST(RedactExceptionTest, BasicException) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    DBException ex(kMsg, ErrorCodes::InternalError);
    ASSERT_EQ(redact(ex), "1 ###");
}

TEST(RedactBSONTest, NoRedact) {
    logger::globalLogDomain()->setShouldRedactLogs(false);
    BSONObj obj = BSON("a" << 1);
    ASSERT_EQ(redact(obj), obj.toString());
}

void testBSONCases(std::initializer_list<BSONStringPair> testCases) {
    for (auto m : testCases) {
        ASSERT_EQ(redact(m.first), m.second);
    }
}

TEST(RedactBSONTest, BasicBSON) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testBSONCases({BSONStringPair(BSONObj(), "{}"),
                   BSONStringPair(BSON("" << 1), "{ : \"###\" }"),
                   BSONStringPair(BSON("a" << 1), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << 1.0), "{ a: \"###\" }"),
                   BSONStringPair(BSON("a"
                                       << "a"),
                                  "{ a: \"###\" }"),
                   BSONStringPair(BSON("a" << 1 << "b"
                                           << "str"),
                                  "{ a: \"###\", b: \"###\" }"),
                   BSONStringPair(BSON("a" << 1 << "a"
                                           << "1"),
                                  "{ a: \"###\", a: \"###\" }")});
}
/*
TEST(RedactBSONTest, NestedBSON) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testCases.push_back(BSONStringPair(BSON("a" << BSONObj()), "{ a: {} }"));
    testCases.push_back(BSONStringPair(
        BSON("a" << BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj(BSONObj())))))))),
        "{ a: {} }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON("a" << 1)), "{ a: { a: \"###\" } }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON("a" << 1 << "b" << 1)),
                                       "{ a: { a: \"###\", b: \"###\" } }"));
    testBSONVector(testCases);
}

TEST(RedactBSONTest, BSONWithArrays) {
    logger::globalLogDomain()->setShouldRedactLogs(true);
    std::vector<BSONStringPair> testCases;

    testCases.push_back(BSONStringPair(BSON("a" << BSONArray()), "{ a: [] }"));
    testCases.push_back(
        BSONStringPair(BSON("a" << BSON_ARRAY("abc" << 1)), "{ a: [ \"###\", \"###\" ] }"));
    testCases.push_back(BSONStringPair(BSON("a" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1))),
                                       "{ a: [ { a: \"###\" }, { b: \"###\" } ] }"));

    testBSONVector(testCases);
}*/
}  // namespace
}  // namespace mongo
