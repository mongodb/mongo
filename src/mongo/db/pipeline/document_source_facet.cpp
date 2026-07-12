// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/db/pipeline/document_source_facet.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/explain_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/owned_lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <list>
#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::vector;

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(facet,
                                     DocumentSourceFacet::LiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(facet, DocumentSourceFacet, FacetStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(facet, DocumentSourceFacet::id);

DocumentSourceFacet::DocumentSourceFacet(std::vector<FacetPipeline> facetPipelines,
                                         const intrusive_ptr<ExpressionContext>& expCtx,
                                         size_t bufferSizeBytes,
                                         size_t maxOutputDocBytes)
    : DocumentSource(kStageName, expCtx),
      _facets(std::move(facetPipelines)),
      _execStatsWrapper(std::make_shared<DSFacetExecStatsWrapper>()),
      _bufferSizeBytes(bufferSizeBytes),
      _maxOutputDocSizeBytes(maxOutputDocBytes) {
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        auto& facet = _facets[facetId];
        facet.pipeline->addInitialSource(
            DocumentSourceTeeConsumer::create(expCtx, facetId, kTeeConsumerStageName));
    }
}

namespace {
/**
 * Extracts the names of the facets and the vectors of raw BSONObjs representing the stages within
 * that facet's pipeline.
 *
 * Throws a AssertionException if it fails to parse for any reason.
 */
vector<pair<string, vector<BSONObj>>> extractRawPipelines(const BSONElement& elem) {
    uassert(40169,
            str::stream() << "the $facet specification must be a non-empty object, but found: "
                          << elem,
            elem.type() == BSONType::object && !elem.embeddedObject().isEmpty());

    vector<pair<string, vector<BSONObj>>> rawFacetPipelines;
    for (auto&& facetElem : elem.embeddedObject()) {
        const auto facetName = facetElem.fieldNameStringData();
        uassertStatusOKWithContext(
            FieldPath::validateFieldName(facetName),
            "$facet pipeline names must follow the naming rules of field path expressions.");
        uassert(40170,
                str::stream() << "arguments to $facet must be arrays, " << facetName << " is type "
                              << typeName(facetElem.type()),
                facetElem.type() == BSONType::array);

        vector<BSONObj> rawPipeline;
        for (auto&& subPipeElem : facetElem.Obj()) {
            uassert(40171,
                    str::stream() << "elements of arrays in $facet spec must be non-empty objects, "
                                  << facetName << " argument contained an element of type "
                                  << typeName(subPipeElem.type()) << ": " << subPipeElem,
                    subPipeElem.type() == BSONType::object);
            rawPipeline.push_back(subPipeElem.embeddedObject());
        }

        rawFacetPipelines.emplace_back(std::string{facetName}, std::move(rawPipeline));
    }
    return rawFacetPipelines;
}

/**
 * Helper function to find the stage that violates the $facet requirement. The 'source' is not
 * allowed either directly or because of some of the sources inside its sub-pipelines.
 */
std::string getStageNameNotAllowedInFacet(const DocumentSource& source,
                                          std::string_view parentName) {
    auto sourceName = source.getSourceName();
    if (auto* sub = source.getSubPipeline()) {
        for (const auto& substage : *sub) {
            if (auto stageConstraints = substage->constraints();
                !stageConstraints.isAllowedInsideFacetStage()) {
                return getStageNameNotAllowedInFacet(*substage, sourceName);
            }
        }
        // If we reach this point, none of the sub-stages is violating the $facet requirement. The
        // 'source' stage itself is not allowed.
        return std::string{sourceName};
    }
    if (!parentName.empty()) {
        return fmt::format("{} inside of {}", sourceName, parentName);
    }
    return std::string{sourceName};
}

// Returns true when any $unionWith or $lookup stage in a facet sub-pipeline names a collection
// that resolves to a view. Handles the string shorthand, {coll} object, and cross-db {db, coll}
// object forms for both stages, then checks the resolved-namespace map.
bool facetSubPipelinesTargetView(const vector<pair<string, vector<BSONObj>>>& rawFacetPipelines,
                                 const ResolvedNamespaceMap& resolvedNamespaces,
                                 const DatabaseName& dbName) {
    // Extract a NamespaceString from a $unionWith spec element (string or {coll[, db]} object)
    // or a $lookup "from" element (string or {db, coll} object).
    auto parseNss = [&](const BSONElement& elem) -> boost::optional<NamespaceString> {
        if (elem.type() == BSONType::string) {
            return NamespaceStringUtil::deserialize(dbName, elem.String());
        }
        if (elem.type() == BSONType::object) {
            auto collElem = elem.Obj()["coll"];
            if (collElem.type() != BSONType::string)
                return boost::none;
            if (auto dbElem = elem.Obj()["db"]; dbElem.type() == BSONType::string) {
                auto targetDb = DatabaseNameUtil::deserialize(
                    dbName.tenantId(), dbElem.String(), SerializationContext::stateDefault());
                return NamespaceStringUtil::deserialize(targetDb, collElem.String());
            }
            return NamespaceStringUtil::deserialize(dbName, collElem.String());
        }
        return boost::none;
    };

    for (const auto& [facetName, pipeline] : rawFacetPipelines) {
        for (const auto& stageObj : pipeline) {
            std::string_view stageName = stageObj.firstElementFieldNameStringData();
            boost::optional<NamespaceString> targetNss;

            if (stageName == "$unionWith") {
                targetNss = parseNss(stageObj.firstElement());
            } else if (stageName == "$lookup") {
                if (stageObj.firstElement().type() == BSONType::object) {
                    targetNss = parseNss(stageObj.firstElement().Obj()["from"]);
                }
            }

            if (targetNss) {
                if (auto it = resolvedNamespaces.find(*targetNss);
                    it != resolvedNamespaces.end() && it->second.isInvolvedNamespaceAView()) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

std::unique_ptr<DocumentSourceFacet::LiteParsed> DocumentSourceFacet::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    std::vector<OwnedLiteParsedPipeline> ownedPipelines;

    for (auto&& rawPipeline : extractRawPipelines(spec)) {
        ownedPipelines.emplace_back(nss, rawPipeline.second, options);
    }

    return std::make_unique<DocumentSourceFacet::LiteParsed>(spec, std::move(ownedPipelines));
}

intrusive_ptr<DocumentSourceFacet> DocumentSourceFacet::create(
    std::vector<FacetPipeline> facetPipelines,
    const intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<size_t> bufferSizeBytes,
    size_t maxOutputDocBytes) {
    size_t resolvedBufferSizeBytes = bufferSizeBytes.value_or(
        static_cast<size_t>(loadMemoryLimit(StageMemoryLimit::QueryFacetBufferSizeBytes)
                                .get(expCtx->getOperationContext())));
    return new DocumentSourceFacet(
        std::move(facetPipelines), expCtx, resolvedBufferSizeBytes, maxOutputDocBytes);
}

Value DocumentSourceFacet::serialize(const query_shape::SerializationOptions& opts) const {
    MutableDocument serialized;
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        auto&& facet = _facets[facetId];
        if (opts.isSerializingForExplain()) {
            bool canAddExecPipelineExplain = opts.verbosity &&
                explainPolicyFor(*opts.verbosity).hasExecStats() &&
                _execStatsWrapper->isStatsProviderAttached();
            auto explain = canAddExecPipelineExplain
                ? mergeExplains(facet.pipeline->writeExplainOps(opts),
                                _execStatsWrapper->getExecStats(facetId, opts))
                : facet.pipeline->writeExplainOps(opts);
            serialized[opts.serializeFieldPathFromString(facet.name)] = Value(std::move(explain));
        } else {
            serialized[opts.serializeFieldPathFromString(facet.name)] =
                Value(facet.pipeline->serialize(opts));
        }
    }
    return Value(Document{{"$facet", serialized.freezeToValue()}});
}

void DocumentSourceFacet::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    for (auto&& facet : _facets) {
        for (auto&& source : facet.pipeline->getSources()) {
            source->addInvolvedCollections(collectionNames);
        }
    }
}

intrusive_ptr<DocumentSource> DocumentSourceFacet::optimize() {
    for (auto&& facet : _facets) {
        pipeline_optimization::optimizePipeline(*facet.pipeline);
    }
    return this;
}

void DocumentSourceFacet::detachSourceFromOperationContext() {
    for (auto&& facet : _facets) {
        facet.pipeline->detachFromOperationContext();
    }
}

void DocumentSourceFacet::reattachSourceToOperationContext(OperationContext* opCtx) {
    for (auto&& facet : _facets) {
        facet.pipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceFacet::validateSourceOperationContext(const OperationContext* opCtx) const {
    return getExpCtx()->getOperationContext() == opCtx &&
        std::all_of(_facets.begin(), _facets.end(), [opCtx](const auto& f) {
               return f.pipeline->validateOperationContext(opCtx);
           });
}

StageConstraints DocumentSourceFacet::constraints(PipelineSplitState state) const {
    // Currently we don't split $facet to have a merger part and a shards part (see SERVER-24154).
    // This means that if any stage in any of the $facet pipelines needs to run on router, then the
    // entire $facet stage must run there.
    static const HostTypeRequirement kDefinitiveHost = HostTypeRequirement::kRouter;
    HostTypeRequirement host = HostTypeRequirement::kNone;
    boost::optional<ShardId> mergeShardId;

    // Iterate through each pipeline to determine the HostTypeRequirement for the $facet stage.
    // Because we have already validated that there are no conflicting HostTypeRequirements during
    // parsing, if we observe a host type of 'kRouter' in any of the pipelines then the entire
    // $facet stage must run on router and iteration can stop. At the end of this process, 'host'
    // will be the $facet's final HostTypeRequirement.
    for (auto fi = _facets.begin(); fi != _facets.end() && host != kDefinitiveHost; fi++) {
        const auto& sources = fi->pipeline->getSources();
        for (auto si = sources.cbegin(); si != sources.cend() && host != kDefinitiveHost; si++) {
            const auto subConstraints = (*si)->constraints(state);
            const auto hostReq = subConstraints.resolvedHostTypeRequirement(getExpCtx());

            if (hostReq != HostTypeRequirement::kNone) {
                host = hostReq;
            }

            // Capture the first merging shard requested by a subpipeline.
            if (!mergeShardId) {
                mergeShardId = subConstraints.mergeShardId;
            }
        }
    }

    // Clear the captured merging shard if 'host' is incompatible with merging on a shard.
    if (!(host == HostTypeRequirement::kNone ||
          host == HostTypeRequirement::kCollectionlessSourceRunOnceAnyNode ||
          host == HostTypeRequirement::kTargetedShards)) {
        mergeShardId = boost::none;
    }

    // Resolve the disk use, lookup, and transaction requirement of this $facet by iterating through
    // the children in its facets.
    StageConstraints constraints(StreamType::kBlocking,
                                 PositionRequirement::kNone,
                                 host,
                                 StageConstraints::DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 StageConstraints::TransactionRequirement::kAllowed,
                                 StageConstraints::LookupRequirement::kAllowed,
                                 StageConstraints::UnionRequirement::kAllowed);
    for (const auto& facet : _facets) {
        constraints =
            StageConstraints::getStrictestConstraints(facet.pipeline->getSources(), constraints);
    }

    if (mergeShardId) {
        constraints.mergeShardId = mergeShardId;
    }
    return constraints;
}

DepsTracker::State DocumentSourceFacet::getDependencies(DepsTracker* deps) const {
    for (auto&& facet : _facets) {
        auto subDepsTracker = facet.pipeline->getDependencies(deps->getAvailableMetadata());

        deps->fields.insert(subDepsTracker.fields.begin(), subDepsTracker.fields.end());
        deps->needWholeDocument = deps->needWholeDocument || subDepsTracker.needWholeDocument;
        deps->setNeedsMetadata(subDepsTracker.metadataDeps());

        if (deps->needWholeDocument && deps->getNeedsMetadata(DocumentMetadataFields::kTextScore)) {
            break;
        }
    }

    // We will combine multiple documents into one, and the output document will have new fields, so
    // we will stop looking for dependencies at this point.
    return DepsTracker::State::EXHAUSTIVE_ALL;
}

void DocumentSourceFacet::addVariableRefs(std::set<Variables::Id>* refs) const {
    for (auto&& facet : _facets) {
        facet.pipeline->addVariableRefs(refs);
    }
}

intrusive_ptr<DocumentSource> DocumentSourceFacet::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    auto rawFacetPipelines = extractRawPipelines(elem);

    // $facet bypasses the LPP view resolution code in the aggregate code by reparsing from raw
    // BSON. When featureFlagExtensionsInsideHybridSearch is on, those inner stages skip
    // applyViewToLiteParsed assuming that views were already bound. In these scenarios, retry the
    // aggregate again with the feature flag off so that we have correct view resolution behavior
    // across all subpipelines.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (hybridSearchFlagEnabled) {
        bool hasViewTargetingStage =
            facetSubPipelinesTargetView(rawFacetPipelines,
                                        expCtx->getResolvedNamespaces(),
                                        expCtx->getNamespaceString().dbName());
        search_helpers::throwIfrKickbackIfNecessary(
            hasViewTargetingStage,
            feature_flags::gFeatureFlagExtensionsInsideHybridSearch,
            search_metrics::inFacetKickbackRetryCount,
            "$facet with $unionWith/$lookup targeting a view is not supported with "
            "featureFlagExtensionsInsideHybridSearch enabled.");
    }

    boost::optional<std::string> needsRouter;
    boost::optional<std::string> needsShard;

    std::vector<FacetPipeline> facetPipelines;
    for (auto&& rawFacet : rawFacetPipelines) {
        const auto facetName = rawFacet.first;

        auto pipeline = pipeline_factory::makeFacetPipeline(
            rawFacet.second, expCtx, [](const Pipeline& pipeline) {
                const auto& sources = pipeline.getSources();
                for (auto& stage : sources) {
                    auto stageConstraints = stage->constraints();
                    if (!stageConstraints.isAllowedInsideFacetStage()) {
                        uasserted(40600,
                                  str::stream()
                                      << getStageNameNotAllowedInFacet(*stage, "")
                                      << " is not allowed to be used within a $facet stage");
                    }
                    // We expect a stage within a $facet stage to have these properties.
                    tassert(11294804,
                            str::stream()
                                << "Expecting $facet stage to have no position requirement, got "
                                << static_cast<int>(stageConstraints.requiredPosition),
                            stageConstraints.requiredPosition ==
                                StageConstraints::PositionRequirement::kNone);
                    tassert(11294803,
                            "Expecting $facet stage not to be independent of any collection",
                            !stageConstraints.isIndependentOfAnyCollection);
                }
            });

        // These checks potentially require that we check the catalog to determine where our data
        // lives. In circumstances where we aren't actually running the query, we don't need to do
        // this (and it can erroneously error - SERVER-83912).
        if (expCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()) {
            // Validate that none of the facet pipelines have any conflicting HostTypeRequirements.
            // This verifies both that all stages within each pipeline are consistent, and that the
            // pipelines are consistent with one another.
            if (!needsShard && pipeline->needsShard()) {
                needsShard.emplace(facetName);
            }
            if (!needsRouter && pipeline->needsRouterMerger()) {
                needsRouter.emplace(facetName);
            }
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "$facet pipeline '" << *needsRouter
                                  << "' must run on router, but '" << *needsShard
                                  << "' requires a shard",
                    !(needsShard && needsRouter));
        }

        facetPipelines.emplace_back(facetName, std::move(pipeline));
    }

    return DocumentSourceFacet::create(std::move(facetPipelines), expCtx);
}
}  // namespace mongo
