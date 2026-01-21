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
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/log_util.h"
#include "mongo/db/extension/sdk/test_extension_util.h"
#include "mongo/db/extension/sdk/tests/transform_test_stages.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static const std::string kStageNameVectorSearchOpt = "$testVectorSearchOptimization";

class TestVectorSearchExecStage : public sdk::TestExecStage {
public:
    TestVectorSearchExecStage(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestExecStage(stageName, arguments) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        MongoExtensionExecAggStage* execStage) override {
        auto input = _getSource()->getNext(execCtx.get());
        if (input.code != mongo::extension::GetNextCode::kAdvanced) {
            return input;
        }
        return mongo::extension::ExtensionGetNextResult::advanced(
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(
                input.resultDocument->getUnownedBSONObj()),
            mongo::extension::ExtensionBSONObj::makeAsByteBuf(BSON("$vectorSearchScore" << 50.0)));
    }
};

class TestVectorSearchLogicalStage : public sdk::TestLogicalStage<TestVectorSearchExecStage> {
public:
    TestVectorSearchLogicalStage(std::string_view stageName, const BSONObj& input)
        : sdk::TestLogicalStage<TestVectorSearchExecStage>(stageName, input) {}

    std::unique_ptr<extension::sdk::LogicalAggStage> clone() const override {
        return std::make_unique<TestVectorSearchLogicalStage>(_name, _arguments);
    }

    bool isSortedByVectorSearchScore() const override {
        return true;
    }
};

class TestVectorSearchAstNode : public sdk::TestAstNode<TestVectorSearchLogicalStage> {
public:
    TestVectorSearchAstNode(std::string_view stageName, const mongo::BSONObj& arguments)
        : sdk::TestAstNode<TestVectorSearchLogicalStage>(stageName, arguments) {}

    mongo::BSONObj getProperties() const override {
        mongo::extension::MongoExtensionStaticProperties properties;
        mongo::BSONObjBuilder builder;
        properties.setProvidedMetadataFields(std::vector<std::string>{"vectorSearchScore"});
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<TestVectorSearchAstNode>(_name, _arguments);
    }
};

DEFAULT_PARSE_NODE(TestVectorSearch);

using TestVectorSearchStageDescriptor =
    sdk::TestStageDescriptor<"$testVectorSearch", TestVectorSearchParseNode>;

class TestVectorSearchOptParseNode
    : public sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode> {
public:
    TestVectorSearchOptParseNode(std::string_view stageName, const BSONObj& input)
        : sdk::TestParseNode<sdk::shared_test_stages::TransformAggStageAstNode>(stageName, input) {}

    size_t getExpandedSize() const override {
        bool desugars =
            !_arguments.hasField("desugar") || _arguments.getField("desugar").booleanSafe();
        return desugars ? 2 : 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> result;
        result.reserve(getExpandedSize());
        auto& host = sdk::HostServicesAPI::getInstance();
        result.emplace_back(new sdk::ExtensionAggStageAstNode(
            std::make_unique<TestVectorSearchAstNode>("$testVectorSearch", _arguments)));
        // If desugar is false, then this extension should only desugar into $testVectorSearch
        // and the optimization that removes a $sort stage that sorts by 'vectorSearchScore'
        // directly after the $vectorSearch stage should work.
        bool desugars =
            !_arguments.hasField("desugar") || _arguments.getField("desugar").booleanSafe();
        if (desugars) {
            // The $addFields stage is ineligible for the $sort optimization.
            if (_arguments.getField("ineligibleForSortOptimization").booleanSafe()) {
                result.emplace_back(
                    host->createHostAggStageParseNode(BSON("$addFields" << BSON("cats" << 67))));
            } else {
                if (_arguments.getField("storedSource").booleanSafe()) {
                    result.emplace_back(host->createHostAggStageParseNode(BSON(
                        "$replaceRoot"
                        << BSON("newRoot"
                                << BSON("$ifNull" << BSON_ARRAY("$storedSource" << "$$ROOT"))))));
                } else {
                    result.emplace_back(host->createHostAggStageParseNode(
                        BSON("$_internalSearchIdLookup" << BSON("limit" << 67LL))));
                }
            }
        }
        return result;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<TestVectorSearchOptParseNode>(getName(), _arguments);
    }
};

/**
 * $testVectorSearchOptimization is a transform stage that can accept the following inputs:
 * - desugar: a boolean input that defaults to true if not present and determines whether the stage
 desugars
 * If desugar is true, then it must specify either a storedSource or an
 ineligibleForSortOptimization input.
 * - storedSource: a boolean input that determines whether the stage desugars into an idLookup or
 replaceRoot stage.
 * - ineligibleForSortOptimization: a boolean input that determines whether the stage desugars into
 an pipeline that is ineligible for sort optimization.
 */
class TestVectorSearchOptStageDescriptor
    : public sdk::TestStageDescriptor<"$testVectorSearchOptimization",
                                      TestVectorSearchOptParseNode> {
public:
    void validate(const mongo::BSONObj& arguments) const override {
        // Get desugar field (defaults to true if not present)
        bool desugar = true;
        if (arguments.hasField("desugar")) {
            sdk_uassert(11543601,
                        "expected desugar input to be a boolean " + kStageNameVectorSearchOpt,
                        arguments.getField("desugar").isBoolean());
            desugar = arguments.getField("desugar").boolean();
        }
        if (!desugar) {
            return;
        }

        bool hasStoredSource = arguments.hasField("storedSource");
        bool hasIneligibleForSortOptimization = arguments.hasField("ineligibleForSortOptimization");

        sdk_uassert(11543602,
                    "expected either storedSource or ineligibleForSortOptimization input to " +
                        kStageNameVectorSearchOpt,
                    hasStoredSource || hasIneligibleForSortOptimization);

        if (hasStoredSource) {
            sdk_uassert(11543603,
                        "expected storedSource input to be a boolean " + kStageNameVectorSearchOpt,
                        arguments.getField("storedSource").isBoolean());
        }
        if (hasIneligibleForSortOptimization) {
            sdk_uassert(11543604,
                        "expected ineligibleForSortOptimization input to be a boolean " +
                            kStageNameVectorSearchOpt,
                        arguments.getField("ineligibleForSortOptimization").isBoolean());
        }
    }
};

class TestVectorSearchOptExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<TestVectorSearchOptStageDescriptor>(portal);
        _registerStage<TestVectorSearchStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(TestVectorSearchOptExtension)
DEFINE_GET_EXTENSION()
