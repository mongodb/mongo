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

#include "mongo/db/pipeline/document_source_lookup.h"

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/sort_reorder_helpers.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>

namespace mongo {
namespace {

/**
 * Constructs a query of the following shape:
 *  {$or: [
 *    {'fieldName': {$eq: 'values[0]'}},
 *    {'fieldName': {$eq: 'values[1]'}},
 *    ...
 *  ]}
 */
BSONObj buildEqualityOrQuery(const std::string& fieldName, const BSONArray& values) {
    BSONObjBuilder orBuilder;
    {
        BSONArrayBuilder orPredicatesBuilder(orBuilder.subarrayStart("$or"));
        for (auto&& value : values) {
            orPredicatesBuilder.append(BSON(fieldName << BSON("$eq" << value)));
        }
    }
    return orBuilder.obj();
}

// Parses $lookup 'from' field. The 'from' field must be a string or one of the following
// exceptions:
// {from: {db: "config", coll: "cache.chunks.*"}, ...} or
// {from: {db: "local", coll: "oplog.rs"}, ...}
NamespaceString parseLookupFromAndResolveNamespace(const BSONElement& elem,
                                                   const DatabaseName& defaultDb,
                                                   bool allowGenericForeignDbLookup) {
    // The object syntax only works for 'cache.chunks.*', 'local.oplog.rs'
    //  which are not user namespaces so object type is
    // omitted from the error message below.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$lookup 'from' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::string || elem.type() == BSONType::object);

    if (elem.type() == BSONType::string) {
        return NamespaceStringUtil::deserialize(defaultDb, elem.valueStringData());
    }

    // Valdate the db and coll names.
    const auto tenantId = defaultDb.tenantId();
    const auto vts = tenantId
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    auto spec = NamespaceSpec::parse(
        IDLParserContext{
            elem.fieldNameStringData(), vts, tenantId, SerializationContext::stateDefault()},
        elem.embeddedObject());
    auto nss = NamespaceStringUtil::deserialize(spec.getDb().value_or(DatabaseName()),
                                                spec.getColl().value_or(""));
    // In the cases nss == config.collections and nss == config.chunks we can proceed with the
    // lookup as the merge will be done on the config server
    bool isConfigSvrSupportedCollection = nss == NamespaceString::kConfigsvrCollectionsNamespace ||
        nss == NamespaceString::kConfigsvrChunksNamespace;
    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << "$lookup with syntax {from: {db:<>, coll:<>},..} is not supported for db: "
                      << nss.dbName().toStringForErrorMsg() << " and coll: " << nss.coll(),
        nss.isConfigDotCacheDotChunks() || nss == NamespaceString::kRsOplogNamespace ||
            isConfigSvrSupportedCollection || allowGenericForeignDbLookup);
    return nss;
}

// Creates the conditions for joining the local and foreign fields inside of a $match.
static BSONObj createMatchStageJoinObj(const Document& input,
                                       const FieldPath& localFieldPath,
                                       const std::string& foreignFieldName) {
    // Add the 'localFieldPath' of 'input' into 'localFieldList'. If 'localFieldPath' references a
    // field with an array in its path, we may need to join on multiple values, so we add each
    // element to 'localFieldList'.
    BSONArrayBuilder arrBuilder;
    bool containsRegex = false;
    document_path_support::visitAllValuesAtPath(input, localFieldPath, [&](const Value& nextValue) {
        arrBuilder << nextValue;
        if (!containsRegex && nextValue.getType() == BSONType::regEx) {
            containsRegex = true;
        }
    });

    if (arrBuilder.arrSize() == 0) {
        // Missing values are treated as null.
        arrBuilder << BSONNULL;
    }

    // We construct a query of one of the following forms, depending on the contents of
    // 'localFieldList'.
    //
    //   {<foreignFieldName>: {$eq: <localFieldList[0]>}}
    //     if 'localFieldList' contains a single element.
    //
    //   {<foreignFieldName>: {$in: [<value>, <value>, ...]}}
    //     if 'localFieldList' contains more than one element but doesn't contain any that are
    //     regular expressions.
    //
    //   {$or: [{<foreignFieldName>: {$eq: <value>}}, {<foreignFieldName>: {$eq: <value>}}, ...]}
    //     if 'localFieldList' contains more than one element and it contains at least one element
    //     that is a regular expression.
    const auto localFieldListSize = arrBuilder.arrSize();
    const auto localFieldList = arrBuilder.arr();
    BSONObjBuilder joinObj;
    if (localFieldListSize > 1) {
        // A $lookup on an array value corresponds to finding documents in the foreign collection
        // that have a value of any of the elements in the array value, rather than finding
        // documents that have a value equal to the entire array value. These semantics are
        // automatically provided to us by using the $in query operator.
        if (containsRegex) {
            // A regular expression inside the $in query operator will perform pattern matching on
            // any string values. Since we want regular expressions to only match other RegEx types,
            // we write the query as a $or of equality comparisons instead.
            return buildEqualityOrQuery(foreignFieldName, localFieldList);
        } else {
            // { <foreignFieldName> : { "$in" : <localFieldList> } }
            BSONObjBuilder subObj(joinObj.subobjStart(foreignFieldName));
            subObj << "$in" << localFieldList;
            subObj.doneFast();
            return joinObj.obj();
        }
    }
    // Otherwise we have a simple $eq.
    // { <foreignFieldName> : { "$eq" : <localFieldList[0]> } }
    BSONObjBuilder subObj(joinObj.subobjStart(foreignFieldName));
    subObj << "$eq" << localFieldList[0];
    subObj.doneFast();
    return joinObj.obj();
}

}  // namespace

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      _sharedState(std::make_shared<LookUpSharedState>(
          std::move(fromNs), std::move(as), expCtx, foreignShardedLookupAllowed())) {
    if (!_sharedState->fromNs.isOnInternalDb()) {
        serviceOpCounters(expCtx->getOperationContext()).gotNestedAggregate();
    }

    const auto& resolvedNamespace = expCtx->getResolvedNamespace(_sharedState->fromNs);
    _resolvedNs = resolvedNamespace.ns;
    _sharedState->resolvedPipeline = resolvedNamespace.pipeline;
    _fromNsIsAView = resolvedNamespace.involvedNamespaceIsAView;

    _sharedState->fromExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, resolvedNamespace.ns, resolvedNamespace.uuid, boost::none, _sharedState->fromNs);
    _sharedState->fromExpCtx->setInLookup(true);
}

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           std::string localField,
                                           std::string foreignField,
                                           const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, expCtx) {
    _sharedState->localField = std::move(localField);
    _sharedState->foreignField = std::move(foreignField);

    // We append an additional BSONObj to '_sharedState->resolvedPipeline' as a placeholder
    // for the $match stage we'll eventually construct from the input document.
    _sharedState->resolvedPipeline.reserve(_sharedState->resolvedPipeline.size() + 1);

    // Initialize the introspection pipeline before we insert the $match. This is okay because we do
    // not use the introspection pipeline during/after query execution, which is when the $match is
    // necessary.
    initializeResolvedIntrospectionPipeline();

    _sharedState->resolvedPipeline.push_back(BSON("$match" << BSONObj()));
    _sharedState->fieldMatchPipelineIdx = _sharedState->resolvedPipeline.size() - 1;
}

std::vector<BSONObj> extractSourceStage(const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return {};
    }

    // When serializing $lookup to send the pipeline to a shard, we send the
    // '_sharedState->resolvedIntrospectionPipeline'.
    // '_sharedState->resolvedIntrospectionPipeline' is parsed and contains a desugared
    // version of $documents. Therefore, on a shard, we must check for a desugared $documents and
    // return those stages as the source stage.
    if (auto desugaredStages =
            DocumentSourceDocuments::extractDesugaredStagesFromPipeline(pipeline);
        desugaredStages.has_value()) {
        return desugaredStages.value();
    }
    // When we first create a $lookup stage, the input 'pipeline' is unparsed, so we
    // check for the $documents stage itself.
    if (pipeline[0].hasField(DocumentSourceDocuments::kStageName) ||
        pipeline[0].hasField("$search"_sd) ||
        pipeline[0].hasField(DocumentSourceQueue::kStageName)) {
        return {pipeline[0]};
    }
    return {};
}

// Process and copy the given `pipeline` to the `_sharedState->resolvedPipeline` attribute and
// compute where the $match stage is going to be placed, indicated through the
// `_sharedState->fieldMatchPipelineIdx` variable.
void DocumentSourceLookUp::resolvedPipelineHelper(
    NamespaceString fromNs,
    std::vector<BSONObj> pipeline,
    boost::optional<std::pair<std::string, std::string>> localForeignFields,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // When fromNs represents a view, we have to decipher if the view is mongot-indexed or not.
    // Currently, if the pipeline to be run on the joined collection is a
    // mongot pipeline (it starts with $search, $searchMeta), $lookup assumes the view is
    // mongot-indexed.
    if (_fromNsIsAView && search_helper_bson_obj::isMongotPipeline(pipeline)) {
        // The user pipeline is a mongot pipeline so we assume the view is a mongot-indexed view. As
        // such, we overwrite the view pipeline. This is because in the case of mongot queries on
        // mongot-indexed views, idLookup applies the view transforms as part of its subpipeline.
        _sharedState->fromExpCtx->setView(
            boost::make_optional(std::make_pair(fromNs, _sharedState->resolvedPipeline)));
        _sharedState->resolvedPipeline = pipeline;
        _sharedState->fieldMatchPipelineIdx = 1;
        if (localForeignFields != boost::none) {
            std::tie(_sharedState->localField, _sharedState->foreignField) = *localForeignFields;
        } else {
            _sharedState->cache.emplace(
                loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes));
        }
        return;
    }

    if (localForeignFields != boost::none) {
        std::tie(_sharedState->localField, _sharedState->foreignField) = *localForeignFields;

        // The $match stage should come after $documents if present.
        auto sourceStages = extractSourceStage(pipeline);
        _sharedState->resolvedPipeline.insert(
            _sharedState->resolvedPipeline.end(), sourceStages.begin(), sourceStages.end());

        // Save the correct position of the $match, but wait to insert it until we have finished
        // constructing the pipeline and created the introspection pipeline below.
        _sharedState->fieldMatchPipelineIdx = _sharedState->resolvedPipeline.size();

        // Add the rest of the user pipeline to `_sharedState->resolvedPipeline` after any
        // potential view prefix and $match.
        _sharedState->resolvedPipeline.insert(_sharedState->resolvedPipeline.end(),
                                              pipeline.begin() + sourceStages.size(),
                                              pipeline.end());

    } else {
        // When local/foreignFields are included, we cannot enable the cache because the $match
        // is a correlated prefix that will not be detected. Here, local/foreignFields are absent,
        // so we enable the cache.
        _sharedState->cache.emplace(
            loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes));

        // Add the user pipeline to '_sharedState->resolvedPipeline' after any potential view
        // prefix and $match
        _sharedState->resolvedPipeline.insert(
            _sharedState->resolvedPipeline.end(), pipeline.begin(), pipeline.end());
    }
}
DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    std::vector<BSONObj> pipeline,
    BSONObj letVariables,
    boost::optional<std::pair<std::string, std::string>> localForeignFields,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, expCtx) {
    // '_sharedState->resolvedPipeline' will first be initialized by the constructor delegated
    // to within this constructor's initializer list. It will be populated with view pipeline prefix
    // if 'fromNs' represents a view. We will then append stages to ensure any view prefix is not
    // overwritten.
    resolvedPipelineHelper(fromNs, pipeline, localForeignFields, expCtx);

    _sharedState->userPipeline = std::move(pipeline);

    for (auto&& varElem : letVariables) {
        const auto varName = varElem.fieldNameStringData();
        variableValidation::validateNameForUserWrite(varName);

        _sharedState->letVariables.emplace_back(
            std::string{varName},
            Expression::parseOperand(expCtx.get(), varElem, expCtx->variablesParseState),
            _sharedState->variablesParseState.defineVariable(varName));
    }

    // Initialize the introspection pipeline before we insert the $match (if applicable). This is
    // okay because we only use the introspection pipeline for reference while doing query analysis
    // and analyzing involved dependencies/variables/collections/constraints. We do not use the
    // introspection pipeline during/after query execution, which is when the $match is necessary.
    // It wouldn't hurt anything to include the $match in this pipeline, but we also use the
    // introspection pipeline in serialization, so it would be a bit odd to include an extra empty
    // $match.
    initializeResolvedIntrospectionPipeline();

    // Finally, insert the $match placeholder if we need it.
    if (_sharedState->fieldMatchPipelineIdx) {
        _sharedState->resolvedPipeline.insert(_sharedState->resolvedPipeline.begin() +
                                                  *_sharedState->fieldMatchPipelineIdx,
                                              BSON("$match" << BSONObj()));
    }
}

DocumentSourceLookUp::DocumentSourceLookUp(const DocumentSourceLookUp& original,
                                           const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(kStageName, newExpCtx),
      _resolvedNs(original._resolvedNs),
      _sharedState(std::make_shared<LookUpSharedState>(
          original._sharedState->fromNs,
          original._sharedState->as,
          original._sharedState->additionalFilter,
          original._sharedState->localField,
          original._sharedState->foreignField,
          original._sharedState->fieldMatchPipelineIdx,
          original._sharedState->variables,
          original._sharedState->variablesParseState.copyWith(
              original._sharedState->variables.useIdGenerator()),
          makeCopyFromExpressionContext(original._sharedState->fromExpCtx,
                                        _resolvedNs,
                                        original._sharedState->fromExpCtx->getUUID(),
                                        boost::none,
                                        original._sharedState->fromExpCtx->getView()),
          original._sharedState->resolvedPipeline,
          original._sharedState->userPipeline,
          original._sharedState->letVariables,
          foreignShardedLookupAllowed())) {
    _sharedState->resolvedIntrospectionPipeline =
        original._sharedState->resolvedIntrospectionPipeline->clone(_sharedState->fromExpCtx);
    if (!_sharedState->localField && !_sharedState->foreignField) {
        _sharedState->cache.emplace(internalDocumentSourceCursorBatchSizeBytes.load());
    }
    if (original._sharedState->matchSrc) {
        _sharedState->matchSrc = static_cast<DocumentSourceMatch*>(
            original._sharedState->matchSrc->clone(getExpCtx()).get());
    }
    if (original._sharedState->unwindSrc) {
        _sharedState->unwindSrc = static_cast<DocumentSourceUnwind*>(
            original._sharedState->unwindSrc->clone(getExpCtx()).get());
    }
}

boost::intrusive_ptr<DocumentSource> DocumentSourceLookUp::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    return make_intrusive<DocumentSourceLookUp>(*this, newExpCtx);
}

void validateLookupCollectionlessPipeline(const std::vector<BSONObj>& pipeline) {
    uassert(ErrorCodes::FailedToParse,
            "$lookup stage without explicit collection must have a pipeline with $documents as "
            "first stage",
            pipeline.size() > 0 &&
                !pipeline[0].getField(DocumentSourceDocuments::kStageName).eoo());
}

void validateLookupCollectionlessPipeline(const BSONElement& pipeline) {
    uassert(ErrorCodes::FailedToParse, "must specify 'pipeline' when 'from' is empty", pipeline);
    auto parsedPipeline = parsePipelineFromBSON(pipeline);
    validateLookupCollectionlessPipeline(parsedPipeline);
}

std::unique_ptr<DocumentSourceLookUp::LiteParsed> DocumentSourceLookUp::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $lookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    auto pipelineElem = specObj["pipeline"];
    NamespaceString fromNss;
    if (!fromElement) {
        validateLookupCollectionlessPipeline(pipelineElem);
        fromNss = NamespaceString::makeCollectionlessAggregateNSS(nss.dbName());
    } else {
        fromNss = parseLookupFromAndResolveNamespace(
            fromElement, nss.dbName(), options.allowGenericForeignDbLookup);
    }
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $lookup namespace: " << fromNss.toStringForErrorMsg(),
            fromNss.isValid());

    // Recursively lite parse the nested pipeline, if one exists.
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (pipelineElem) {
        auto pipeline = parsePipelineFromBSON(pipelineElem);
        liteParsedPipeline = LiteParsedPipeline(fromNss, pipeline);
    }

    return std::make_unique<DocumentSourceLookUp::LiteParsed>(
        spec.fieldName(), std::move(fromNss), std::move(liteParsedPipeline));
}

PrivilegeVector DocumentSourceLookUp::LiteParsed::requiredPrivileges(
    bool isMongos, bool bypassDocumentValidation) const {
    PrivilegeVector requiredPrivileges;
    invariant(_pipelines.size() <= 1);
    invariant(_foreignNss);

    // If no pipeline is specified or the local/foreignField syntax was used, then assume that we're
    // reading directly from the collection.
    if (_pipelines.empty() || !_pipelines[0].startsWithInitialSource()) {
        Privilege::addPrivilegeToPrivilegeVector(
            &requiredPrivileges,
            Privilege(ResourcePattern::forExactNamespace(*_foreignNss), ActionType::find));
    }

    // Add the sub-pipeline privileges, if one was specified.
    if (!_pipelines.empty()) {
        const LiteParsedPipeline& pipeline = _pipelines[0];
        Privilege::addPrivilegesToPrivilegeVector(
            &requiredPrivileges, pipeline.requiredPrivileges(isMongos, bypassDocumentValidation));
    }

    return requiredPrivileges;
}

REGISTER_DOCUMENT_SOURCE(lookup,
                         DocumentSourceLookUp::LiteParsed::parse,
                         DocumentSourceLookUp::createFromBson,
                         AllowedWithApiStrict::kConditionally);
ALLOCATE_DOCUMENT_SOURCE_ID(lookup, DocumentSourceLookUp::id)

const char* DocumentSourceLookUp::getSourceName() const {
    return kStageName.data();
}

bool DocumentSourceLookUp::foreignShardedLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !getExpCtx()->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

void DocumentSourceLookUp::determineSbeCompatibility() {
    _sbeCompatibility = getExpCtx()->getSbeCompatibility();
    // This stage has the SBE compatibility as least the same as that of the expression context.
    auto sbeCompatibleByStageConfig =
        // We currently only support lowering equi-join that uses localField/foreignField
        // syntax.
        !_sharedState->userPipeline && _sharedState->localField &&
        _sharedState->foreignField
        // SBE doesn't support match-like paths with numeric components. (Note: "as" field is a
        // project-like field and numbers in it are treated as literal names of fields rather
        // than indexes into arrays, which is compatible with SBE.)
        && !FieldRef(_sharedState->localField->fullPath()).hasNumericPathComponents() &&
        !FieldRef(_sharedState->foreignField->fullPath()).hasNumericPathComponents()
        // We currently don't lower $lookup against views ('_sharedState->fromNs' does not
        // correspond to a view).
        && getExpCtx()->getResolvedNamespace(_sharedState->fromNs).pipeline.empty();
    if (!sbeCompatibleByStageConfig) {
        _sbeCompatibility = SbeCompatibility::notCompatible;
    }
}

StageConstraints DocumentSourceLookUp::constraints(PipelineSplitState pipeState) const {
    HostTypeRequirement hostRequirement;
    bool nominateMergingShard = false;
    if (_sharedState->fromNs.isConfigDotCacheDotChunks()) {
        // $lookup from config.cache.chunks* namespaces is permitted to run on each individual
        // shard, rather than just a merging shard, since each shard should have an identical copy
        // of the namespace.
        hostRequirement = HostTypeRequirement::kAnyShard;
    } else if (pipeState == PipelineSplitState::kSplitForShards) {
        // This stage will only be on the shards pipeline if $lookup on sharded foreign collections
        // is allowed.
        hostRequirement = HostTypeRequirement::kAnyShard;
    } else if (_sharedState->fromNs.isCollectionlessAggregateNS()) {
        // When the inner pipeline does not target a collection, it can run on any node.
        hostRequirement = HostTypeRequirement::kRunOnceAnyNode;
    } else {
        // If the pipeline is unsplit, then this $lookup can run anywhere.
        hostRequirement = HostTypeRequirement::kNone;
        nominateMergingShard = pipeState == PipelineSplitState::kSplitForMerge;
    }

    // By default, $lookup is allowed in a transaction and does not use disk.
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 hostRequirement,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kAllowed,
                                 TransactionRequirement::kAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed);

    // However, if $lookup is specified with a pipeline, it inherits the strictest disk use, facet,
    // transaction, and lookup requirements from the children in its pipeline.
    if (hasPipeline()) {
        constraints = StageConstraints::getStrictestConstraints(
            _sharedState->resolvedIntrospectionPipeline->getSources(), constraints);
    }

    if (nominateMergingShard) {
        constraints.mergeShardId = getMergeShardId();
    }

    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = !_sharedState->unwindSrc;

    return constraints;
}

boost::optional<ShardId> DocumentSourceLookUp::computeMergeShardId() const {
    // If this $lookup is on the merging half of the pipeline and the inner collection isn't
    // sharded (that is, it is either unsplittable or untracked), then we should merge on the shard
    // which owns the inner collection.
    if (auto msi = getExpCtx()->getMongoProcessInterface()->determineSpecificMergeShard(
            getExpCtx()->getOperationContext(), _sharedState->fromNs)) {
        return msi;
    }

    // If we have not yet designated a merging shard and are executing on mongod, designate the
    // current shard as the merging shard. This is done to prevent pushing this $lookup to the
    // shards part of the pipeline. This is an important optimization designating as this $lookup as
    // a merging stage allows us to execute a single $lookup (as opposed to executing one $lookup on
    // each involved shard). When this stage is part of a deeply nested pipeline, it prevents
    // creating an exponential explosion of cursors/resources (proportional to the level of pipeline
    // nesting). If we are in a replica set, though, then we do not have an initialized sharding
    // state nor a valid shard Id. Note that the sharding state being disabled may mean we are on a
    // secondary of a shard server node that hasn't yet initialized its sharding state. Since this
    // choice is only for performance, that is acceptable.
    const auto shardingState = ShardingState::get(getExpCtx()->getOperationContext());
    if (!getExpCtx()->getInRouter() && shardingState->enabled()) {
        return shardingState->shardId();
    }

    return boost::none;
}

DocumentSource::GetModPathsReturn DocumentSourceLookUp::getModifiedPaths() const {
    OrderedPathSet modifiedPaths{_sharedState->as.fullPath()};
    if (_sharedState->unwindSrc) {
        auto pathsModifiedByUnwind = _sharedState->unwindSrc->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

DocumentSourceContainer::iterator DocumentSourceLookUp::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If the following stage is $sort and this $lookup has not absorbed a following $unwind, try to
    // move the $sort ahead of the $lookup.
    if (!_sharedState->unwindSrc) {
        itr = tryReorderingWithSort(itr, container);
        if (*itr != this) {
            return itr;
        }
    }

    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());

    // If we are not already handling an $unwind stage internally and the following stage is an
    // $unwind of the $lookup "as" output array, subsume the $unwind into the current $lookup as an
    // $LU ($lookup + $unwind) macro stage. The combined stage acts like a SQL join (one result
    // record per LHS x RHS match instead of one result per LHS with an array of its RHS matches).
    //
    // Ideally for simplicity in the stage builder we would not absorb the downstream $unwind if the
    // lookup strategy is kNonExistentForeignCollection, but that is not determined until later and
    // would be hard to do so here as it requires several inputs we do not have. It is also hard to
    // move that determination earlier as it occurs in the deep stack under createLegacyExecutor().
    if (nextUnwind && !_sharedState->unwindSrc &&
        nextUnwind->getUnwindPath() == _sharedState->as.fullPath()) {
        if (nextUnwind->preserveNullAndEmptyArrays() || nextUnwind->indexPath()) {
            downgradeSbeCompatibility(SbeCompatibility::notCompatible);
        } else {
            downgradeSbeCompatibility(SbeCompatibility::requiresTrySbe);
        }
        _sharedState->unwindSrc = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }

    // Attempt to internalize any predicates of a $match upon the "_sharedState->as" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    if (!_sharedState->unwindSrc || _sharedState->unwindSrc->indexPath() ||
        _sharedState->unwindSrc->preserveNullAndEmptyArrays()) {
        // We must be unwinding our result to internalize a $match. For example, consider the
        // following pipeline:
        //
        // Input: {_id: 0}
        // Foreign Collection: {a: 0, b: 0}, {a: 0, b: 5}
        // Pipeline:
        //   {$lookup: {localField: "_id", foreignField: "a", as: "foo"}}
        //   {$match: {'foo.b': {$gt: 0}}}
        // Output: {_id: 0, foo: [{a: 0, b: 0}, {a: 0, b: 5}]}
        //
        // If we executed {b: {$gt: 0}} as part of our $lookup, our output would instead be:
        // {_id: 0, foo: [{a: 0, b: 5}]}
        //
        // However, if we are already unwinding 'foo', then we can move the $match inside, since it
        // will have the same effect as filtering the unwound results, that is, the output will be:
        // {_id: 0, foo: {a: 0, b: 5}}
        //
        // Note that we cannot absorb a $match if the absorbed $unwind has
        // "preserveNullAndEmptyArrays" set to true, for the following reason: A document that had
        // an empty output array from $lookup would be preserved by the $unwind, but could be
        // removed by the $match. However, if we absorb the $match into the $lookup, our joined
        // query inside the $lookup will output an empty array, which $unwind will then preserve.
        // Thus, depending on the optimization, the user would see a different output.
        //
        // In addition, we must avoid internalizing a $match if an absorbed $unwind has an
        // "includeArrayIndex" option, since the $match will alter the indices of the returned
        // values.
        return std::next(itr);
    }

    // We cannot internalize a $match if a collation has been set on the $lookup stage and it
    // differs from that of the parent pipeline.
    if (_sharedState->fromExpCtx->getCollator() &&
        !CollatorInterface::collatorsMatch(getExpCtx()->getCollator(),
                                           _sharedState->fromExpCtx->getCollator())) {
        return std::next(itr);
    }

    auto outputPath = _sharedState->as.fullPath();

    // Since $match splitting is handled in a generic way, we expect to have already swapped
    // portions of the $match that do not depend on the 'as' path or on an internalized $unwind's
    // index path before ourselves. But due to the early return above, we know there is no
    // internalized $unwind with an index path.
    //
    // Therefore, 'nextMatch' should only depend on the 'as' path. We now try to absorb the match on
    // the 'as' path in order to push down these predicates into the foreign collection.
    bool isMatchOnlyOnAs = true;
    auto computeWhetherMatchOnAs = [&isMatchOnlyOnAs, &outputPath](MatchExpression* expression,
                                                                   std::string path) -> void {
        // There are certain situations where this rewrite would not be correct. For example,
        // if 'expression' is the child of a value $elemMatch, we cannot internalize the $match.
        // Consider {b: {$elemMatch: {$gt: 1, $lt: 4}}}, where "b" is our "_sharedState->as"
        // field. This rewrite is not supported because there's no way to modify the expression to
        // be a match just on 'b'--we cannot change the path to an empty string, or remove the node
        // entirely. For other internal nodes with paths, we don't support the rewrite to keep the
        // descendMatchOnPath implementation simple.
        if (MatchExpression::isInternalNodeWithPath(expression->matchType())) {
            isMatchOnlyOnAs = false;
        }
        if (expression->numChildren() == 0) {
            // 'expression' is a leaf node; examine the path. It is important that 'outputPath'
            // not equal 'path', because we cannot change the expression {b: {$eq: 3}}, where
            // 'path' is 'b', to be a match on a subfield, since no subfield exists.
            isMatchOnlyOnAs = isMatchOnlyOnAs && expression::isPathPrefixOf(outputPath, path);
        }
    };

    expression::mapOver(nextMatch->getMatchExpression(), computeWhetherMatchOnAs);

    if (!isMatchOnlyOnAs) {
        // "nextMatch" does not contain any predicates that can be absorbed into this stage.
        return std::next(itr);
    }

    // We cannot yet lower $LUM (combined $lookup + $unwind + $match) stages to SBE.
    _sbeCompatibility = SbeCompatibility::notCompatible;
    bool needToOptimize = false;
    if (!_sharedState->matchSrc) {
        _sharedState->matchSrc = nextMatch;
    } else {
        // We have already absorbed a $match. We need to join it with the next one.
        _sharedState->matchSrc->joinMatchWith(nextMatch, MatchExpression::MatchType::AND);
        needToOptimize = true;
    }

    // Remove the original $match.
    container->erase(std::next(itr));

    // We have internalized a $match, but have not yet computed the descended $match that should
    // be applied to our queries. Note that we have to optimze the MatchExpression that we pass into
    // 'descendMatchOnPath' because the call to 'joinMatchWith' rebuilds the new $match stage using
    // each stage's unoptimized BSON predicate. The unoptimized BSON may contain predicates that
    // were optimized away, so that the checks performed by 'computeWhetherMatchOnlyOnAs' may no
    // longer be true for the combined $match's MatchExpression.
    _sharedState->additionalFilter =
        DocumentSourceMatch::descendMatchOnPath(
            needToOptimize
                ? optimizeMatchExpression(
                      std::move(_sharedState->matchSrc->getMatchProcessor()->getExpression()),
                      /* enableSimplification */ false)
                      .get()
                : _sharedState->matchSrc->getMatchExpression(),
            _sharedState->as.fullPath(),
            getExpCtx())
            ->getQuery()
            .getOwned();

    // Add '_sharedState->additionalFilter' to '_sharedState->resolvedPipeline' if there
    // is a pipeline. If there is no pipeline, '_sharedState->additionalFilter' can safely be
    // added to the local/foreignField $match stage during 'doGetNext()'.
    if (hasPipeline()) {
        auto matchObj = BSON("$match" << *_sharedState->additionalFilter);
        _sharedState->resolvedPipeline.push_back(matchObj);
    }

    // There may be further optimization between this $lookup and the new neighbor, so we return an
    // iterator pointing to ourself.
    return itr;
}  // doOptimizeAt

BSONObj DocumentSourceLookUp::makeMatchStageFromInput(const Document& input,
                                                      const FieldPath& localFieldPath,
                                                      const std::string& foreignFieldName,
                                                      const BSONObj& additionalFilter) {
    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.
    BSONObjBuilder match;

    BSONObj joinObj = createMatchStageJoinObj(input, localFieldPath, foreignFieldName);

    // If we have one condition, do not place inside a $and. This BSON could be created many times,
    // so we want to produce simple queries for the planner if possible.
    if (additionalFilter.isEmpty()) {
        match << "$match" << joinObj;
    } else {
        BSONObjBuilder query(match.subobjStart("$match"));
        BSONArrayBuilder andObj(query.subarrayStart("$and"));
        andObj.append(joinObj);
        andObj.append(additionalFilter);
        andObj.doneFast();
        query.doneFast();
    }

    return match.obj();
}

void DocumentSourceLookUp::initializeResolvedIntrospectionPipeline() {
    _sharedState->variables.copyToExpCtx(_sharedState->variablesParseState,
                                         _sharedState->fromExpCtx.get());
    _sharedState->fromExpCtx->startExpressionCounters();
    _sharedState->resolvedIntrospectionPipeline = Pipeline::parse(
        _sharedState->resolvedPipeline, _sharedState->fromExpCtx, mongo::lookupPipeValidator);
    _sharedState->fromExpCtx->stopExpressionCounters();
}

void DocumentSourceLookUp::appendSpecificExecStats(MutableDocument& doc) const {
    const PlanSummaryStats& stats = _sharedState->stats.planSummaryStats;
    doc["totalDocsExamined"] = Value(static_cast<long long>(stats.totalDocsExamined));
    doc["totalKeysExamined"] = Value(static_cast<long long>(stats.totalKeysExamined));
    doc["collectionScans"] = Value(stats.collectionScans);
    std::vector<Value> indexesUsedVec;
    std::transform(stats.indexesUsed.begin(),
                   stats.indexesUsed.end(),
                   std::back_inserter(indexesUsedVec),
                   [](const std::string& idx) -> Value { return Value(idx); });
    doc["indexesUsed"] = Value{std::move(indexesUsedVec)};
}

void DocumentSourceLookUp::serializeToArray(std::vector<Value>& array,
                                            const SerializationOptions& opts) const {
    // Support alternative $lookup from config.cache.chunks* namespaces.
    //
    // Do not include the tenantId in serialized 'from' namespace.
    auto fromValue = getExpCtx()->getNamespaceString().isEqualDb(_sharedState->fromNs)
        ? Value(opts.serializeIdentifier(_sharedState->fromNs.coll()))
        : Value(Document{{"db",
                          opts.serializeIdentifier(
                              _sharedState->fromNs.dbName().serializeWithoutTenantPrefix_UNSAFE())},
                         {"coll", opts.serializeIdentifier(_sharedState->fromNs.coll())}});

    MutableDocument output(Document{
        {getSourceName(),
         Document{{"from", fromValue}, {"as", opts.serializeFieldPath(_sharedState->as)}}}});

    if (hasLocalFieldForeignFieldJoin()) {
        output[getSourceName()]["localField"] =
            Value(opts.serializeFieldPath(_sharedState->localField.value()));
        output[getSourceName()]["foreignField"] =
            Value(opts.serializeFieldPath(_sharedState->foreignField.value()));
    }

    // Add a pipeline field if only-pipeline syntax was used (to ensure the output is valid $lookup
    // syntax) or if a $match was absorbed.
    auto serializedPipeline = [&]() -> std::vector<BSONObj> {
        if (!_sharedState->userPipeline) {
            return std::vector<BSONObj>{};
        }
        if (opts.isSerializingForQueryStats()) {
            // TODO SERVER-94227 we don't need to do any validation as part of this parsing pass.
            return Pipeline::parse(*_sharedState->userPipeline, _sharedState->fromExpCtx)
                ->serializeToBson(opts);
        }
        if (opts.isSerializingForExplain()) {
            // TODO SERVER-81802 We should also serialize the resolved pipeline for explain.
            return *_sharedState->userPipeline;
        }
        if (opts.serializeForFLE2) {
            // This is a workaround for testing server rewrites for FLE2. We need to verify that the
            // _sharedState->resolvedPipeline was rewritten, since the
            // _sharedState->resolvedPipeline is used to execute the query.
            auto resolvedPipelineWithoutIndexMatchPlaceholder = _sharedState->resolvedPipeline;

            /**
             * We serialize for FLE2 in two cases:
             * 1) In rebuildResolvedPipeline:
             *    This method is called during FLE2 the server rewrite step for FLE2. It relies on
             *    serializing the rewritten _sharedState->resolvedIntrospectionPipeline to
             * regenerate the _sharedState->resolvedPipeline. In this step, we add the
             * _sharedState->fieldMatchPipelineIdx placeholder to the
             * _sharedState->resolvedPipeline after it has been serialized. 2) From
             * query_rewriter_test.cpp, where we would like to verify the
             * _sharedState->resolvedPipeline was succesfully rewritten.
             *
             * In both of these cases, we would like to serialize the
             * _sharedState->resolvedPipeline, since doGetNext() uses the resolved pipeline
             * for the query execution. Using the resolved pipeline also ensures we serialize nested
             * pipelines (i.e from nested lookups) in their FLE2 rewritten form as well. However, in
             * both of these cases we don't want the index match placeholder. We don't want the
             * empty $match stage, because when $lookup's pipeline is parsed, the match stage is
             * added in the DocumentSourceLookUp constructor, leading to a duplicate empty match
             * stage.
             */
            if (_sharedState->fieldMatchPipelineIdx) {
                resolvedPipelineWithoutIndexMatchPlaceholder.erase(
                    resolvedPipelineWithoutIndexMatchPlaceholder.begin() +
                    *_sharedState->fieldMatchPipelineIdx);
            }
            return resolvedPipelineWithoutIndexMatchPlaceholder;
        }
        return _sharedState->resolvedIntrospectionPipeline->serializeToBson(opts);
    }();
    if (_sharedState->additionalFilter) {
        auto serializedFilter = [&]() -> BSONObj {
            if (opts.isSerializingForQueryStats()) {
                auto filter = uassertStatusOK(
                    MatchExpressionParser::parse(*_sharedState->additionalFilter, getExpCtx()));
                return filter->serialize(opts);
            }
            return *_sharedState->additionalFilter;
        }();
        serializedPipeline.emplace_back(BSON("$match" << serializedFilter));
    }
    if (!hasLocalFieldForeignFieldJoin() || serializedPipeline.size() > 0) {
        MutableDocument exprList;
        for (const auto& letVar : _sharedState->letVariables) {
            exprList.addField(opts.serializeFieldPathFromString(letVar.name),
                              letVar.expression->serialize(opts));
        }
        output[getSourceName()]["let"] = Value(exprList.freeze());

        output[getSourceName()]["pipeline"] = Value(serializedPipeline);

        if (!opts.isSerializingForExplain() &&
            hybrid_scoring_util::isHybridSearchPipeline(
                _sharedState->userPipeline.value_or(std::vector<BSONObj>()))) {
            output[getSourceName()][hybrid_scoring_util::kIsHybridSearchFlagFieldName] =
                Value(true);
        }
    }

    if (opts.isSerializingForExplain()) {
        if (_sharedState->unwindSrc) {
            const boost::optional<FieldPath> indexPath = _sharedState->unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] = Value(
                DOC("preserveNullAndEmptyArrays"
                    << _sharedState->unwindSrc->preserveNullAndEmptyArrays() << "includeArrayIndex"
                    << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        if (opts.verbosity.value() >= ExplainOptions::Verbosity::kExecStats) {
            appendSpecificExecStats(output);
        }

        array.push_back(output.freezeToValue());
    } else if (opts.serializeForCloning && _sharedState->unwindSrc) {
        output[getSourceName()]["$_internalUnwind"] = _sharedState->unwindSrc->serialize(opts);
        array.push_back(output.freezeToValue());
    } else {
        array.push_back(output.freezeToValue());

        if (_sharedState->unwindSrc) {
            _sharedState->unwindSrc->serializeToArray(array);
        }
    }
}

DepsTracker::State DocumentSourceLookUp::getDependencies(DepsTracker* deps) const {
    if (hasPipeline() || _sharedState->letVariables.size() > 0) {
        // We will use the introspection pipeline which we prebuilt during construction.
        invariant(_sharedState->resolvedIntrospectionPipeline);

        DepsTracker subDeps;

        // Get the subpipeline dependencies. Subpipeline stages may reference both 'let' variables
        // declared by this $lookup and variables declared externally.
        for (auto&& source : _sharedState->resolvedIntrospectionPipeline->getSources()) {
            source->getDependencies(&subDeps);
        }

        // Add the 'let' dependencies to the tracker.
        for (auto&& letVar : _sharedState->letVariables) {
            expression::addDependencies(letVar.expression.get(), deps);
        }
    }

    if (hasLocalFieldForeignFieldJoin()) {
        const FieldRef ref(_sharedState->localField->fullPath());
        // We need everything up until the first numeric component. Otherwise, a projection could
        // treat the numeric component as a field name rather than an index into an array.
        size_t firstNumericIx;
        for (firstNumericIx = 0; firstNumericIx < ref.numParts(); firstNumericIx++) {
            // We are lenient with the component, because classic $lookup treats 0-prefixed numeric
            // fields like "00" as both an index and a field name. Allowing it in a dependency would
            // restrict the usage to only a field name.
            if (ref.isNumericPathComponentLenient(firstNumericIx)) {
                break;
            }
        }
        deps->fields.insert(std::string{ref.dottedSubstring(0, firstNumericIx)});
    }

    // Purposely ignore '_sharedState->matchSrc' and '_sharedState->unwindSrc', since
    // those should only be absorbed if we know they are only operating on the "as" field which will
    // be generated by this stage.

    return DepsTracker::State::SEE_NEXT;
}

void DocumentSourceLookUp::addVariableRefs(std::set<Variables::Id>* refs) const {
    // Do not add SEARCH_META as a reference, since it is scoped to one pipeline.
    if (hasPipeline()) {
        std::set<Variables::Id> subPipeRefs;
        _sharedState->resolvedIntrospectionPipeline->addVariableRefs(&subPipeRefs);
        for (auto&& varId : subPipeRefs) {
            if (varId != Variables::kSearchMetaId)
                refs->insert(varId);
        }
    }

    // Add the 'let' variable references. Because the caller is only interested in references to
    // external variables, filter out any subpipeline references to 'let' variables declared by this
    // $lookup. This step must happen after gathering the sub-pipeline variable references as they
    // may refer to let variables.
    for (auto&& letVar : _sharedState->letVariables) {
        expression::addVariableRefs(letVar.expression.get(), refs);
        refs->erase(letVar.id);
    }
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceLookUp::distributedPlanLogic() {
    // If $lookup into a sharded foreign collection is allowed and the foreign namespace is sharded,
    // top-level $lookup stages can run in parallel on the shards.
    //
    // Note that this decision is inherently racy and subject to become stale. This is okay because
    // either choice will work correctly; we are simply applying a heuristic optimization.
    if (_sharedState->isForeignShardedLookupAllowed && getExpCtx()->getSubPipelineDepth() == 0 &&
        !_sharedState->fromNs.isCollectionlessAggregateNS() &&
        getExpCtx()->getMongoProcessInterface()->isSharded(
            _sharedState->fromExpCtx->getOperationContext(), _sharedState->fromNs)) {
        tassert(
            8725000,
            "Should not attempt to nominate merging shard when $lookup is not acting as a merger",
            !mergeShardId.isInitialized() ||
                (mergeShardId.isInitialized() && getMergeShardId() == boost::none));
        return boost::none;
    }

    if (_sharedState->fromExpCtx->getNamespaceString().isConfigDotCacheDotChunks()) {
        // When $lookup reads from config.cache.chunks.* namespaces, it should run on each
        // individual shard in parallel. This is a special case, and atypical for standard $lookup
        // since a full copy of config.cache.chunks.* collections exists on all shards.
        tassert(
            8725001,
            "Should not attempt to nominate merging shard when $lookup is not acting as a merger",
            !mergeShardId.isInitialized() ||
                (mergeShardId.isInitialized() && getMergeShardId() == boost::none));
        return boost::none;
    }

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{nullptr, this, boost::none};
}

void DocumentSourceLookUp::detachSourceFromOperationContext() {
    if (_sharedState->pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::detachFromOperationContext() to take care of updating
        // '_sharedState->fromExpCtx->getOperationContext()'.
        tassert(10713706,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' "
                "is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->detachFromOperationContext();
        _sharedState->pipeline->detachFromOperationContext();
        tassert(10713707,
                "expecting _sharedState->fromExpCtx->getOperationContext() == nullptr",
                _sharedState->fromExpCtx->getOperationContext() == nullptr);
    }
    if (_sharedState->fromExpCtx) {
        _sharedState->fromExpCtx->setOperationContext(nullptr);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->detachFromOperationContext();
    }
}

void DocumentSourceLookUp::reattachSourceToOperationContext(OperationContext* opCtx) {
    if (_sharedState->pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating
        // '_sharedState->fromExpCtx->getOperationContext()'.
        tassert(10713710,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' "
                "is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->reattachToOperationContext(opCtx);
        _sharedState->pipeline->reattachToOperationContext(opCtx);
        tassert(10713711,
                "expecting _sharedState->fromExpCtx->getOperationContext() == opCtx",
                _sharedState->fromExpCtx->getOperationContext() == opCtx);
    }
    if (_sharedState->fromExpCtx) {
        _sharedState->fromExpCtx->setOperationContext(opCtx);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceLookUp::validateSourceOperationContext(const OperationContext* opCtx) const {
    if (getExpCtx()->getOperationContext() != opCtx ||
        (_sharedState->fromExpCtx && _sharedState->fromExpCtx->getOperationContext() != opCtx)) {
        return false;
    }
    if (_sharedState->execPipeline &&
        !_sharedState->execPipeline->validateOperationContext(opCtx)) {
        return false;
    }
    if (_sharedState->resolvedIntrospectionPipeline &&
        !_sharedState->resolvedIntrospectionPipeline->validateOperationContext(opCtx)) {
        return false;
    }

    return true;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            "the $lookup specification must be an Object",
            elem.type() == BSONType::object);

    NamespaceString fromNs;
    std::string as;

    std::string localField;
    std::string foreignField;

    BSONObj letVariables;
    BSONObj unwindSpec;
    std::vector<BSONObj> pipeline;
    bool hasPipeline = false;
    bool hasLet = false;

    // TODO SERVER-108117 Validate that the isHybridSearch flag is only set internally. See helper
    // hybrid_scoring_util::validateIsHybridSearchNotSetByUser to handle this.

    auto lookupSpec = DocumentSourceLookupSpec::parse(IDLParserContext(kStageName), elem.Obj());

    if (lookupSpec.getFrom().has_value()) {
        fromNs = parseLookupFromAndResolveNamespace(lookupSpec.getFrom().value().getElement(),
                                                    pExpCtx->getNamespaceString().dbName(),
                                                    pExpCtx->getAllowGenericForeignDbLookup());
    }

    as = std::string{lookupSpec.getAs()};

    if (lookupSpec.getPipeline().has_value()) {
        hasPipeline = true;
        pipeline = lookupSpec.getPipeline().value();
    }
    if (lookupSpec.getLetVars().has_value()) {
        hasLet = true;
        letVariables = lookupSpec.getLetVars().value();
    }
    localField = std::string{lookupSpec.getLocalField().value_or("")};
    foreignField = std::string{lookupSpec.getForeignField().value_or("")};
    if (lookupSpec.getUnwindSpec().has_value()) {
        unwindSpec = lookupSpec.getUnwindSpec().value();
    }

    if (fromNs.isEmpty()) {
        validateLookupCollectionlessPipeline(pipeline);
        fromNs =
            NamespaceString::makeCollectionlessAggregateNSS(pExpCtx->getNamespaceString().dbName());
    }

    if (lookupSpec.getIsHybridSearch() || hybrid_scoring_util::isHybridSearchPipeline(pipeline)) {
        // If there is a hybrid search stage in our pipeline, then we should validate that we
        // are not running on a timeseries collection.
        //
        // If the hybrid search flag is set to true, this request may have
        // come from a mongos that does not know if the collection is a valid collection for
        // hybrid search. Therefore, we must validate it here.
        hybrid_scoring_util::assertForeignCollectionIsNotTimeseries(fromNs, pExpCtx);
    }

    boost::intrusive_ptr<DocumentSourceLookUp> lookupStage = nullptr;
    if (hasPipeline) {
        if (localField.empty() && foreignField.empty()) {
            // $lookup specified with only pipeline syntax.
            lookupStage = new DocumentSourceLookUp(std::move(fromNs),
                                                   std::move(as),
                                                   std::move(pipeline),
                                                   std::move(letVariables),
                                                   boost::none,
                                                   pExpCtx);
        } else {
            // $lookup specified with pipeline syntax and local/foreignField syntax.
            uassert(ErrorCodes::FailedToParse,
                    "$lookup requires both or neither of 'localField' and 'foreignField' to be "
                    "specified",
                    !localField.empty() && !foreignField.empty());

            lookupStage =
                new DocumentSourceLookUp(std::move(fromNs),
                                         std::move(as),
                                         std::move(pipeline),
                                         std::move(letVariables),
                                         std::pair(std::move(localField), std::move(foreignField)),
                                         pExpCtx);
        }
    } else {
        // $lookup specified with only local/foreignField syntax.
        uassert(ErrorCodes::FailedToParse,
                "$lookup requires both or neither of 'localField' and 'foreignField' to be "
                "specified",
                !localField.empty() && !foreignField.empty());
        uassert(ErrorCodes::FailedToParse,
                "$lookup with a 'let' argument must also specify 'pipeline'",
                !hasLet);

        lookupStage = new DocumentSourceLookUp(std::move(fromNs),
                                               std::move(as),
                                               std::move(localField),
                                               std::move(foreignField),
                                               pExpCtx);
    }

    if (!unwindSpec.isEmpty()) {
        lookupStage->_sharedState->unwindSrc = boost::dynamic_pointer_cast<DocumentSourceUnwind>(
            DocumentSourceUnwind::createFromBson(unwindSpec.firstElement(), pExpCtx));
    }
    lookupStage->determineSbeCompatibility();
    return lookupStage;
}

void DocumentSourceLookUp::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_resolvedNs);
    for (auto&& stage : _sharedState->resolvedIntrospectionPipeline->getSources()) {
        stage->addInvolvedCollections(collectionNames);
    }
}

void DocumentSourceLookUp::rebuildResolvedPipeline() {
    tassert(9775504,
            "Invalid resolved introspection pipeline ",
            _sharedState->resolvedIntrospectionPipeline);
    // We must serialize the resolved introspection pipeline with the "serializeForFLE2" option to
    // ensure that any nested DocumentSourceLookUp stages serialize their
    // _sharedState->resolvedPipeline.
    SerializationOptions opts{.serializeForFLE2 = true};
    _sharedState->resolvedPipeline =
        _sharedState->resolvedIntrospectionPipeline->serializeToBson(opts);
    // The introspection pipeline does not contain the placeholder match stage or the additional
    // filter. Add those back in here if applicable.
    if (_sharedState->fieldMatchPipelineIdx) {
        _sharedState->resolvedPipeline.insert(_sharedState->resolvedPipeline.begin() +
                                                  *_sharedState->fieldMatchPipelineIdx,
                                              BSON("$match" << BSONObj()));
    }

    if (_sharedState->additionalFilter) {
        auto matchObj = BSON("$match" << *_sharedState->additionalFilter);
        _sharedState->resolvedPipeline.push_back(matchObj);
    }
}

}  // namespace mongo
