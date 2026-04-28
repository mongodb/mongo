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
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/sdk/tests/shared_test_stages.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

/**
 * Execution stage that produces sequential integer _ids. When _produceScore is true (determined
 * by whether the downstream pipeline suffix needs "score" metadata), each document also gets
 * $score metadata set to _id * 5.
 */
class ProduceIdsExecStage : public sdk::ExecAggStageSource {
public:
    ProduceIdsExecStage(std::string_view stageName, bool sortById, int numDocs, bool produceScore)
        : sdk::ExecAggStageSource(stageName),
          _sortById(sortById),
          _numDocs(numDocs),
          _produceScore(produceScore) {}

    extension::ExtensionGetNextResult getNext(const sdk::QueryExecutionContextHandle& execCtx,
                                              ::MongoExtensionExecAggStage* execStage) override {
        if (_currentDoc == _numDocs) {
            // At EOF, all idLookup processing for previously produced IDs is complete. Capture the
            // accumulated idLookup metrics into this stage's extension operation metrics.
            auto metrics = execCtx->getMetrics(execStage);
            BSONObj hostMetrics = execCtx->getHostMetrics(
                {"docsSeenByIdLookup", "docsReturnedByIdLookup", "idLookupSuccessRate"});
            metrics->update(extension::objAsByteView(hostMetrics));
            return extension::ExtensionGetNextResult::eof();
        }

        // Generate zero-indexed, ascending ids.
        auto currentId = _currentDoc++;
        auto document = extension::ExtensionBSONObj::makeAsByteBuf(BSON("_id" << currentId));

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
    int _currentDoc = 0;
};

/**
 * Logical stage for $produceIds. Overrides applyPipelineSuffixDependencies() to conditionally
 * enable $score metadata production: if the downstream pipeline suffix needs "score" metadata,
 * _produceScore is set to true and the compiled exec stage will emit score = _id * 5.
 */
class ProduceIdsLogicalStage : public sdk::LogicalAggStage {
public:
    ProduceIdsLogicalStage(std::string_view stageName, const BSONObj& arguments)
        : ProduceIdsLogicalStage(stageName,
                                 arguments["sortById"] && arguments["sortById"].booleanSafe(),
                                 arguments["numDocs"] && arguments["numDocs"].isNumber()
                                     ? arguments["numDocs"].safeNumberInt()
                                     : 1,
                                 arguments["produceScore"].booleanSafe()) {}

    ProduceIdsLogicalStage(std::string_view stageName,
                           bool sortById,
                           int numDocs,
                           bool produceScore)
        : sdk::LogicalAggStage(stageName),
          _sortById(sortById),
          _numDocs(numDocs),
          _produceScore(produceScore) {}

    BSONObj serialize() const override {
        BSONObjBuilder spec;
        spec.append("numDocs", _numDocs);
        if (_sortById)
            spec.appendBool("sortById", true);
        if (_produceScore)
            spec.appendBool("produceScore", true);
        return BSON(_name << spec.obj());
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return serialize();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<ProduceIdsExecStage>(_name, _sortById, _numDocs, _produceScore);
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<ProduceIdsLogicalStage>(_name, _sortById, _numDocs, _produceScore);
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
        // We will generate _ids from [0, numDocs). We can turn this range into a filter that can be
        // used for shard targeting.
        auto rangeFilter = BSON("$gte" << 0 << "$lt" << _numDocs);
        return BSON("_id" << rangeFilter);
    }

    void applyPipelineSuffixDependencies(
        const extension::PipelineDependenciesHandle& deps) override {
        _produceScore = deps->needsMetadata("score");
    }

private:
    const bool _sortById;
    const int _numDocs;
    bool _produceScore;
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
        properties.setProvidedMetadataFields(std::vector<std::string>{"score"});

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
    }
};
REGISTER_EXTENSION(ReadNDocumentsExtension)
DEFINE_GET_EXTENSION()
