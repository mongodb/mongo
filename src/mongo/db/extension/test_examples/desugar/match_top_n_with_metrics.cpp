// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * $matchTopNWithMetrics: a $matchTopN clone that ALSO injects an AST-only, no-BSON marker
 * sub-stage ($matchTopNMetrics) at the front of its expansion.
 *
 * Concretely it expands to:
 *   1) $match             -- host BSON node
 *   2) $sort              -- host BSON node (makes the pipeline "ranked" for $rankFusion)
 *   3) $limit             -- host BSON node
 *   4) $matchTopNMetrics  -- an AST-only marker stage emitted directly via
 *                            ExtensionAggStageAstNodeAdapter (NOT via createHostAggStageParseNode),
 *                            so it has no originating host BSON. It advertises
 *                            isSelectionStage:true so the selection gate accepts it.
 */

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/host_portal.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

static const std::string kMatchTopNWithMetricsName = "$matchTopNWithMetrics";
static const std::string kMetricsStageName = "$matchTopNMetrics";
static const std::string kMatchName = "$match";
static const std::string kSortName = "$sort";
static const std::string kLimitName = "$limit";

/**
 * Executable stage for the AST-only marker: a pure passthrough that forwards documents from its
 * source unchanged. It performs no transformation, so it is a valid selection stage.
 */
class MetricsExecAggStage : public sdk::ExecAggStageTransform {
public:
    MetricsExecAggStage() : sdk::ExecAggStageTransform(kMetricsStageName) {}

    mongo::extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& execCtx,
        MongoExtensionExecAggStage* execStage) override {
        return _getSource()->getNext(execCtx.get());
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kMetricsStageName << BSONObj());
    }
};

class MetricsLogicalStage : public sdk::LogicalAggStage {
public:
    MetricsLogicalStage() : sdk::LogicalAggStage(kMetricsStageName) {}

    BSONObj serialize() const override {
        // Serialization used for mongos -> shard dispatch in the sharded case.
        return BSON(kMetricsStageName << BSONObj());
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSON(kMetricsStageName << BSONObj());
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<MetricsExecAggStage>();
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<MetricsLogicalStage>();
    }
};

/**
 * AST-only marker node. Emitted directly (via ExtensionAggStageAstNodeAdapter) during expansion,
 * so it has no host BSON backing it. Advertises isSelectionStage:true so hybrid search's
 * selection-only gate accepts it as an input-pipeline stage.
 */
class MetricsAstNode : public sdk::AggStageAstNode {
public:
    MetricsAstNode() : sdk::AggStageAstNode(kMetricsStageName) {}

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setIsSelectionStage(true);
        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::LogicalAggStage> promote(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        return std::make_unique<MetricsLogicalStage>();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<MetricsAstNode>();
    }
};

/**
 * Parse node for the marker stage. Only reached in the sharded/reparse case, where the router
 * serializes the pipeline (including the placeholder {$matchTopNMetrics: {}}) and re-parses it on
 * the shard. It expands to a single MetricsAstNode.
 */
class MetricsParseNode : public sdk::AggStageParseNode {
public:
    MetricsParseNode() : sdk::AggStageParseNode(kMetricsStageName) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(
            new sdk::ExtensionAggStageAstNodeAdapter(std::make_unique<MetricsAstNode>()));
        return expanded;
    }

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle& ctx) const override {
        return BSON(kMetricsStageName << BSONObj());
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<MetricsParseNode>();
    }
};

/**
 * Even though users never write $matchTopNMetrics directly, we must register a stage descriptor for
 * the sharded case, where mongos serializes the pipeline and sends it to the shards.
 */
class MetricsStageDescriptor
    : public sdk::TestStageDescriptor<"$matchTopNMetrics", MetricsParseNode> {
public:
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        sdk::validateStageDefinition(stageBson, kStageName);
        return std::make_unique<MetricsParseNode>();
    }
};

class MatchTopNWithMetricsParseNode : public sdk::AggStageParseNode {
public:
    // Input: {$matchTopNWithMetrics: {filter: {...}, sort: {...}, limit: <num>}}
    MatchTopNWithMetricsParseNode(BSONObj topN)
        : sdk::AggStageParseNode(kMatchTopNWithMetricsName),
          _input(topN.getOwned()),
          _matchSpec(BSON(kMatchName << _input["filter"].Obj())),
          _sortSpec(BSON(kSortName << _input["sort"].Obj())),
          _limitSpec(BSON(kLimitName << _input["limit"].numberInt())) {}

    size_t getExpandedSize() const override {
        return 4;
    }

    std::vector<mongo::extension::VariantNodeHandle> expand() const override {
        std::vector<mongo::extension::VariantNodeHandle> out;
        out.reserve(getExpandedSize());

        // Expands to four stages, all selection-compatible:
        // 1) Host $match.
        // 2) Host $sort.
        // 3) Host $limit.
        // 4) AST-only $matchTopNMetrics marker (emitted directly, no host BSON).
        auto& hostServices = sdk::HostServicesAPI::getInstance();
        out.emplace_back(hostServices->createHostAggStageParseNode(_matchSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_sortSpec));
        out.emplace_back(hostServices->createHostAggStageParseNode(_limitSpec));
        out.emplace_back(
            new sdk::ExtensionAggStageAstNodeAdapter(std::make_unique<MetricsAstNode>()));

        return out;
    }

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return BSON(kMatchTopNWithMetricsName << _input);
    }

    BSONObj toBsonForLog() const override {
        return BSON(kMatchTopNWithMetricsName << _input);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<MatchTopNWithMetricsParseNode>(_input);
    }

private:
    const BSONObj _input;      // {filter: {...}, sort: {...}, limit: <num>}
    const BSONObj _matchSpec;  // {$match: {...}}
    const BSONObj _sortSpec;   // {$sort: {...}}
    const BSONObj _limitSpec;  // {$limit: <int>}
};

class MatchTopNWithMetricsStageDescriptor
    : public sdk::TestStageDescriptor<"$matchTopNWithMetrics", MatchTopNWithMetricsParseNode> {
public:
    std::unique_ptr<sdk::AggStageParseNode> parse(BSONObj stageBson) const override {
        const auto obj = sdk::validateStageDefinition(stageBson, kStageName);

        sdk_uassert(12559500,
                    "$matchTopNWithMetrics requires 'filter' object",
                    obj.hasField("filter") && obj["filter"].isABSONObj());
        sdk_uassert(12559501,
                    "$matchTopNWithMetrics requires 'sort' object",
                    obj.hasField("sort") && obj["sort"].isABSONObj());

        const auto limitElem = obj["limit"];
        sdk_uassert(12559502, "$matchTopNWithMetrics requires a 'limit' field", !limitElem.eoo());
        const auto swLimit = limitElem.parseIntegerElementToNonNegativeLong();
        sdk_uassert(12559503,
                    "$matchTopNWithMetrics requires a positive integral 'limit'",
                    swLimit.isOK() && swLimit.getValue() >= 1);

        return std::make_unique<MatchTopNWithMetricsParseNode>(obj.getOwned());
    }
};

class MatchTopNWithMetricsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<MetricsStageDescriptor>(portal);
        _registerStage<MatchTopNWithMetricsStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MatchTopNWithMetricsExtension)
DEFINE_GET_EXTENSION()
