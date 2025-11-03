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
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_util.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static constexpr std::string kExplainStageName = "$explain";

class ExplainLogicalStage : public sdk::LogicalAggStage {
public:
    ExplainLogicalStage(StringData input) : _input(input) {}

    BSONObj serialize() const override {
        return BSON(kExplainStageName << BSON("input" << _input));
    }

    BSONObj explain(::MongoExtensionExplainVerbosity verbosity) const override {
        BSONObjBuilder builder;

        {
            BSONObjBuilder stageBuilder = builder.subobjStart(kExplainStageName);
            stageBuilder.append("input", _input);

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
                                                 << kExplainStageName << " stage: " << verbosity));
            }

            stageBuilder.done();
        }

        return builder.obj();
    }

private:
    std::string _input;
};

class ExplainAstNode : public sdk::AggStageAstNode {
public:
    ExplainAstNode(StringData input) : _input(input) {}

    std::unique_ptr<sdk::LogicalAggStage> bind() const override {
        return std::make_unique<ExplainLogicalStage>(_input);
    }

private:
    std::string _input;
};

class ExplainParseNode : public sdk::AggStageParseNode {
public:
    ExplainParseNode(StringData input) : sdk::AggStageParseNode(kExplainStageName), _input(input) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<sdk::VariantNode> expand() const override {
        std::vector<sdk::VariantNode> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNode(std::make_unique<ExplainAstNode>(_input)));
        return expanded;
    }

    BSONObj getQueryShape(const ::MongoExtensionHostQueryShapeOpts* ctx) const override {
        return BSONObj();
    }

private:
    std::string _input;
};

/**
 * Stage with a non-default explain implementation. Syntax:
 *
 * {$explain: {input: "any string"}}
 *
 * Explain will output the input and the verbosity level.
 */
class ExplainStageDescriptor : public sdk::AggStageDescriptor {
public:
    static inline const std::string kStageName = "$explain";

    ExplainStageDescriptor() : sdk::AggStageDescriptor(kStageName) {}

    std::unique_ptr<sdk::AggStageParseNode> parse(mongo::BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);

        auto arguments = stageBson[kStageName];

        sdk_uassert(
            11239403,
            (str::stream() << "input to " << kStageName << " must be a string " << arguments),
            arguments["input"] && arguments["input"].type() == mongo::BSONType::string);
        auto input = arguments["input"].valueStringDataSafe();

        return std::make_unique<ExplainParseNode>(input);
    }
};

class ExplainExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ExplainStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(ExplainExtension)
DEFINE_GET_EXTENSION()
