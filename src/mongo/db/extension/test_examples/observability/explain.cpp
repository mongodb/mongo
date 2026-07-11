// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

#include <memory>
#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

constexpr char ExplainStageName[] = "$explain";

class ExplainExecStage : public sdk::TestExecStage {
public:
    ExplainExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, arguments) {}

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON("execMetricField" << "execMetricValue");
    }
};

class ExplainLogicalStage : public sdk::TestLogicalStage<ExplainExecStage> {
public:
    ExplainLogicalStage(std::string_view stageName, const mongo::BSONObj& spec)
        : TestLogicalStage(stageName, spec) {}

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
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

    std::unique_ptr<extension::sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ExplainLogicalStage>(_name, _arguments);
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
