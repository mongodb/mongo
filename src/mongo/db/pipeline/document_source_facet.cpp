/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/db/pipeline/document_source_facet.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_tee_consumer.h"
#include "mongo/db/pipeline/explain_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/tee_buffer.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <list>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::pair;
using std::string;
using std::vector;

DocumentSourceFacet::DocumentSourceFacet(std::vector<FacetPipeline> facetPipelines,
                                         const intrusive_ptr<ExpressionContext>& expCtx,
                                         size_t bufferSizeBytes,
                                         size_t maxOutputDocBytes)
    : DocumentSource(kStageName, expCtx),
      exec::agg::Stage(kStageName, expCtx),
      _teeBuffer(TeeBuffer::create(facetPipelines.size(), bufferSizeBytes)),
      _facets(std::move(facetPipelines)),
      _maxOutputDocSizeBytes(maxOutputDocBytes) {
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        auto& facet = _facets[facetId];
        facet.pipeline->addInitialSource(
            DocumentSourceTeeConsumer::create(pExpCtx, facetId, _teeBuffer, kTeeConsumerStageName));
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
                                          const std::string& parentName) {
    auto* subPipeline = source.getSubPipeline();
    const std::string& sourceName = source.getSourceName();
    if (!subPipeline) {
        if (!parentName.empty()) {
            return str::stream() << sourceName << " inside of " << parentName;
        } else {
            return str::stream() << sourceName;
        }
    } else {
        for (const auto& substage : *subPipeline) {
            auto stageConstraints = substage->constraints();
            if (!stageConstraints.isAllowedInsideFacetStage()) {
                return getStageNameNotAllowedInFacet(*substage, sourceName);
            }
        }
        // If we reach this point, none of the sub-stages is violating the $facet requirement. The
        // 'source' stage itself is not allowed.
        return str::stream() << sourceName;
    }

    MONGO_UNREACHABLE_TASSERT(8045600);
}

}  // namespace

std::unique_ptr<DocumentSourceFacet::LiteParsed> DocumentSourceFacet::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    std::vector<LiteParsedPipeline> liteParsedPipelines;

    for (auto&& rawPipeline : extractRawPipelines(spec)) {
        liteParsedPipelines.emplace_back(nss, rawPipeline.second);
    }

    return std::make_unique<DocumentSourceFacet::LiteParsed>(spec.fieldName(),
                                                             std::move(liteParsedPipelines));
}

REGISTER_DOCUMENT_SOURCE(facet,
                         DocumentSourceFacet::LiteParsed::parse,
                         DocumentSourceFacet::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(facet, DocumentSourceFacet::id)

intrusive_ptr<DocumentSourceFacet> DocumentSourceFacet::create(
    std::vector<FacetPipeline> facetPipelines,
    const intrusive_ptr<ExpressionContext>& expCtx,
    size_t bufferSizeBytes,
    size_t maxOutputDocBytes) {
    return new DocumentSourceFacet(
        std::move(facetPipelines), expCtx, bufferSizeBytes, maxOutputDocBytes);
}

void DocumentSourceFacet::setSource(Stage* source) {
    _teeBuffer->setSource(source);
}

void DocumentSourceFacet::doDispose() {
    for (auto&& facet : _facets) {
        facet.pipeline.get_deleter().dismissDisposal();
        facet.pipeline->dispose(pExpCtx->getOperationContext());
    }
}

DocumentSource::GetNextResult DocumentSourceFacet::doGetNext() {
    if (_done) {
        // stats update (previously done in usedDisk())
        _stats.planSummaryStats.usedDisk =
            std::any_of(_facets.begin(), _facets.end(), [](const auto& facet) {
                return facet.execPipeline && facet.execPipeline->usedDisk();
            });

        return GetNextResult::makeEOF();
    }

    // Create execution pipeline for each facet (this code is executed only once).
    for (auto&& facet : _facets) {
        tassert(10616300, "facet execution pipeline is already initialized", !facet.execPipeline);
        facet.execPipeline =
            exec::agg::buildPipeline(facet.pipeline->getSources(), facet.pipeline->getContext());
    }

    const size_t maxBytes = _maxOutputDocSizeBytes;
    auto ensureUnderMemoryLimit = [usedBytes = 0ul, &maxBytes](long long additional) mutable {
        usedBytes += additional;
        uassert(4031700,
                str::stream() << "document constructed by $facet is " << usedBytes
                              << " bytes, which exceeds the limit of " << maxBytes << " bytes",
                usedBytes <= maxBytes);
    };

    vector<vector<Value>> results(_facets.size());
    bool allPipelinesEOF = false;
    while (!allPipelinesEOF) {
        allPipelinesEOF = true;  // Set this to false if any pipeline isn't EOF.
        for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
            auto& execPipeline = *_facets[facetId].execPipeline;
            auto next = execPipeline.getNextResult();
            for (; next.isAdvanced(); next = execPipeline.getNextResult()) {
                ensureUnderMemoryLimit(next.getDocument().getApproximateSize());
                results[facetId].emplace_back(next.releaseDocument());
            }
            allPipelinesEOF = allPipelinesEOF && next.isEOF();
            execPipeline.accumulatePlanSummaryStats(_stats.planSummaryStats);
        }
    }

    MutableDocument resultDoc;
    for (size_t facetId = 0; facetId < _facets.size(); ++facetId) {
        resultDoc[_facets[facetId].name] = Value(std::move(results[facetId]));
    }

    _done = true;  // We will only ever produce one result.
    return resultDoc.freeze();
}

Value DocumentSourceFacet::serialize(const SerializationOptions& opts) const {
    MutableDocument serialized;

    for (auto&& facet : _facets) {
        if (opts.isSerializingForExplain()) {
            bool canAddExecPipelineExplain =
                opts.verbosity >= ExplainOptions::Verbosity::kExecStats && facet.execPipeline;
            auto explain = canAddExecPipelineExplain
                ? mergeExplains(*facet.pipeline, *facet.execPipeline, opts)
                : facet.pipeline->writeExplainOps(opts);
            serialized[opts.serializeFieldPathFromString(facet.name)] = Value(explain);
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
        facet.pipeline->optimizePipeline();
    }
    return this;
}

void DocumentSourceFacet::detachFromOperationContext() {
    for (auto&& facet : _facets) {
        if (facet.execPipeline) {
            facet.execPipeline->detachFromOperationContext();
        }
        if (facet.pipeline) {
            facet.pipeline->detachFromOperationContext();
        }
    }
}

void DocumentSourceFacet::detachSourceFromOperationContext() {
    for (auto&& facet : _facets) {
        if (facet.execPipeline) {
            facet.execPipeline->detachFromOperationContext();
        }
        if (facet.pipeline) {
            facet.pipeline->detachFromOperationContext();
        }
    }
}

void DocumentSourceFacet::reattachToOperationContext(OperationContext* opCtx) {
    for (auto&& facet : _facets) {
        if (facet.execPipeline) {
            facet.execPipeline->reattachToOperationContext(opCtx);
        }
        if (facet.pipeline) {
            facet.pipeline->reattachToOperationContext(opCtx);
        }
    }
}

void DocumentSourceFacet::reattachSourceToOperationContext(OperationContext* opCtx) {
    for (auto&& facet : _facets) {
        if (facet.execPipeline) {
            facet.execPipeline->reattachToOperationContext(opCtx);
        }
        if (facet.pipeline) {
            facet.pipeline->reattachToOperationContext(opCtx);
        }
    }
}

bool DocumentSourceFacet::validateOperationContext(const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        std::all_of(_facets.begin(), _facets.end(), [opCtx](const auto& f) {
               return (!f.execPipeline || f.execPipeline->validateOperationContext(opCtx));
           });
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
            const auto hostReq = subConstraints.resolvedHostTypeRequirement(pExpCtx);

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
    if (!(host == HostTypeRequirement::kNone || host == HostTypeRequirement::kRunOnceAnyNode ||
          host == HostTypeRequirement::kAnyShard)) {
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

bool DocumentSourceFacet::usedDisk() const {
    return _done ? _stats.planSummaryStats.usedDisk
                 : std::any_of(_facets.begin(), _facets.end(), [](const auto& facet) {
                       return facet.execPipeline && facet.execPipeline->usedDisk();
                   });
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

    boost::optional<std::string> needsRouter;
    boost::optional<std::string> needsShard;

    std::vector<FacetPipeline> facetPipelines;
    for (auto&& rawFacet : extractRawPipelines(elem)) {
        const auto facetName = rawFacet.first;

        auto pipeline =
            Pipeline::parseFacetPipeline(rawFacet.second, expCtx, [](const Pipeline& pipeline) {
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
                    invariant(stageConstraints.requiredPosition ==
                              StageConstraints::PositionRequirement::kNone);
                    invariant(!stageConstraints.isIndependentOfAnyCollection);
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
