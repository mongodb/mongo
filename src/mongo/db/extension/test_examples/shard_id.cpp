// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/assert_util.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/get_next_result.h"

#include <string_view>

using namespace std::literals::string_view_literals;

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Test extension that defines $testShardId. It reads catalogContext.shardId in promote(),
 * propagates the value through LogicalStage to ExecStage, and appends a "shardId" string
 * field to each output document.
 *
 * Usage:
 *   {$testShardId: {}}                      - runs on shards (default pushdown)
 *   {$testShardId: {runOnRouter: true}}     - runs on mongos via DistributedPlanLogic
 */

static constexpr auto kRunOnRouterField = "runOnRouter"sv;

class ShardIdExecStage : public sdk::ExecAggStageTransform {
public:
    ShardIdExecStage(std::string_view stageName, std::string shardId)
        : sdk::ExecAggStageTransform(stageName), _shardId(std::move(shardId)) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const mongo::extension::sdk::QueryExecutionContextHandle& execCtx,
        ::MongoExtensionExecAggStage* execStage) override {
        auto input = _getSource()->getNext(execCtx.get());
        if (input.code != extension::GetNextCode::kAdvanced) {
            return input;
        }
        sdk_tassert(12337900, "Expected result document", input.resultDocument.has_value());
        BSONObjBuilder bob(input.resultDocument->getUnownedBSONObj());
        bob.append("shardId", _shardId);
        return extension::ExtensionGetNextResult::advanced(
            extension::ExtensionBSONObj::makeAsByteBuf(bob.done()));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

private:
    const std::string _shardId;
};

class ShardIdLogicalStage : public sdk::LogicalAggStage {
public:
    ShardIdLogicalStage(std::string_view stageName, std::string shardId, bool runOnRouter)
        : sdk::LogicalAggStage(stageName),
          _shardId(std::move(shardId)),
          _runOnRouter(runOnRouter) {}

    BSONObj serialize() const override {
        if (_runOnRouter) {
            return BSON(_name << BSON(kRunOnRouterField << true));
        }
        return BSON(_name << BSONObj());
    }
    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(_name << BSON("shardId" << _shardId << kRunOnRouterField << _runOnRouter));
    }
    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ShardIdExecStage>(_name, _shardId);
    }
    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        if (!_runOnRouter) {
            return boost::none;
        }
        // Place this stage in the merging pipeline so it executes on mongos.
        sdk::DistributedPlanLogic dpl;
        std::vector<extension::VariantDPLHandle> pipeline;
        pipeline.emplace_back(
            extension::LogicalAggStageHandle{new sdk::ExtensionLogicalAggStageAdapter(clone())});
        dpl.mergingPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        return dpl;
    }
    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ShardIdLogicalStage>(_name, _shardId, _runOnRouter);
    }

private:
    const std::string _shardId;
    const bool _runOnRouter;
};

class ShardIdAstNode : public sdk::AggStageAstNode {
public:
    ShardIdAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::AggStageAstNode(stageName), _arguments(arguments.getOwned()) {}

    std::unique_ptr<sdk::LogicalAggStage> promote(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        std::string shardId(extension::byteViewAsStringView(catalogContext.shardId));
        bool runOnRouter =
            _arguments.hasField(kRunOnRouterField) && _arguments[kRunOnRouterField].booleanSafe();
        return std::make_unique<ShardIdLogicalStage>(getName(), std::move(shardId), runOnRouter);
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<ShardIdAstNode>(getName(), _arguments);
    }

private:
    const BSONObj _arguments;
};

DEFAULT_PARSE_NODE(ShardId)

/**
 * Custom descriptor: allow {} (default, runs on shards) or {runOnRouter: true} (runs on mongos).
 */
class ShardIdStageDescriptor
    : public sdk::TestStageDescriptor<"$testShardId", ShardIdParseNode, false> {
public:
    void validate(const BSONObj& arguments) const override {
        if (arguments.isEmpty()) {
            return;
        }
        sdk_uassert(12337901,
                    "$testShardId only accepts {} or {runOnRouter: true}",
                    arguments.nFields() == 1 && arguments.hasField(kRunOnRouterField) &&
                        arguments[kRunOnRouterField].isBoolean());
    }
};

DEFAULT_EXTENSION(ShardId)
REGISTER_EXTENSION(ShardIdExtension)
DEFINE_GET_EXTENSION()
