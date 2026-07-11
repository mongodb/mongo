// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/extension/sdk/aggregation_stage.h"
#include "mongo/db/extension/sdk/extension_factory.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"

#include <string_view>

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Execution stage that produces sequential integer _ids. When _produceScore is true (determined
 * by whether the downstream pipeline suffix needs "score" metadata), each document also gets
 * $score metadata set to _id * 5. When _valueNeeded is true, each document also includes a
 * "value" field equal to _id * 2. When _labelNeeded is true, each document also includes a
 * "label" field equal to "doc_<id>".
 */
class ProduceIdsExecStage : public sdk::ExecAggStageSource {
public:
    ProduceIdsExecStage(std::string_view stageName,
                        bool sortById,
                        int numDocs,
                        bool produceScore,
                        bool valueNeeded,
                        int startId,
                        bool labelNeeded)
        : sdk::ExecAggStageSource(stageName),
          _sortById(sortById),
          _numDocs(numDocs),
          _produceScore(produceScore),
          _valueNeeded(valueNeeded),
          _labelNeeded(labelNeeded),
          _currentDoc(startId) {}

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              ::MongoExtensionExecAggStage* execStage) override {
        if (_currentDoc >= _numDocs) {
            // At EOF, all idLookup processing for previously produced IDs is complete. Capture the
            // accumulated idLookup metrics into this stage's extension operation metrics.
            auto metrics = execCtx->getMetrics(execStage);
            BSONObj hostMetrics = execCtx->getHostMetrics(
                {"docsSeenByIdLookup", "docsReturnedByIdLookup", "idLookupSuccessRate"});
            metrics->update(extension::objAsByteView(hostMetrics));
            return extension::ExtensionGetNextResult::eof();
        }

        // Generate ascending ids starting from _startId.
        auto currentId = _currentDoc++;
        BSONObjBuilder docBuilder;
        docBuilder.append("_id", currentId);
        if (_valueNeeded)
            docBuilder.append("value", currentId * 2);
        if (_labelNeeded)
            docBuilder.append("label", "doc_" + std::to_string(currentId));
        auto document = extension::ExtensionBSONObj::makeAsByteBuf(docBuilder.obj());

        if (!_sortById && !_produceScore) {
            // We haven't been asked for sorted results or a $score, no need to generate metadata.
            return extension::ExtensionGetNextResult::advanced(std::move(document));
        }

        BSONObjBuilder metaBuilder;
        if (_sortById) {
            metaBuilder.append("$sortKey", BSON("_id" << currentId));
        }
        if (_produceScore) {
            metaBuilder.append("$score", static_cast<double>(currentId) * 5);
        }
        auto metadata = extension::ExtensionBSONObj::makeAsByteBuf(metaBuilder.obj());
        return extension::ExtensionGetNextResult::advanced(std::move(document),
                                                           std::move(metadata));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}
    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity verbosity) const override {
        return BSONObj();
    }

    std::unique_ptr<sdk::OperationMetricsBase> createMetrics() const override {
        return std::make_unique<sdk::shared_test_stages::BSONSnapshotOperationMetrics>();
    }

private:
    const bool _sortById;
    const int _numDocs;
    const bool _produceScore;
    const bool _valueNeeded;
    const bool _labelNeeded;
    int _currentDoc;
};

/**
 * Logical stage for $produceIds.
 *
 * Match pushdown — "applyMatchPushdown" (reordering rule): folds a subsequent $match with an _id
 * lower-bound filter into _startId, eliminating the $match from the pipeline.
 *
 * Project pushdown — "applyProjectPushdown" (in-place rule): reads the required-fields set
 * computed by dependency analysis (stored by applyPipelineSuffixDependencies) and suppresses
 * any output fields not needed by the downstream pipeline.
 */
class ProduceIdsLogicalStage : public sdk::LogicalAggStage {
public:
    ProduceIdsLogicalStage(std::string_view stageName, const BSONObj& arguments)
        : ProduceIdsLogicalStage(
              stageName,
              arguments["sortById"] && arguments["sortById"].booleanSafe(),
              arguments["numDocs"] && arguments["numDocs"].isNumber()
                  ? arguments["numDocs"].safeNumberInt()
                  : 1,
              arguments["produceScore"].booleanSafe(),
              !arguments["skipValue"].booleanSafe(),
              arguments["startId"].isNumber() ? arguments["startId"].safeNumberInt() : 0,
              !arguments["skipLabel"].booleanSafe()) {}

    ProduceIdsLogicalStage(std::string_view stageName,
                           bool sortById,
                           int numDocs,
                           bool produceScore,
                           bool valueNeeded = true,
                           int startId = 0,
                           bool labelNeeded = true)
        : sdk::LogicalAggStage(stageName),
          _sortById(sortById),
          _numDocs(numDocs),
          _produceScore(produceScore),
          _valueNeeded(valueNeeded),
          _labelNeeded(labelNeeded),
          _startId(startId) {}

    BSONObj serialize() const override {
        BSONObjBuilder spec;
        spec.append("numDocs", _numDocs);
        if (_sortById)
            spec.appendBool("sortById", true);
        if (_produceScore)
            spec.appendBool("produceScore", true);
        if (!_valueNeeded)
            spec.appendBool("skipValue", true);
        if (!_labelNeeded)
            spec.appendBool("skipLabel", true);
        if (_startId != 0)
            spec.append("startId", _startId);
        return BSON(_name << spec.obj());
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return serialize();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ProduceIdsExecStage>(
            _name, _sortById, _numDocs, _produceScore, _valueNeeded, _startId, _labelNeeded);
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ProduceIdsLogicalStage>(
            _name, _sortById, _numDocs, _produceScore, _valueNeeded, _startId, _labelNeeded);
    }

    BSONObj getSortPattern() const override {
        if (_sortById) {
            return BSON("_id" << 1);
        }
        return BSONObj();
    }

    boost::optional<extension::sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        // We only need to provide distributed planning logic for correctness when we
        // need to sort by _id, but we specify it unconditionally to force more interesting
        // distributed planning.
        extension::sdk::DistributedPlanLogic dpl;

        {
            std::vector<extension::VariantDPLHandle> pipeline;
            pipeline.emplace_back(extension::LogicalAggStageHandle{
                new sdk::ExtensionLogicalAggStageAdapter(clone())});
            dpl.shardsPipeline = sdk::DPLArrayContainer(std::move(pipeline));
        }

        if (_sortById) {
            dpl.sortPattern = BSON("_id" << 1);
        }

        return dpl;
    }

    BSONObj getFilter() const override {
        // We will generate _ids from [_startId, _numDocs). We can turn this range into a filter
        // that can be used for shard targeting.
        auto rangeFilter = BSON("$gte" << _startId << "$lt" << _numDocs);
        return BSON("_id" << rangeFilter);
    }

    void applyPipelineSuffixDependencies(
        const extension::PipelineDependenciesHandle& deps) override {
        _produceScore = deps->needsMetadata("score");
        // Store the required-fields set for the "applyProjectPushdown" in-place rule to consume.
        // boost::none means the whole document is needed. After the rule fires it
        // resets _neededFields back to boost::none to prevent re-firing on later passes.
        _neededFields = deps->getNeededFields();
    }

    bool evaluatePipelineRewriteRulePrecondition(
        std::string_view ruleName,
        extension::ConstPipelineRewriteContextHandle ctx) const override {
        // $readNDocuments desugars to [$produceIds, $_internalSearchIdLookup, ...]. From
        // $produceIds, index 1 is the id-lookup stage and index 2 is the first user-added stage.
        if (ruleName == "applyMatchPushdown") {
            if (!ctx->hasAtLeastNNextStages(2) || ctx->getNthNextStage(2)->getName() != "$match")
                return false;
            auto bound = extractIdLowerBound(ctx->getNthNextStage(2)->getFilter());
            if (!bound)
                return false;
            _startId = *bound;
            return true;
        }
        if (ruleName == "applyProjectPushdown") {
            return _neededFields.has_value();
        }
        return false;
    }

    bool evaluatePipelineRewriteRuleTransform(
        std::string_view ruleName, extension::PipelineRewriteContextHandle ctx) override {
        if (ruleName == "applyMatchPushdown") {
            ctx->eraseNthNext(2);
            return true;
        }
        if (ruleName == "applyProjectPushdown") {
            _valueNeeded = false;
            _labelNeeded = false;
            for (auto elem : *_neededFields) {
                auto name = elem.valueStringDataSafe();
                if (name == "value")
                    _valueNeeded = true;
                else if (name == "label")
                    _labelNeeded = true;
            }
            _neededFields = boost::none;
            return false;
        }
        return false;
    }

    static boost::optional<int> extractIdLowerBound(const BSONObj& filter) {
        auto idElem = filter["_id"];
        if (idElem.eoo())
            return boost::none;
        if (auto gt = idElem["$gt"]; !gt.eoo())
            return gt.safeNumberInt() + 1;
        if (auto gte = idElem["$gte"]; !gte.eoo())
            return gte.safeNumberInt();
        return boost::none;
    }

private:
    const bool _sortById;
    const int _numDocs;
    bool _produceScore;
    bool _valueNeeded;
    bool _labelNeeded;
    mutable int _startId;
    boost::optional<BSONObj> _neededFields;
};

class ProduceIdsAstNode : public sdk::TestAstNode<ProduceIdsLogicalStage> {
public:
    ProduceIdsAstNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestAstNode<ProduceIdsLogicalStage>(stageName, arguments) {}

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setPosition(extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setRequiresInputDocSource(false);
        properties.setAllowedInFacet(false);

        // When sorting by _id, this stage produces a merge sort pattern in its distributed plan
        // logic and attaches $sortKey metadata to every document.
        std::vector<std::string> providedMetadataFields{"score"};
        if (_arguments["sortById"].booleanSafe()) {
            providedMetadataFields.push_back("sortKey");
        }
        properties.setProvidedMetadataFields(std::move(providedMetadataFields));

        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<ProduceIdsAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(ProduceIds);

class ReadNDocumentsParseNode : public sdk::TestParseNode<ProduceIdsAstNode> {
public:
    ReadNDocumentsParseNode(std::string_view stageName, const BSONObj& arguments)
        : sdk::TestParseNode<ProduceIdsAstNode>(stageName, arguments) {}

    size_t getExpandedSize() const override {
        return 2;
    }

    std::vector<extension::VariantNodeHandle> expand() const override {
        std::vector<extension::VariantNodeHandle> expanded;
        expanded.reserve(getExpandedSize());
        expanded.emplace_back(new sdk::ExtensionAggStageAstNodeAdapter(
            std::make_unique<ProduceIdsAstNode>("$produceIds", _arguments)));
        expanded.emplace_back(
            extension::sdk::HostServicesAPI::getInstance()->createIdLookup(kIdLookupSpec));
        return expanded;
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<ReadNDocumentsParseNode>(getName(), _arguments);
    }

private:
    static const BSONObj kIdLookupSpec;
};

const BSONObj ReadNDocumentsParseNode::kIdLookupSpec =
    BSON("$_internalSearchIdLookup" << BSONObj());


/**
 * ReadNDocuments stage that will read documents with integer ids from a collection and will
 * optionally return them sorted by _id value (ascending). Syntax:
 *
 * {$readNDocuments: {numDocs: <int>, sortById: <optional bool>}}
 *
 * $readNDocuments will desugar to two stages: $produceIds, which will produce integer ids up to
 * 'numDocs', followed by $_internalSearchIdLookup, which will attempt to read documents with the
 * given ids from the collection.
 */
using ReadNDocumentsStageDescriptor =
    sdk::TestStageDescriptor<"$readNDocuments", ReadNDocumentsParseNode>;
// $produceIds needs to be parseable to be used in sharded execution.
using ProduceIdsStageDescriptor = sdk::TestStageDescriptor<"$produceIds", ProduceIdsParseNode>;

class ReadNDocumentsExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ReadNDocumentsStageDescriptor>(portal);
        _registerStage<ProduceIdsStageDescriptor>(portal);
        _registerStageRules<ProduceIdsStageDescriptor>(
            portal,
            {
                {"applyMatchPushdown", extension::kReordering},
                {"applyProjectPushdown", extension::kInPlace},
            });
    }
};
REGISTER_EXTENSION(ReadNDocumentsExtension)
DEFINE_GET_EXTENSION()
