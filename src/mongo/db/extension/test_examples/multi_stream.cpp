/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/host_services.h"
#include "mongo/db/extension/sdk/test_extension_factory.h"
#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/byte_buf_utils.h"
#include "mongo/db/extension/shared/extension_status.h"

namespace sdk = mongo::extension::sdk;
using namespace mongo;

using StreamType = sdk::ExecAggStageResultsAndMetadataSource::StreamType;

namespace {

constexpr StringData kByShardField = "byShard"_sd;
constexpr StringData kMetaField = "meta"_sd;
constexpr StringData kNumMetaField = "numMeta"_sd;
constexpr StringData kNumDocsField = "numDocs"_sd;
constexpr StringData kAddStreamTypeField = "addStreamTypeField"_sd;
constexpr StringData kSortPatternField = "sortPattern"_sd;
constexpr StringData kMergePipelineField = "mergePipeline"_sd;

// DRM wrapper / metadata field names.
constexpr StringData kSourceField = "source"_sd;
constexpr StringData kMetadataField = "metadata"_sd;
constexpr StringData kAsField = "as"_sd;
constexpr StringData kSearchMetaName = "SEARCH_META"_sd;

// Emitted document field names.
constexpr StringData kScoreField = "score"_sd;
constexpr StringData kSortKeyField = "$sortKey"_sd;
constexpr StringData kSearchScoreField = "$searchScore"_sd;
constexpr StringData kStreamTypeField = "_streamType"_sd;

// True if 'field' is present at the top level or inside any per-shard override.
bool fieldPresentInAnyShard(const BSONObj& args, StringData field) {
    if (args.hasField(field)) {
        return true;
    }
    if (auto byShard = args[kByShardField]; byShard.type() == BSONType::object) {
        for (const auto& e : byShard.Obj()) {
            if (e.type() == BSONType::object && e.Obj().hasField(field)) {
                return true;
            }
        }
    }
    return false;
}

// Merges this shard's byShard override (if any) on top of the base args. The
// returned object is always owned.
BSONObj resolveEffectiveArgs(const BSONObj& args, const std::string& shardId) {
    if (shardId.empty()) {
        return args.getOwned();
    }
    auto byShardElem = args[kByShardField];
    if (byShardElem.type() != BSONType::object || !byShardElem.Obj().hasField(shardId)) {
        return args.getOwned();
    }
    return args.addFields(byShardElem.Obj()[shardId].Obj());
}

}  // namespace

struct DocEmitConfig {
    int numDocs = 0;
    bool addStreamTypeField = false;

    static DocEmitConfig parse(const BSONObj& args) {
        return {
            args[kNumDocsField].isNumber() ? args[kNumDocsField].safeNumberInt() : 0,
            args[kAddStreamTypeField].booleanSafe(),
        };
    }
};

struct MetaEmitConfig {
    BSONObj meta;
    int numMeta = 1;
    bool skip = false;

    // Shard-side check: the per-shard meta has already been resolved into `meta`.
    bool isActive() const {
        return !meta.isEmpty();
    }

    // Router-side check (shardId not yet known): inspect top-level + byShard args.
    static bool isActiveInArgs(const BSONObj& args) {
        return fieldPresentInAnyShard(args, kMetaField);
    }

    static MetaEmitConfig parse(const BSONObj& args) {
        const bool hasMeta = args.hasField(kMetaField);
        return {
            hasMeta ? args[kMetaField].Obj().getOwned() : BSONObj(),
            args[kNumMetaField].isNumber() ? args[kNumMetaField].safeNumberInt()
                                           : (hasMeta ? 1 : 0),
            false,
        };
    }
};

struct DplConfig {
    BSONObj sortPattern;
    BSONObj mergePipeline;

    static DplConfig parse(const BSONObj& args) {
        return {
            args.hasField(kSortPatternField) ? args[kSortPatternField].Obj().getOwned()
                                             : BSON(kScoreField << -1),
            // Default merge unions every shard's metadata via a blocking $group/$mergeObjects +
            // $replaceRoot. This incorporates a field from each shard's metadata document while
            // staying schema-agnostic. A value-combining merge such as $group/$sum cannot be the
            // default because it must name a numeric field (e.g. {$sum: "$metaVal"}). When all
            // shards return the same metadata the union reproduces it. Tests whose per-shard
            // metadata must be summed (or otherwise combined by value) can supply an explicit,
            // schema-aware mergePipeline via kMergePipelineField.
            args.hasField(kMergePipelineField)
                ? args[kMergePipelineField].Obj().getOwned()
                : BSON_ARRAY(BSON("$group" << BSON("_id" << BSONNULL << "doc"
                                                         << BSON("$mergeObjects" << "$$ROOT")))
                             << BSON("$replaceRoot" << BSON("newRoot" << "$doc"))),
        };
    }
};

/**
 * Emits numMeta metadata docs (kMetaResult) followed by numDocs document results (kDocResult).
 * numMeta drives three test scenarios:
 *   1 (default when "meta" present) — normal two-stream path
 *   0 — triggers $setVariableFromSubPipeline error 625296 (no metadata doc)
 *   2 — triggers $setVariableFromSubPipeline error 625297 (duplicate metadata)
 */
class MultiStreamSourceExecStage : public sdk::ExecAggStageResultsAndMetadataSource {
public:
    MultiStreamSourceExecStage(std::string_view name,
                               DocEmitConfig docConfig,
                               MetaEmitConfig metaConfig)
        : sdk::ExecAggStageResultsAndMetadataSource(name),
          _docConfig(std::move(docConfig)),
          _metaConfig(std::move(metaConfig)) {}

    extension::ExtensionGetNextResult getNext(
        const sdk::QueryExecutionContextHandle& /*execCtx*/,
        ::MongoExtensionExecAggStage* /*execStage*/) override {
        if (!_metaConfig.skip && _metaConfig.isActive() && _metaEmitted < _metaConfig.numMeta) {
            ++_metaEmitted;
            return advanced(_metaConfig.meta, StreamType::kMetaResult);
        }
        if (_nextDoc >= _docConfig.numDocs) {
            return extension::ExtensionGetNextResult::eof();
        }
        int i = _nextDoc++;
        int score = _docConfig.numDocs - i;
        BSONObjBuilder builder;
        builder.append("_id", i);
        builder.append(kScoreField, score);
        builder.append("name", "doc_" + std::to_string(i));
        if (_docConfig.addStreamTypeField) {
            builder.append(kStreamTypeField, -1);
        }
        // $sortKey drives merge-sort across shards. $searchScore is score * 0.125.
        return advanced(
            builder.obj(),
            StreamType::kDocResult,
            BSON(kSortKeyField << BSON_ARRAY(score) << kSearchScoreField << score * 0.125));
    }

    void open() override {}
    void reopen() override {}
    void close() override {}

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return BSONObj();
    }

private:
    const DocEmitConfig _docConfig;
    // Non-const: skipStream() on the logical stage flips `skip` before compile().
    MetaEmitConfig _metaConfig;
    int _nextDoc = 0;
    int _metaEmitted = 0;
};

class MultiStreamSourceLogicalStage : public sdk::LogicalAggStage {
public:
    MultiStreamSourceLogicalStage(std::string_view name,
                                  const BSONObj& args,
                                  std::string shardId = "")
        : sdk::LogicalAggStage(name), _args(args.getOwned()) {
        auto effectiveArgs = resolveEffectiveArgs(_args, shardId);
        _docConfig = DocEmitConfig::parse(effectiveArgs);
        _metaConfig = MetaEmitConfig::parse(effectiveArgs);
    }

    BSONObj serialize() const override {
        return BSON(_name << _args);
    }

    BSONObj explain(const sdk::QueryExecutionContextHandle&,
                    ::MongoExtensionExplainVerbosity) const override {
        return serialize();
    }

    std::unique_ptr<sdk::ExecAggStageBase> compile() const override {
        return std::make_unique<MultiStreamSourceExecStage>(_name, _docConfig, _metaConfig);
    }

    boost::optional<extension::sdk::DistributedPlanLogic> getDistributedPlanLogic() const override {
        return boost::none;
    }

    std::unique_ptr<sdk::LogicalAggStage> clone() const override {
        return std::make_unique<MultiStreamSourceLogicalStage>(*this);
    }

    void skipStream(::MongoExtensionStreamType streamType) override {
        if (streamType == ::MongoExtensionStreamType::kMongoExtensionStreamTypeMetaResult) {
            _metaConfig.skip = true;
        }
    }

private:
    const BSONObj _args;
    DocEmitConfig _docConfig;
    MetaEmitConfig _metaConfig;
};

class MultiStreamSourceAstNode : public sdk::TestAstNode<MultiStreamSourceLogicalStage> {
public:
    using sdk::TestAstNode<MultiStreamSourceLogicalStage>::TestAstNode;

    BSONObj getProperties() const override {
        extension::MongoExtensionStaticProperties properties;
        properties.setRequiresInputDocSource(false);
        properties.setPosition(extension::MongoExtensionPositionRequirementEnum::kFirst);
        properties.setHostType(extension::MongoExtensionHostTypeRequirementEnum::kRunOnceAnyNode);
        properties.setProvidedMetadataFields(std::vector<std::string>{"searchScore"});
        BSONObjBuilder builder;
        properties.serialize(&builder);
        return builder.obj();
    }

    std::unique_ptr<sdk::LogicalAggStage> promote(
        const ::MongoExtensionCatalogContext& catalogContext) const override {
        // Thread the shardId through so the logical stage can resolve per-shard byShard overrides.
        std::string shardId(extension::byteViewAsStringView(catalogContext.shardId));
        return std::make_unique<MultiStreamSourceLogicalStage>(
            getName(), _arguments, std::move(shardId));
    }

    std::unique_ptr<sdk::AggStageAstNode> clone() const override {
        return std::make_unique<MultiStreamSourceAstNode>(getName(), _arguments);
    }
};

DEFAULT_PARSE_NODE(MultiStreamSource)

namespace {

// Owns the C DPL callback trampoline and its heap-allocated user-data. The host
// (DPLCallbackOwner) runs the destroy hook exactly once, so the new/destroy
// pairing lives here in a single place. All members are null when metadata is
// inactive (no distributed plan logic needed).
struct DplCallbackBundle {
    ::MongoExtensionDocResultsDPLCallback callback = nullptr;
    void* userData = nullptr;
    void (*destroy)(void*) = nullptr;
};

DplCallbackBundle makeDplCallback(const BSONObj& args) {
    auto* data = new DplConfig(DplConfig::parse(args));
    const auto callback = [](void* userData,
                             ::MongoExtensionQueryExecutionContext* /*execCtx*/,
                             ::MongoExtensionByteBuf** sortOut,
                             ::MongoExtensionByteBuf** mergeOut) -> ::MongoExtensionStatus* {
        auto* d = static_cast<DplConfig*>(userData);
        *sortOut = new extension::ByteBuf(d->sortPattern);
        *mergeOut = d->mergePipeline.isEmpty() ? nullptr : new extension::ByteBuf(d->mergePipeline);
        return &extension::ExtensionStatusOK::getInstance();
    };
    const auto destroy = [](void* userData) {
        delete static_cast<DplConfig*>(userData);
    };
    return {callback, data, destroy};
}

// Wraps the $_multiStreamSource args in a $_internalDocumentResultsAndMetadata
// spec, attaching metadata + distributed plan logic only when metadata is active.
extension::AggStageAstNodeHandle expandToDrm(const BSONObj& args) {
    auto& host = sdk::HostServicesAPI::getInstance();
    const bool metaActive = MetaEmitConfig::isActiveInArgs(args);

    BSONObj sourceSpec = BSON("$_multiStreamSource" << args);
    BSONObjBuilder drmInner;
    drmInner.append(kSourceField, sourceSpec);
    if (metaActive) {
        drmInner.append(kMetadataField, BSON(kAsField << kSearchMetaName));
    }
    BSONObj drmSpec = BSON("$_internalDocumentResultsAndMetadata" << drmInner.obj());

    DplCallbackBundle dpl = metaActive ? makeDplCallback(args) : DplCallbackBundle{};
    return host->createDocumentResultsAndMetadata(drmSpec, dpl.callback, dpl.userData, dpl.destroy);
}

}  // namespace

/**
 * $extensionMultiStream desugars into a single $_internalDocumentResultsAndMetadata
 * stage whose source is $_multiStreamSource. When metadata is requested it is
 * exposed as SEARCH_META and a distributed plan logic callback (sortPattern +
 * mergePipeline) is supplied.
 *
 * Arguments (all optional, also accepted under byShard.<shardId> overrides):
 *   numDocs:            <int>   number of document results to emit
 *   numMeta:            <int>   number of metadata docs (0/1/2 exercise DRM edge cases)
 *   meta:               <obj>   metadata payload; presence activates the metadata stream
 *   addStreamTypeField: <bool>  append a debug _streamType field to each document
 *   sortPattern:        <obj>   merge-sort pattern for the DPL callback
 *   mergePipeline:      <array> metadata merge pipeline for the DPL callback
 *   byShard:            <obj>   { <shardId>: { ...per-shard overrides... } }
 */
class ExtensionMultiStreamParseNode : public sdk::AggStageParseNode {
public:
    ExtensionMultiStreamParseNode(std::string_view name, const BSONObj& args)
        : sdk::AggStageParseNode(name), _args(args.getOwned()) {}

    size_t getExpandedSize() const override {
        return 1;
    }

    std::vector<extension::VariantNodeHandle> expand() const override {
        std::vector<extension::VariantNodeHandle> expanded;
        expanded.reserve(1);
        expanded.emplace_back(expandToDrm(_args));
        return expanded;
    }

    BSONObj getQueryShape(const sdk::QueryShapeOptsHandle&) const override {
        return BSONObj();
    }

    BSONObj toBsonForLog() const override {
        return BSON(_name << _args);
    }

    std::unique_ptr<sdk::AggStageParseNode> clone() const override {
        return std::make_unique<ExtensionMultiStreamParseNode>(getName(), _args);
    }

private:
    BSONObj _args;
};

// $_multiStreamSource: source stage that emits the two-stream (doc + metadata)
// output consumed by $_internalDocumentResultsAndMetadata. See
// MultiStreamSourceExecStage for the numMeta scenario matrix.
using MultiStreamSourceStageDescriptor =
    sdk::TestStageDescriptor<"$_multiStreamSource", MultiStreamSourceParseNode>;

// $extensionMultiStream: router-facing stage that desugars into DRM + source.
using ExtensionMultiStreamStageDescriptor =
    sdk::TestStageDescriptor<"$extensionMultiStream", ExtensionMultiStreamParseNode>;

class MultiStreamExtension : public sdk::Extension {
public:
    void initialize(const sdk::HostPortalHandle& portal) override {
        _registerStage<ExtensionMultiStreamStageDescriptor>(portal);
        _registerStage<MultiStreamSourceStageDescriptor>(portal);
    }
};

REGISTER_EXTENSION(MultiStreamExtension)
DEFINE_GET_EXTENSION()
