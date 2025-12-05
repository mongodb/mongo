/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <memory>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

constexpr char ExplainStageName[] = "$explain";

class ExplainExecStage : public sdk::TestExecStage {
public:
    ExplainExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, arguments) {}

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON("execMetricField" << "execMetricValue");
    }
};

class ExplainLogicalStage : public sdk::TestLogicalStage<ExplainExecStage> {
public:
    ExplainLogicalStage(std::string_view stageName, const mongo::BSONObj& spec)
        : TestLogicalStage(stageName, spec) {}

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        BSONObjBuilder builder;

        {
            BSONObjBuilder stageBuilder = builder.subobjStart(_name);

            // This was validated at parse time.
            auto input = _arguments["input"].valueStringDataSafe();
            stageBuilder.append("input", input);

            switch (verbosity) {
                case ::MongoExtensionExplainVerbosity::kQueryPlanner:
                    stageBuilder.append("verbosity", "queryPlanner");
                    break;
                case ::MongoExtensionExplainVerbosity::kExecStats:
                    stageBuilder.append("verbosity", "executionStats");
                    break;
                case ::MongoExtensionExplainVerbosity::kExecAllPlans:
                    stageBuilder.append("verbosity", "allPlansExecution");
                    break;
                default:
                    sdk_tasserted(11239405,
                                  (str::stream() << "unknown explain verbosity provided to "
                                                 << _name << " stage: " << verbosity));
            }

            stageBuilder.done();
        }

        return builder.obj();
    }
};

DEFAULT_AST_NODE(Explain);
DEFAULT_PARSE_NODE(Explain);

/**
 * Stage with a non-default explain implementation. Syntax:
 *
 * {$explain: {input: "any string"}}
 *
 * Explain will output the input and the verbosity level.
 */
class ExplainStageDescriptor : public sdk::TestStageDescriptor<"$explain", ExplainParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        sdk_uassert(
            11239403,
            (str::stream() << "input to " << ExplainStageName << " must be a string " << arguments),
            arguments["input"] && arguments["input"].type() == mongo::BSONType::string);
    }
};

DEFAULT_EXTENSION(Explain)
REGISTER_EXTENSION(ExplainExtension)
DEFINE_GET_EXTENSION()
