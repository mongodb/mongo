// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
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
#include <string_view>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using Verbosity = explain::VerbosityEnum;

TEST(ExplainTest, VerbosityEnumToStringReturnsCorrectValues) {
    ASSERT_EQ(idl::serialize(Verbosity::kQueryPlanner), "queryPlanner"sv);
    ASSERT_EQ(idl::serialize(Verbosity::kExecStats), "executionStats"sv);
    ASSERT_EQ(idl::serialize(Verbosity::kExecAllPlans), "allPlansExecution"sv);
}

TEST(ExplainTest, VerbosityEnumToStringReturnsCorrectValuesForV3) {
    ASSERT_EQ(idl::serialize(Verbosity::kPlanSummary), "planSummary"sv);
    ASSERT_EQ(idl::serialize(Verbosity::kPlannerChoice), "plannerChoice"sv);
    ASSERT_EQ(idl::serialize(Verbosity::kPlannerStats), "plannerStats"sv);
    ASSERT_EQ(idl::serialize(Verbosity::kExecStatsV3), "execStats"sv);
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

TEST(ExplainTest, CanParseExplainVerbosityV3) {
    auto parseVerbosity = [](const std::string& json) {
        return ExplainCommandRequest::parse(fromjson(json), IDLParserContext("explain"))
            .getVerbosity();
    };
    ASSERT(parseVerbosity("{explain: {}, verbosity: 'planSummary', $db: 'dummy'}") ==
           Verbosity::kPlanSummary);
    ASSERT(parseVerbosity("{explain: {}, verbosity: 'plannerChoice', $db: 'dummy'}") ==
           Verbosity::kPlannerChoice);
    ASSERT(parseVerbosity("{explain: {}, verbosity: 'plannerStats', $db: 'dummy'}") ==
           Verbosity::kPlannerStats);
    ASSERT(parseVerbosity("{explain: {}, verbosity: 'execStats', $db: 'dummy'}") ==
           Verbosity::kExecStatsV3);
}

TEST(ExplainTest, IsV3VerbosityIsTrueOnlyForNewModes) {
    // The new V3 verbosity modes are reported as V3.
    ASSERT_TRUE(ExplainOptions::isV3Verbosity(Verbosity::kPlanSummary));
    ASSERT_TRUE(ExplainOptions::isV3Verbosity(Verbosity::kPlannerChoice));
    ASSERT_TRUE(ExplainOptions::isV3Verbosity(Verbosity::kPlannerStats));
    ASSERT_TRUE(ExplainOptions::isV3Verbosity(Verbosity::kExecStatsV3));

    // The legacy verbosity modes are not V3.
    ASSERT_FALSE(ExplainOptions::isV3Verbosity(Verbosity::kQueryPlanner));
    ASSERT_FALSE(ExplainOptions::isV3Verbosity(Verbosity::kExecStats));
    ASSERT_FALSE(ExplainOptions::isV3Verbosity(Verbosity::kExecAllPlans));
    ASSERT_FALSE(ExplainOptions::isV3Verbosity(Verbosity::kInternal));
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
