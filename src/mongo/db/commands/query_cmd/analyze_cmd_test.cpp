// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * IDL-parse-time validation tests for the analyze command. These exercise only the IDL parser
 * generated from src/mongo/db/query/analyze_command.idl — no opCtx, no catalog, no fixture.
 * Runtime checks that require a real collection acquisition live in
 * jstests/noPassthrough/query/analyze_sample_persist.js instead.
 */

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/analyze_command_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

TEST(AnalyzeCommandParseTest, SampleSizeRejectsZero) {
    auto cmd = BSON("analyze" << "myColl"
                              << "$db" << "test"
                              << "mode" << "sample"
                              << "samplingMethod" << "random"
                              << "sampleSize" << 0);
    ASSERT_THROWS_CODE(AnalyzeCommandRequest::parse(cmd, IDLParserContext("analyze")),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(AnalyzeCommandParseTest, SampleSizeRejectsNegative) {
    auto cmd = BSON("analyze" << "myColl"
                              << "$db" << "test"
                              << "mode" << "sample"
                              << "samplingMethod" << "random"
                              << "sampleSize" << -1);
    ASSERT_THROWS_CODE(AnalyzeCommandRequest::parse(cmd, IDLParserContext("analyze")),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(AnalyzeCommandParseTest, NumChunksRejectsZero) {
    auto cmd = BSON("analyze" << "myColl"
                              << "$db" << "test"
                              << "mode" << "sample"
                              << "samplingMethod" << "chunk"
                              << "numChunks" << 0);
    ASSERT_THROWS_CODE(AnalyzeCommandRequest::parse(cmd, IDLParserContext("analyze")),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(AnalyzeCommandParseTest, NumChunksRejectsNegative) {
    auto cmd = BSON("analyze" << "myColl"
                              << "$db" << "test"
                              << "mode" << "sample"
                              << "samplingMethod" << "chunk"
                              << "numChunks" << -1);
    ASSERT_THROWS_CODE(AnalyzeCommandRequest::parse(cmd, IDLParserContext("analyze")),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(AnalyzeCommandParseTest, SamplingMethodRejectsUnknownValue) {
    auto cmd = BSON("analyze" << "myColl"
                              << "$db" << "test"
                              << "mode" << "sample"
                              << "samplingMethod" << "invalid_method");
    ASSERT_THROWS_CODE(AnalyzeCommandRequest::parse(cmd, IDLParserContext("analyze")),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(AnalyzeCommandParseTest, ModeRejectsUnknownValue) {
    auto cmd = BSON("analyze" << "myColl"
                              << "$db" << "test"
                              << "mode" << "invalid_mode");
    ASSERT_THROWS_CODE(AnalyzeCommandRequest::parse(cmd, IDLParserContext("analyze")),
                       DBException,
                       ErrorCodes::BadValue);
}

}  // namespace
}  // namespace mongo
