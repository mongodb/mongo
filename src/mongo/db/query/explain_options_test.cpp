/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/explain_options.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ExplainOptionsTest, VerbosityEnumToStringReturnsCorrectValues) {
    ASSERT_EQ(ExplainOptions::verbosityString(ExplainOptions::Verbosity::kQueryPlanner),
              "queryPlanner"_sd);
    ASSERT_EQ(ExplainOptions::verbosityString(ExplainOptions::Verbosity::kExecStats),
              "executionStats"_sd);
    ASSERT_EQ(ExplainOptions::verbosityString(ExplainOptions::Verbosity::kExecAllPlans),
              "allPlansExecution"_sd);
}

TEST(ExplainOptionsTest, ExplainOptionsSerializeToBSONCorrectly) {
    ASSERT_BSONOBJ_EQ(BSON("verbosity"
                           << "queryPlanner"),
                      ExplainOptions::toBSON(ExplainOptions::Verbosity::kQueryPlanner));
    ASSERT_BSONOBJ_EQ(BSON("verbosity"
                           << "executionStats"),
                      ExplainOptions::toBSON(ExplainOptions::Verbosity::kExecStats));
    ASSERT_BSONOBJ_EQ(BSON("verbosity"
                           << "allPlansExecution"),
                      ExplainOptions::toBSON(ExplainOptions::Verbosity::kExecAllPlans));
}

TEST(ExplainOptionsTest, CanParseExplainVerbosity) {
    auto verbosity = unittest::assertGet(
        ExplainOptions::parseCmdBSON(fromjson("{explain: {}, verbosity: 'queryPlanner'}")));
    ASSERT(verbosity == ExplainOptions::Verbosity::kQueryPlanner);
    verbosity = unittest::assertGet(
        ExplainOptions::parseCmdBSON(fromjson("{explain: {}, verbosity: 'executionStats'}")));
    ASSERT(verbosity == ExplainOptions::Verbosity::kExecStats);
    verbosity = unittest::assertGet(
        ExplainOptions::parseCmdBSON(fromjson("{explain: {}, verbosity: 'allPlansExecution'}")));
    ASSERT(verbosity == ExplainOptions::Verbosity::kExecAllPlans);
}

TEST(ExplainOptionsTest, ParsingFailsIfVerbosityIsNotAString) {
    ASSERT_EQ(ExplainOptions::parseCmdBSON(fromjson("{explain: {}, verbosity: 1}")).getStatus(),
              ErrorCodes::FailedToParse);
    ASSERT_EQ(ExplainOptions::parseCmdBSON(fromjson("{explain: {}, verbosity: {foo: 'bar'}}"))
                  .getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(ExplainOptionsTest, ParsingFailsIfVerbosityStringIsNotRecognized) {
    ASSERT_EQ(ExplainOptions::parseCmdBSON(fromjson("{explain: {}, verbosity: 'badVerbosity'}"))
                  .getStatus(),
              ErrorCodes::FailedToParse);
}

TEST(ExplainOptionsTest, ParsingFailsIfFirstElementIsNotAnObject) {
    ASSERT_EQ(ExplainOptions::parseCmdBSON(fromjson("{explain: 1, verbosity: 'queryPlanner'}"))
                  .getStatus(),
              ErrorCodes::FailedToParse);
}

}  // namespace
}  // namespace mongo
