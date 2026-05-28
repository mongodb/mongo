/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
