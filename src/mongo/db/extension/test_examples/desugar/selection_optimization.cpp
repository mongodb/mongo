// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * $testSelectionOptimization: a selection extension stage with optimization hooks.
 *
 * This stage is a passthrough that optionally skips documents whose integer _id is below a
 * configurable startId threshold. It declares isSelectionStage: true so it is valid inside
 * $rankFusion/$scoreFusion input pipelines, making it the vehicle for testing that optimization
 * rules fire on extension stages in that context.
 *
 * Optimization rules:
 *   applyPipelineBounds (in-place): reads the DocsNeededBounds of the pipeline suffix and stores
 *       them in the stage's state, visible in explain output.
 *   applyMatchPushdown (reordering): folds a following $match{_id:{$gte:N}} into the stage's
 *       startId and erases the $match from the pipeline.
 *
 * Syntax: {$testSelectionOptimization: {startId: <int, default 0>}}
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
using mongo::extension::PipelineRewriteRule;

// Passes documents through, skipping any whose integer _id is below _startId.
class TestSelectionOptExecStage : public sdk::ExecAggStageTransform {
public:
    TestSelectionOptExecStage(std::string_view stageName, int startId)
        : sdk::ExecAggStageTransform(stageName), _startId(startId) {}

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              MongoExtensionExecAggStage* execStage) override {
        while (true) {
            auto result = _getSource()->getNext(execCtx.get());
            if (result.code != extension::GetNextCode::kAdvanced)
                return result;
            auto doc = result.resultDocument->getUnownedBSONObj();
            auto idElem = doc["_id"];
            if (idElem.isNumber() && idElem.safeNumberInt() < _startId)
                continue;
            return result;
        }
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    MongoExtensionExplainVerbosity) const override {
        return BSONObj();
    }

private:
    const int _startId;
};

// Logical stage that accumulates optimization results (DocsNeededBounds and any absorbed $match
// lower bound) during planning, then compiles to TestSelectionOptExecStage.
class TestSelectionOptLogicalStage : public sdk::LogicalAggStage {
public:
    TestSelectionOptLogicalStage(std::string_view stageName, const BSONObj& arguments)
        : sdk::LogicalAggStage(stageName),
          _startId(arguments["startId"].isNumber() ? arguments["startId"].safeNumberInt() : 0) {}

    BSONObj serialize() const override {
        return BSON(_name << _buildSpec());
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    MongoExtensionExplainVerbosity) const override {
        return serialize();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<TestSelectionOptExecStage>(_name, _startId);
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<TestSelectionOptLogicalStage>(*this);
    }

    boost::optional<sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }

    // applyPipelineBounds fires unconditionally; applyMatchPushdown requires an immediately
    // following $match that is exactly a single numeric _id lower-bound predicate.
    bool evaluatePipelineRewriteRulePrecondition(
        std::string_view ruleName,
        extension::ConstPipelineRewriteContextHandle ctx) const override {
        if (ruleName == "applyPipelineBounds")
            return true;
        if (ruleName == "applyMatchPushdown") {
            if (!ctx->hasAtLeastNNextStages(1) || ctx->getNthNextStage(1)->getName() != "$match")
                return false;
            return _extractIdLowerBound(ctx->getNthNextStage(1)->getFilter()).has_value();
        }
        return false;
    }

    // applyPipelineBounds snapshots the pipeline-suffix bounds into stage state; applyMatchPushdown
    // raises _startId by the extracted lower bound (never lowers it) and erases the $match.
    bool evaluatePipelineRewriteRuleTransform(
        std::string_view ruleName, extension::PipelineRewriteContextHandle ctx) override {
        if (ruleName == "applyPipelineBounds") {
            auto bounds = ctx->getPipelineSuffixBounds();
            _minBoundsType = bounds.minBounds.type;
            _maxBoundsType = bounds.maxBounds.type;
            _extractedLimit = boost::none;
            if (_maxBoundsType == kDocsNeededConstraintDiscrete) {
                _extractedLimit = static_cast<int>(bounds.maxBounds.value);
            }
            return false;
        }
        if (ruleName == "applyMatchPushdown") {
            auto bound = _extractIdLowerBound(ctx->getNthNextStage(1)->getFilter());
            if (bound && *bound > _startId)
                _startId = *bound;
            return ctx->eraseNthNext(1);
        }
        return false;
    }

private:
    static std::string_view _boundsTypeStr(MongoExtensionDocsNeededConstraintType type) {
        switch (type) {
            case kDocsNeededConstraintDiscrete:
                return "discrete";
            case kDocsNeededConstraintNeedAll:
                return "needAll";
            case kDocsNeededConstraintUnknown:
                return "unknownConstraints";
            default:
                return "unknown";
        }
    }

    BSONObj _buildSpec() const {
        BSONObjBuilder b;
        b.append("startId", _startId);
        b.append("minBoundsType", _boundsTypeStr(_minBoundsType));
        b.append("maxBoundsType", _boundsTypeStr(_maxBoundsType));
        if (_extractedLimit)
            b.append("extractedLimit", *_extractedLimit);
        return b.obj();
    }

    static boost::optional<int> _extractIdLowerBound(const BSONObj& filter) {
        // Reject filters with predicates on fields other than _id (e.g. {_id:{$gte:2}, x:1}).
        // Erasing the entire $match would silently drop those other predicates.
        if (filter.nFields() != 1)
            return boost::none;
        auto idElem = filter["_id"];
        if (idElem.eoo() || idElem.type() != BSONType::object)
            return boost::none;
        // Reject compound _id predicates (e.g. {$gte:2, $lte:10}); only a sole lower-bound
        // operator is safe to absorb because the entire $match is erased.
        auto idPred = idElem.Obj();
        if (idPred.nFields() != 1)
            return boost::none;
        if (auto gte = idPred["$gte"]; !gte.eoo())
            return gte.isNumber() ? boost::optional<int>{gte.safeNumberInt()} : boost::none;
        if (auto gt = idPred["$gt"]; !gt.eoo())
            return gt.isNumber() ? boost::optional<int>{gt.safeNumberInt() + 1} : boost::none;
        return boost::none;
    }

    int _startId;
    MongoExtensionDocsNeededConstraintType _minBoundsType = kDocsNeededConstraintUnknown;
    MongoExtensionDocsNeededConstraintType _maxBoundsType = kDocsNeededConstraintUnknown;
    boost::optional<int> _extractedLimit;
};

// AST node that advertises isSelectionStage: true, enabling use inside $rankFusion/$scoreFusion
// input pipelines (the selection-stage validator rejects stages that omit this property).
class TestSelectionOptAstNode : public sdk::TestAstNode<TestSelectionOptLogicalStage> {
public:
    TestSelectionOptAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestAstNode<TestSelectionOptLogicalStage>(stageName, arguments) {}

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setIsSelectionStage(true);
        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<TestSelectionOptAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(TestSelectionOpt)

class TestSelectionOptStageDescriptor
    : public sdk::TestStageDescriptor<"$testSelectionOptimization", TestSelectionOptParseNode> {};

// Extension registering the stage and its two optimization rules: applyPipelineBounds (in-place)
// and applyMatchPushdown (reordering).
class TestSelectionOptExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<TestSelectionOptStageDescriptor>(portal);
        std::vector<PipelineRewriteRule> rules{
            {"applyPipelineBounds", kPipelineRewriteRuleTagInPlace},
            {"applyMatchPushdown", kPipelineRewriteRuleTagReordering},
        };
        _registerStageRules<TestSelectionOptStageDescriptor>(portal, rules);
    }
};

REGISTER_EXTENSION(TestSelectionOptExtension)
DEFINE_GET_EXTENSION()
