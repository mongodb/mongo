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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/query_cmd/explain_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

namespace mongo {
namespace {

using Verbosity = explain::VerbosityEnum;

TEST(ExplainTest, VerbosityEnumToStringReturnsCorrectValues) {
    ASSERT_EQ(explain::Verbosity_serializer(Verbosity::kQueryPlanner), "queryPlanner"_sd);
    ASSERT_EQ(explain::Verbosity_serializer(Verbosity::kExecStats), "executionStats"_sd);
    ASSERT_EQ(explain::Verbosity_serializer(Verbosity::kExecAllPlans), "allPlansExecution"_sd);
}

TEST(ExplainTest, ExplainSerializeToBSONCorrectly) {
    ASSERT_BSONOBJ_EQ(BSON("verbosity" << "queryPlanner"),
                      ExplainOptions::toBSON(Verbosity::kQueryPlanner));
    ASSERT_BSONOBJ_EQ(BSON("verbosity" << "executionStats"),
                      ExplainOptions::toBSON(Verbosity::kExecStats));
    ASSERT_BSONOBJ_EQ(BSON("verbosity" << "allPlansExecution"),
                      ExplainOptions::toBSON(Verbosity::kExecAllPlans));
}

TEST(ExplainTest, CanParseExplainVerbosity) {
    auto verbosity = ExplainCommandRequest::parse(
                         fromjson("{explain: {}, verbosity: 'queryPlanner', $db: 'dummy'}"),
                         IDLParserContext("explain"))
                         .getVerbosity();
    ASSERT(verbosity == Verbosity::kQueryPlanner);
    verbosity = ExplainCommandRequest::parse(
                    fromjson("{explain: {}, verbosity: 'executionStats', $db: 'dummy'}"),
                    IDLParserContext("explain"))
                    .getVerbosity();
    ASSERT(verbosity == Verbosity::kExecStats);
    verbosity = ExplainCommandRequest::parse(
                    fromjson("{explain: {}, verbosity: 'allPlansExecution', $db: 'dummy'}"),
                    IDLParserContext("explain"))
                    .getVerbosity();
    ASSERT(verbosity == Verbosity::kExecAllPlans);
}

TEST(ExplainTest, ParsingFailsIfVerbosityIsNotAString) {
    ASSERT_THROWS_CODE(ExplainCommandRequest::parse(fromjson("{explain: {}, verbosity: 1}"),
                                                    IDLParserContext("explain")),
                       DBException,
                       ErrorCodes::TypeMismatch);
    ASSERT_THROWS_CODE(
        ExplainCommandRequest::parse(fromjson("{explain: {}, verbosity: {foo: 'bar'}}"),
                                     IDLParserContext("explain")),
        DBException,
        ErrorCodes::TypeMismatch);
}

TEST(ExplainTest, ParsingFailsIfVerbosityStringIsNotRecognized) {
    ASSERT_THROWS_CODE(
        ExplainCommandRequest::parse(fromjson("{explain: {}, verbosity: 'badVerbosity'}"),
                                     IDLParserContext("explain")),
        DBException,
        ErrorCodes::BadValue);
}

TEST(ExplainTest, ParsingFailsIfFirstElementIsNotAnObject) {
    ASSERT_THROWS_CODE(
        ExplainCommandRequest::parse(fromjson("{explain: 1, verbosity: 'queryPlanner'}"),
                                     IDLParserContext("explain")),
        DBException,
        ErrorCodes::IDLFailedToParse);
}

TEST(ExplainTest, ParsingFailsIfUnknownFieldInCommandObject) {
    ASSERT_THROWS_CODE(ExplainCommandRequest::parse(
                           fromjson("{explain: {}, verbosity: 'queryPlanner', unknownField: true}"),
                           IDLParserContext("explain")),
                       DBException,
                       ErrorCodes::IDLUnknownField);
}

TEST(ExplainTest, CanParseGenericCommandArguments) {
    ExplainCommandRequest::parse(
        fromjson("{explain: {}, verbosity: 'queryPlanner', comment: true, $db: 'test'}"),
        IDLParserContext("explain"));
}

}  // namespace
}  // namespace mongo
