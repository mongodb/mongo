// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_lookup.h"

#include <algorithm>
#include <array>
#include <string_view>

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
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_internal_hybrid_search.h"
#include "mongo/db/pipeline/document_source_lookup_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h"
#include "mongo/db/pipeline/search/document_source_search.h"
#include "mongo/db/pipeline/search/document_source_search_meta.h"
#include "mongo/db/pipeline/search/document_source_vector_search.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/sort_reorder_helpers.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/compiler/dependency_analysis/document_transformation_helpers.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/views/pipeline_resolver.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

namespace mongo {
using namespace std::literals::string_view_literals;

// Parses $lookup 'from' field. The 'from' field must be a string, or an object {db: "", coll: ""}
// that resolves to one of the following allowed namespaces:
//   - config.cache.chunks.* (any database's chunk cache)
//   - config.collections
//   - config.chunks
//   - config.queryShapeRepresentativeQueries
//   - local.oplog.rs
// The {db, coll} object form is otherwise rejected unless the 'allowGenericForeignDbLookup' is set.
NamespaceString parseLookupFromAndResolveNamespace(const BSONElement& elem,
                                                   const DatabaseName& defaultDb,
                                                   bool allowGenericForeignDbLookup) {
    // The 'from' field must be a string or an object.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$lookup 'from' field must be a string or an object, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::string || elem.type() == BSONType::object);

    if (elem.type() == BSONType::string) {
        return NamespaceStringUtil::deserialize(defaultDb, elem.valueStringData());
    }

    // Validate the db and coll names.
    const auto tenantId = defaultDb.tenantId();
    const auto vts = tenantId
        ? boost::make_optional(auth::ValidatedTenancyScopeFactory::create(
              *tenantId, auth::ValidatedTenancyScopeFactory::TrustedForInnerOpMsgRequestTag{}))
        : boost::none;
    auto spec = NamespaceSpec::parse(
        elem.embeddedObject(),
        IDLParserContext{
            elem.fieldNameStringData(), vts, tenantId, SerializationContext::stateDefault()});
    auto nss = NamespaceStringUtil::deserialize(spec.getDb().value_or(DatabaseName()),
                                                spec.getColl().value_or(""));
    // In the cases nss == config.collections, config.chunks, or queryShapeRepresentativeQueries we
    // can proceed with the lookup as the merge will be done on the config server.
    bool isConfigSvrSupportedCollection = nss == NamespaceString::kConfigsvrCollectionsNamespace ||
        nss == NamespaceString::kConfigsvrChunksNamespace ||
        nss == NamespaceString::kQueryShapeRepresentativeQueriesNamespace;
    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << "$lookup with syntax {from: {db:<>, coll:<>},..} is not supported for db: "
                      << nss.dbName().toStringForErrorMsg() << " and coll: " << nss.coll(),
        nss.isConfigDotCacheDotChunks() || nss == NamespaceString::kRsOplogNamespace ||
            isConfigSvrSupportedCollection || allowGenericForeignDbLookup);
    return nss;
}

// The mongot_lookup_prefix helpers (isSourceStage / isSupportStage / prefixEndIdx / extractPrefix)
// live in search_helper_bson_obj.h so this search-specific logic stays with the other BSON-level
// mongot helpers rather than in the generic $lookup code. This alias keeps the call sites terse.
using namespace search_helper_bson_obj::mongot_lookup_prefix;

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

// Creates the conditions for joining the local and foreign fields inside of a $match.
BSONObj createMatchStageJoinObj(const Document& input,
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

void lookupPipeValidator(const Pipeline& pipeline) {
    // TODO SERVER-128961: Teach the LiteParsed layer the lookup constraints of built-in stages (as
    // is already done for extension stages) so lite-parse can own this check uniformly and this
    // full-parse validation can be removed.
    for (const auto& src : pipeline.getSources()) {
        uassert(51047,
                str::stream() << src->getSourceName()
                              << " is not allowed within a $lookup's sub-pipeline",
                src->constraints().isAllowedInLookupPipeline());
    }
}

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _variablesParseState(_variables.useIdGenerator()),
      _sharedState(std::make_shared<LookUpSharedState>()) {
    if (!_fromNs.isOnInternalDb()) {
        globalOpCounters().gotNestedAggregate();
    }
    const auto resolvedNamespace = expCtx->hasResolvedNamespace(_fromNs)
        ? expCtx->getResolvedNamespace(_fromNs)
        : ResolvedNamespace{_fromNs, std::vector<BSONObj>{}};
    _resolvedNs = resolvedNamespace.getResolvedNamespace();
    _fromNsIsAView = resolvedNamespace.isInvolvedNamespaceAView();

    // Prevent view resolution for rawData timeseries commands.
    if (!resolvedNamespace.isInvolvedNamespaceAView() ||
        !isRawDataOperation(expCtx->getOperationContext()) ||
        !resolvedNamespace.getResolvedNamespace().isTimeseriesBucketsCollection()) {
        _sharedState->resolvedPipeline = resolvedNamespace.getBsonPipeline();
    }

    _fromExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, resolvedNamespace.getResolvedNamespace(), resolvedNamespace.getCollUUID(), _fromNs);
    _fromExpCtx->setInLookup(true);
    // We must use variables from the sub-pipeline's ExpressionContext, because some extra varialbes
    // might have been defined in makeCopyForSubPipelineFromExpressionContext
    _variables = _fromExpCtx->variables;
    _variablesParseState = _fromExpCtx->variablesParseState.copyWith(_variables.useIdGenerator());
}

DocumentSourceLookUp::DocumentSourceLookUp(NamespaceString fromNs,
                                           std::string as,
                                           std::string localField,
                                           std::string foreignField,
                                           const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, expCtx) {
    _localField = std::move(localField);
    _foreignField = std::move(foreignField);

    // We append an additional BSONObj to '_sharedState->resolvedPipeline' as a placeholder for the
    // $match stage we'll eventually construct from the input document.
    _sharedState->resolvedPipeline.reserve(_sharedState->resolvedPipeline.size() + 1);

    // Initialize the introspection pipeline before we insert the $match. This is okay because we do
    // not use the introspection pipeline during/after query execution, which is when the $match is
    // necessary.
    initializeResolvedIntrospectionPipeline();

    _sharedState->resolvedPipeline.push_back(BSON("$match" << BSONObj()));
    _fieldMatchPipelineIdx = _sharedState->resolvedPipeline.size() - 1;
}

std::vector<BSONObj> extractSourceStage(const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return {};
    }

    // When serializing $lookup to send the pipeline to a shard, we send the
    // '_sharedState->resolvedIntrospectionPipeline'. '_sharedState->resolvedIntrospectionPipeline'
    // is parsed and contains a desugared version of $documents. Therefore, on a shard, we must
    // check for a desugared $documents and return those stages as the source stage.
    if (auto desugaredStages =
            DocumentSourceDocuments::extractDesugaredStagesFromPipeline(pipeline);
        desugaredStages.has_value()) {
        return desugaredStages.value();
    }
    // When we first create a $lookup stage, the input 'pipeline' is unparsed, so we
    // check for the $documents stage itself.
    if (pipeline[0].hasField(DocumentSourceDocuments::kStageName) ||
        pipeline[0].hasField(DocumentSourceQueue::kStageName)) {
        return {pipeline[0]};
    }
    // For mongot subpipelines the join $match must be placed after the entire mongot prefix (the
    // search source stage plus any idLookup / storedSource $replaceRoot support stages), not just
    // after the first stage. Returning the full prefix here keeps the mongot source stage first and
    // ensures the equality $match runs after view transforms / storedSource promotion.
    return extractPrefix(pipeline);
}

namespace {
// True if 'pipeline' should be treated as a mongot search subpipeline for the purposes of $lookup
// view handling. This covers three shapes:
//   - a legacy mongot pipeline ($search / $searchMeta / $vectorSearch first),
//   - an extension mongot pipeline (the extension desugar of those), and
//   - an already-desugared mongot pipeline whose first stage is a mongot source stage (DRM /
//     $_extension* / $_internalSearchMongotRemote).
// Recognizing the extension/desugared cases (not just isMongotPipeline) is required so $lookup
// still treats the foreign namespace as a mongot-indexed view: otherwise the view pipeline is
// prepended and the mongot stage is no longer first, yielding empty join results (or error 40602
// because the mongot stage must lead the subpipeline).
bool isMongotLookupSubpipeline(const std::shared_ptr<IncrementalFeatureRolloutContext>& ifrContext,
                               const std::vector<BSONObj>& pipeline) {
    if (pipeline.empty()) {
        return false;
    }
    return search_helper_bson_obj::isMongotPipeline(ifrContext, pipeline) ||
        search_helper_bson_obj::isExtensionMongotPipeline(ifrContext, pipeline) ||
        isSourceStage(pipeline[0]);
}
}  // namespace

// Process and copy the given `pipeline` to the `_sharedState->resolvedPipeline` attribute and
// compute where the $match stage is going to be placed, indicated through the
// `_fieldMatchPipelineIdx` variable.
void DocumentSourceLookUp::resolvedPipelineHelper(
    NamespaceString fromNs,
    std::vector<BSONObj> pipeline,
    boost::optional<std::pair<std::string, std::string>> localForeignFields,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    FirstStageViewApplicationPolicy subpipelineViewPolicy) {
    // When fromNs represents a view, we have to decipher if the view is mongot-indexed or not.
    // Currently, if the pipeline to be run on the joined collection is a
    // mongot pipeline (it starts with $search, $searchMeta, or $vectorSearch), $lookup assumes
    // the view is mongot-indexed. The same applies when the subpipeline's first stage declared
    // FirstStageViewApplicationPolicy::kDoNothing at lite-parse time (e.g. an extension search
    // stage): the stage applies the view itself, so the view pipeline must not be prepended.
    //
    // Skip validation/view application when we know that the router already processed the view.
    const bool pipelineIsAlreadyDesugared = !pipeline.empty() &&
        pipeline[0].hasField(InternalSearchMongotRemoteSpec::kMongotQueryFieldName);

    const bool isLegacyMongotPipeline =
        search_helper_bson_obj::isMongotPipeline(expCtx->getIfrContext(), pipeline);
    if (_fromNsIsAView &&
        (isMongotLookupSubpipeline(expCtx->getIfrContext(), pipeline) ||
         subpipelineViewPolicy == FirstStageViewApplicationPolicy::kDoNothing) &&
        !pipelineIsAlreadyDesugared) {
        if (isLegacyMongotPipeline) {
            // The user pipeline is a legacy mongot pipeline so we assume the view is a
            // mongot-indexed view. Stash the view on the subpipeline's ExpressionContext so the
            // legacy stage's createFromBson can attach it: idLookup applies the view transforms
            // as part of its subpipeline.
            _fromExpCtx->setView(boost::make_optional(ResolvedNamespace::makeForView(
                fromNs, _resolvedNs, _sharedState->resolvedPipeline)));
        }
        _sharedState->resolvedPipeline = pipeline;
        // The join $match belongs immediately after the mongot prefix. For an unparsed pipeline
        // (a single $search/$searchMeta/$vectorSearch stage) the prefix is one stage, so index 1.
        // If the pipeline already arrived desugared (e.g. DRM + idLookup, or a storedSource
        // $replaceRoot), the prefix spans multiple stages and the $match must go after all of them.
        const size_t prefixLen = prefixEndIdx(pipeline);
        _fieldMatchPipelineIdx = prefixLen == 0 ? 1 : prefixLen;
        if (localForeignFields != boost::none) {
            std::tie(_localField, _foreignField) = *localForeignFields;
        }
        return;
    }

    if (localForeignFields != boost::none) {
        std::tie(_localField, _foreignField) = *localForeignFields;

        // The $match stage should come after $documents if present.
        auto sourceStages = extractSourceStage(pipeline);
        _sharedState->resolvedPipeline.insert(
            _sharedState->resolvedPipeline.end(), sourceStages.begin(), sourceStages.end());

        // Save the correct position of the $match, but wait to insert it until we have finished
        // constructing the pipeline and created the introspection pipeline below.
        _fieldMatchPipelineIdx = _sharedState->resolvedPipeline.size();

        // Add the rest of the user pipeline to `_sharedState->resolvedPipeline` after any potential
        // view prefix and $match.
        _sharedState->resolvedPipeline.insert(_sharedState->resolvedPipeline.end(),
                                              pipeline.begin() + sourceStages.size(),
                                              pipeline.end());

    } else {
        // Add the user pipeline to '_sharedState->resolvedPipeline' after any potential view prefix
        // and $match.
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
    // '_sharedState->resolvedPipeline' will first be initialized by the constructor delegated to
    // within this constructor's initializer list. It will be populated with view pipeline prefix if
    // 'fromNs' represents a view. We will then append stages to ensure any view prefix is not
    // overwritten.
    resolvedPipelineHelper(fromNs, pipeline, localForeignFields, expCtx);

    _userPipeline = std::move(pipeline);

    parseAndDefineLetVariables(letVariables, expCtx);

    // Initialize the introspection pipeline before we insert the $match (if applicable). This is
    // okay because we only use the introspection pipeline for reference while doing query analysis
    // and analyzing involved dependencies/variables/collections/constraints. We do not use the
    // introspection pipeline during/after query execution, which is when the $match is necessary.
    // It wouldn't hurt anything to include the $match in this pipeline, but we also use the
    // introspection pipeline in serialization, so it would be a bit odd to include an extra empty
    // $match.
    initializeResolvedIntrospectionPipeline();

    insertFieldMatchPlaceholder();
}

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    std::vector<BSONObj> userPipeline,
    StageParamsPipeline subpipelineStageParams,
    BSONObj letVariables,
    boost::optional<std::pair<std::string, std::string>> localForeignFields,
    boost::optional<BSONObj> unwindSpec,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    bool containsUserSpecifiedPipeline,
    FirstStageViewApplicationPolicy subpipelineViewPolicy)
    : DocumentSourceLookUp(fromNs, as, pExpCtx) {
    resolvedPipelineHelper(
        fromNs, userPipeline, localForeignFields, pExpCtx, subpipelineViewPolicy);

    parseAndDefineLetVariables(letVariables, pExpCtx);
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());
    _fromExpCtx->startExpressionCounters();
    // The desugared mongot stage's injected 'view' field requires isHybridSearch on the
    // sub-pipeline's expCtx.
    if (hybrid_scoring_util::isHybridSearchPipeline(userPipeline)) {
        _fromExpCtx->setIsHybridSearch();
    }
    const auto& resolvedNamespaces = pExpCtx->getResolvedNamespaces();
    auto it = resolvedNamespaces.find(_fromNs);
    if (it != resolvedNamespaces.end() && !it->second.getBsonPipeline().empty()) {
        _sharedState->resolvedIntrospectionPipeline =
            parsePipelineFromStageParamsWithMaybeViewDefinition(
                _fromExpCtx, it->second, std::move(subpipelineStageParams), userPipeline, _fromNs);
    } else {
        _sharedState->resolvedIntrospectionPipeline = Pipeline::parseFromStageParams(
            std::move(subpipelineStageParams), _fromExpCtx, lookupPipeValidator);
    }
    _fromExpCtx->stopExpressionCounters();

    // For hybrid-search ($rankFusion/$scoreFusion) pipelines on a view, replace resolvedPipeline
    // with the serialized introspection pipeline when nested sub-pipelines are present. At
    // introspection time, bindResolvedNamespaceToStages stitches the view into every nested
    // sub-pipeline; without this, buildPipeline re-parses the raw user pipeline and those nested
    // stages scan the underlying collection instead of the view.
    //
    // Restricted to isHybridSearch() because plain $search/$vectorSearch stages have a 'view'
    // field injected into their spec by createFromBson; serializing them would embed that field
    // and fail validateViewNotSetByUser (error 5491300) on re-parse.
    //
    // Skipped for localField/foreignField joins: those need a per-getNext() $match placeholder
    // that the introspection pipeline omits.
    if (_fromNsIsAView && !hasLocalFieldForeignFieldJoin() && _fromExpCtx->isHybridSearch()) {
        const auto& sources = _sharedState->resolvedIntrospectionPipeline->getSources();
        const bool hasNestedSubPipeline =
            std::any_of(sources.begin(), sources.end(), [](const auto& source) {
                return source->getSubPipeline() != nullptr;
            });
        if (hasNestedSubPipeline) {
            _sharedState->resolvedPipeline =
                _sharedState->resolvedIntrospectionPipeline->serializeToBson();
            _sharedState->resolvedPipelineViewBinding =
                LookupResolvedPipelineViewBinding::kAlreadyBound;
        }
    }

    // Note that we can't just check `userPipeline.empty()` here.
    // For localField/foreignField joins, a placeholder $match is injected into userPipeline, making
    // it non-empty even though no pipeline was given explicitly by the user.
    if (containsUserSpecifiedPipeline) {
        _userPipeline = std::move(userPipeline);
    }

    insertFieldMatchPlaceholder();

    // Absorb an $unwind spec if one was provided via LookUpStageParams.
    if (unwindSpec && !unwindSpec->isEmpty()) {
        _unwindSrc = boost::dynamic_pointer_cast<DocumentSourceUnwind>(
            DocumentSourceUnwind::createFromBson(unwindSpec->firstElement(), pExpCtx));
    }
}

void DocumentSourceLookUp::relocateFieldMatchPlaceholder(
    boost::intrusive_ptr<DocumentSourceLookUp>& lookupStage, size_t newIdx) {
    if (!lookupStage->_fieldMatchPipelineIdx || newIdx == *lookupStage->_fieldMatchPipelineIdx)
        return;
    auto& resolvedPipeline = lookupStage->_sharedState->resolvedPipeline;
    auto oldIdx = *lookupStage->_fieldMatchPipelineIdx;
    const auto oldSize = resolvedPipeline.size();
    tassert(12761200,
            str::stream() << "expected empty $match placeholder at old _fieldMatchPipelineIdx "
                          << oldIdx << " in resolvedPipeline of size " << oldSize,
            oldIdx < oldSize && resolvedPipeline[oldIdx].hasField("$match") &&
                resolvedPipeline[oldIdx]["$match"].Obj().isEmpty());
    // 'newIdx' is an index into the placeholder-containing pipeline and may equal 'oldSize' when
    // the mongot prefix is the entire subpipeline and the $match must be appended at the end.
    tassert(12761201,
            str::stream() << "internalFieldMatchPipelineIdx " << newIdx
                          << " out of range of resolvedPipeline of size " << oldSize,
            newIdx <= oldSize);
    resolvedPipeline.erase(resolvedPipeline.begin() + oldIdx);
    // Erasing the old placeholder shifts every later element left by one, so a target index past
    // the old position must be decremented to land in the right place (this also maps an
    // append-at-end 'newIdx' == oldSize to the new last position).
    if (newIdx > oldIdx) {
        --newIdx;
    }
    resolvedPipeline.insert(resolvedPipeline.begin() + newIdx, BSON("$match" << BSONObj()));
    lookupStage->_fieldMatchPipelineIdx = newIdx;
}

namespace {
// TODO SERVER-121094 Remove when legacy mongot branches are removed from pipeline
// parsing/desugaring/resolution.
// Computes where the localField/foreignField equality $match placeholder must live in a mongot
// $lookup subpipeline on the shard. The placeholder must sit immediately after the full mongot
// prefix (the search source stage plus any idLookup / storedSource $replaceRoot support stages).
//
// 'resolvedPipeline' may already contain the empty $match placeholder (inserted during
// construction at the pre-relocation index, which can fall inside the mongot prefix). Skip that
// placeholder while scanning so it does not truncate the prefix. Returns the target insert index,
// which may equal resolvedPipeline.size() to append at the end.
size_t computeDesugaredMongotFieldMatchIdx(const std::vector<BSONObj>& resolvedPipeline) {
    boost::optional<size_t> lastPrefixIdx;
    for (size_t i = 0; i < resolvedPipeline.size(); ++i) {
        const auto& stage = resolvedPipeline[i];
        // Skip an already-inserted empty $match placeholder. This relies on the injected
        // placeholder being the only empty {$match: {}} in a desugared mongot subpipeline: mongot
        // desugaring never emits one, and a user-authored empty $match is semantically inert, so
        // treating any empty $match as the placeholder does not change results here.
        if (stage.hasField("$match") && stage.getObjectField("$match").isEmpty()) {
            continue;
        }
        if (isSourceStage(stage) || isSupportStage(stage)) {
            lastPrefixIdx = i;
            continue;
        }
        // First non-prefix, non-placeholder stage after the prefix ends the scan.
        if (lastPrefixIdx) {
            break;
        }
    }
    if (!lastPrefixIdx) {
        return resolvedPipeline.empty() ? 0 : resolvedPipeline.size() - 1;
    }
    return *lastPrefixIdx + 1;
}

// TODO SERVER-121094 Remove when legacy mongot branches are removed from pipeline
// parsing/desugaring/resolution.
// Computes where the localField/foreignField equality $match placeholder must live in a hybrid
// search ($rankFusion/$scoreFusion) $lookup subpipeline on the shard.
size_t computeHybridSearchFieldMatchIdx(const std::vector<BSONObj>& resolvedPipeline) {
    for (size_t i = resolvedPipeline.size(); i-- > 0;) {
        if (resolvedPipeline[i].hasField(DocumentSourceInternalHybridSearch::kStageName)) {
            return i;
        }
    }
    // Guard the empty case so size() - 1 does not wrap to a huge index (matches the mongot sibling
    // above). Not reachable today since a hybrid subpipeline is never empty here.
    return resolvedPipeline.empty() ? 0 : resolvedPipeline.size() - 1;
}
}  // namespace

DocumentSourceContainer DocumentSourceLookUp::createFromStageParams(
    LookUpStageParams& params, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-121094 This can be removed once hybrid search desugars into the internal hybrid
    // search stage.
    const bool isHybridSearchLookup =
        params.isHybridSearch || hybrid_scoring_util::isHybridSearchPipeline(params.pipeline);
    if (isHybridSearchLookup) {
        hybrid_scoring_util::assertForeignCollectionIsNotTimeseries(params.fromNss, expCtx);
    } else {
        hybrid_scoring_util::assertForeignSearchViewIsNotTimeseries(params.fromNss, expCtx);
    }

    const bool hasLocal = params.localField.has_value();
    const bool hasForeign = params.foreignField.has_value();
    uassert(ErrorCodes::FailedToParse,
            "$lookup requires both or neither of 'localField' and 'foreignField' to be specified",
            hasLocal == hasForeign);

    boost::optional<std::pair<std::string, std::string>> localForeignFields;
    if (hasLocal) {
        localForeignFields =
            std::pair(std::move(*params.localField), std::move(*params.foreignField));
    }

    // Without a subpipeline there is no desugared LPP to forward.
    if (!params.subpipelineStageParams.has_value()) {
        // When a $lookup has no user pipeline, bypass createFromBson and instead just use the
        // provided view LPP directly.
        if (auto view =
                tryGetPreResolvedNamespace(params.fromNss, expCtx->getResolvedNamespaces())) {
            auto stageParams = view->getViewPipeline().getStageParams();
            return {make_intrusive<DocumentSourceLookUp>(std::move(params.fromNss),
                                                         std::move(params.as),
                                                         std::vector<BSONObj>{},
                                                         std::move(stageParams),
                                                         std::move(params.letVariables),
                                                         std::move(localForeignFields),
                                                         std::move(params.unwindSpec),
                                                         expCtx)};
        }
        return {DocumentSourceLookUp::createFromBson(params.getOriginalBson(), expCtx)};
    }

    auto lookupStage =
        make_intrusive<DocumentSourceLookUp>(std::move(params.fromNss),
                                             std::move(params.as),
                                             std::move(params.pipeline),
                                             std::move(params.subpipelineStageParams.value()),
                                             std::move(params.letVariables),
                                             std::move(localForeignFields),
                                             std::move(params.unwindSpec),
                                             expCtx,
                                             !params.noUserPipeline,
                                             params.subpipelineViewPolicy);

    // TODO SERVER-121094 Remove when legacy mongot branches are removed from pipeline
    // parsing/desugaring/resolution.
    if (params.internalFromIsAView && lookupStage->hasLocalFieldForeignFieldJoin() &&
        params.internalFieldMatchPipelineIdx) {
        // The router computed internalFieldMatchPipelineIdx against the undesugared pipeline (index
        // 1, right after the leading search/hybrid stage). On the shard the pipeline arrives
        // desugared, so we recompute the field match placeholder's position.
        const auto& resolvedPipeline = lookupStage->_sharedState->resolvedPipeline;
        size_t newIdx = isHybridSearchLookup
            ? computeHybridSearchFieldMatchIdx(resolvedPipeline)
            : computeDesugaredMongotFieldMatchIdx(resolvedPipeline);
        relocateFieldMatchPlaceholder(lookupStage, newIdx);
    } else if (const auto& idx = params.internalFieldMatchPipelineIdx) {
        relocateFieldMatchPlaceholder(lookupStage, static_cast<size_t>(*idx));
    }

    if (params.internalFromIsAView) {
        lookupStage->_fromNsIsAView = true;
    }

    return {lookupStage};
}

DocumentSourceContainer lookupStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<LookUpStageParams*>(stageParams.get());
    tassert(11786210, "Expected LookUpStageParams for lookup stage", typedParams != nullptr);

    // TODO SERVER-121094 Remove when feature flag is removed.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (!hybridSearchFlagEnabled) {
        return {DocumentSourceLookUp::createFromBson(typedParams->getOriginalBson(), expCtx)};
    }

    // Reject a user-supplied isHybridSearch flag before building from stage params.
    if (auto originalSpec = typedParams->getOriginalBson();
        originalSpec.type() == BSONType::object) {
        hybrid_scoring_util::validateIsHybridSearchNotSetByUser(expCtx,
                                                                originalSpec.embeddedObject());
    }
    return DocumentSourceLookUp::createFromStageParams(*typedParams, expCtx);
}

ALLOCATE_AND_REGISTER_STAGE_PARAMS(lookup, LookUpStageParams)

std::unique_ptr<Pipeline> DocumentSourceLookUp::parsePipelineFromStageParamsWithMaybeViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
    const ResolvedNamespace& resolvedNs,
    StageParamsPipeline stageParams,
    const std::vector<BSONObj>& rawPipeline,
    const NamespaceString& fromNss) {
    auto opts = pipeline_factory::kDesugarOnly;
    opts.validator = lookupPipeValidator;
    return pipeline_factory::makePipelineFromViewDefinitionStageParams(
        fromExpCtx, resolvedNs, std::move(stageParams), rawPipeline, fromNss, opts);
}

DocumentSourceLookUp::DocumentSourceLookUp(const DocumentSourceLookUp& original,
                                           const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(kStageName, newExpCtx),
      _fromNs(original._fromNs),
      _resolvedNs(original._resolvedNs),
      _fromNsIsAView(original._fromNsIsAView),
      _as(original._as),
      _localField(original._localField),
      _foreignField(original._foreignField),
      _fieldMatchPipelineIdx(original._fieldMatchPipelineIdx),
      _variables(original._variables),
      _variablesParseState(original._variablesParseState.copyWith(_variables.useIdGenerator())),
      _fromExpCtx(makeCopyFromExpressionContext(original._fromExpCtx,
                                                _resolvedNs,
                                                original._fromExpCtx->getUUID(),
                                                boost::none,
                                                original._fromExpCtx->getView())),
      _userPipeline(original._userPipeline),
      _sharedState(std::make_shared<LookUpSharedState>()) {
    _additionalFilter = original._additionalFilter;
    _sharedState->resolvedPipeline = original._sharedState->resolvedPipeline;
    _sharedState->resolvedPipelineViewBinding = original._sharedState->resolvedPipelineViewBinding;
    _sharedState->resolvedIntrospectionPipeline =
        original._sharedState->resolvedIntrospectionPipeline->clone(_fromExpCtx);

    if (original._matchSrc) {
        _matchSrc = static_cast<DocumentSourceMatch*>(original._matchSrc->clone(getExpCtx()).get());
    }
    if (original._unwindSrc) {
        _unwindSrc =
            static_cast<DocumentSourceUnwind*>(original._unwindSrc->clone(getExpCtx()).get());
    }
    // clone let variables with new expCtx in case the original expCtx is deleted.
    copyLetVariablesWithNewExpCtx(original._letVariables, *newExpCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceLookUp::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    return make_intrusive<DocumentSourceLookUp>(*this, newExpCtx);
}

void DocumentSourceLookUp::copyLetVariablesWithNewExpCtx(const std::vector<LetVariable>& src,
                                                         ExpressionContext& newExpCtx) {
    _letVariables.clear();
    _letVariables.reserve(src.size());

    for (const auto& var : src) {
        _letVariables.emplace_back(var.clone(newExpCtx));
    }
}

ALLOCATE_DOCUMENT_SOURCE_ID(lookup, DocumentSourceLookUp::id)

std::string_view DocumentSourceLookUp::getSourceName() const {
    return kStageName;
}

bool DocumentSourceLookUp::foreignShardedLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !getExpCtx()->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

StageConstraints DocumentSourceLookUp::constraints(PipelineSplitState pipeState) const {
    HostTypeRequirement hostRequirement;
    bool nominateMergingShard = false;
    if (_fromNs.isConfigDotCacheDotChunks()) {
        // $lookup from config.cache.chunks* namespaces is permitted to run on each individual
        // shard, rather than just a merging shard, since each shard should have an identical copy
        // of the namespace.
        hostRequirement = HostTypeRequirement::kTargetedShards;
    } else if (pipeState == PipelineSplitState::kSplitForShards) {
        // This stage will only be on the shards pipeline if $lookup on sharded foreign collections
        // is allowed.
        hostRequirement = HostTypeRequirement::kTargetedShards;
    } else if (_fromNs.isCollectionlessAggregateNS()) {
        // When the inner pipeline does not target a collection, it can run on any node.
        hostRequirement = HostTypeRequirement::kCollectionlessSourceRunOnceAnyNode;
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
    constraints.canSwapWithSkippingOrLimitingStage = !_unwindSrc;
    constraints.outputDependsOnSingleInput = true;

    return constraints;
}

boost::optional<ShardId> DocumentSourceLookUp::computeMergeShardId() const {
    // If this $lookup is on the merging half of the pipeline and the inner collection isn't
    // sharded (that is, it is either unsplittable or untracked), then we should merge on the shard
    // which owns the inner collection.
    if (auto msi = getExpCtx()->getMongoProcessInterface()->determineSpecificMergeShard(
            getExpCtx()->getOperationContext(), _fromNs)) {
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
    OrderedPathSet modifiedPaths{_as.fullPath()};
    if (_unwindSrc) {
        auto pathsModifiedByUnwind = _unwindSrc->getModifiedPaths();
        tassert(11282981,
                "Expecting $unwind to modify a finite set of paths",
                pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

void DocumentSourceLookUp::describeTransformation(
    document_transformation::DocumentOperationVisitor& visitor) const {
    using namespace mongo::document_transformation;
    if (_unwindSrc) {
        // After $lookup+$unwind the 'as' field holds one document from the subpipeline
        // (or null/absent for preserveNullAndEmptyArrays), never an array.
        visitor(NonArrayModifyPath{_as.fullPath(), /*canLeafBeArray*/ false});
        if (const auto& idxPath = _unwindSrc->indexPath()) {
            // includeArrayIndex is a numeric position, never an array.
            visitor(NonArrayModifyPath{idxPath->fullPath(), /*canLeafBeArray*/ false});
        }
        return;
    }
    // Without an absorbed $unwind the 'as' field is an array.
    visitor(NonArrayModifyPath{_as.fullPath(), /*canLeafBeArray*/ true});
}

DocumentSourceContainer::iterator DocumentSourceLookUp::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282980, "Expecting DocumentSource iterator pointing to this stage", *itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If the following stage is $sort and this $lookup has not absorbed a following $unwind, try to
    // move the $sort ahead of the $lookup.
    if (!_unwindSrc) {
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
    if (nextUnwind && !_unwindSrc && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwindSrc = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }

    // Attempt to internalize any predicates of a $match upon the "_as" field.
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (!nextMatch) {
        return std::next(itr);
    }

    if (!_unwindSrc || _unwindSrc->indexPath() || _unwindSrc->preserveNullAndEmptyArrays()) {
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
    if (_fromExpCtx->getCollator() &&
        !CollatorInterface::collatorsMatch(getExpCtx()->getCollator(),
                                           _fromExpCtx->getCollator())) {
        return std::next(itr);
    }

    auto outputPath = _as.fullPath();

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
        // Consider {b: {$elemMatch: {$gt: 1, $lt: 4}}}, where "b" is our "_as" field. This rewrite
        // is not supported because there's no way to modify the expression to be a match just on
        // 'b'--we cannot change the path to an empty string, or remove the node entirely.
        // For other internal nodes with paths, we don't support the rewrite to keep the
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

    bool needToOptimize = false;
    if (!_matchSrc) {
        _matchSrc = nextMatch;
    } else {
        // We have already absorbed a $match. We need to join it with the next one.
        _matchSrc->joinMatchWith(nextMatch, MatchExpression::MatchType::AND);
        needToOptimize = true;
    }

    // Remove the original $match.
    container->erase(std::next(itr));

    // We have internalized a $match, but have not yet computed the descended $match that should
    // be applied to our queries. Note that we have to optimize the MatchExpression that we pass
    // into 'descendMatchOnPath' because the call to 'joinMatchWith' rebuilds the new $match stage
    // using each stage's unoptimized BSON predicate. The unoptimized BSON may contain predicates
    // that were optimized away, so that the checks performed by 'computeWhetherMatchOnlyOnAs' may
    // no longer be true for the combined $match's MatchExpression.
    _additionalFilter =
        DocumentSourceMatch::descendMatchOnPath(
            needToOptimize ? optimizeMatchExpression(
                                 std::move(_matchSrc->getMatchProcessor()->getExpression()),
                                 /* enableSimplification */ false)
                                 .get()
                           : _matchSrc->getMatchExpression(),
            _as.fullPath(),
            getExpCtx())
            ->getQuery()
            .getOwned();

    // Add '_additionalFilter' to '_sharedState->resolvedPipeline' if there is a pipeline. If there
    // is no pipeline, '_additionalFilter' can safely be added to the local/foreignField $match
    // stage during 'doGetNext()'.
    if (hasPipeline()) {
        auto matchObj = BSON("$match" << *_additionalFilter);
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

void DocumentSourceLookUp::parseAndDefineLetVariables(
    const BSONObj& letVariables, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    for (auto&& varElem : letVariables) {
        const auto varName = varElem.fieldNameStringData();
        variableValidation::validateNameForUserWrite(varName);

        _letVariables.emplace_back(
            std::string{varName},
            Expression::parseOperand(expCtx.get(), varElem, expCtx->variablesParseState),
            _variablesParseState.defineVariable(varName));
    }
}

void DocumentSourceLookUp::insertFieldMatchPlaceholder() {
    if (_fieldMatchPipelineIdx) {
        _sharedState->resolvedPipeline.insert(_sharedState->resolvedPipeline.begin() +
                                                  *_fieldMatchPipelineIdx,
                                              BSON("$match" << BSONObj()));
    }
}

void DocumentSourceLookUp::initializeResolvedIntrospectionPipeline() {
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());
    _fromExpCtx->startExpressionCounters();
    pipeline_factory::MakePipelineOptions pipelineOpts = pipeline_factory::kOptionsMinimal;
    pipelineOpts.validator = lookupPipeValidator;
    // Lite parse the resolved view pipeline and add its involved namespaces - the expCtx currently
    // does not contain entries for them.
    if (_fromNsIsAView) {
        LiteParsedPipeline viewLpp(_resolvedNs, _sharedState->resolvedPipeline);
        _fromExpCtx->addResolvedNamespaces(viewLpp.getInvolvedNamespaces());
    }
    _sharedState->resolvedIntrospectionPipeline =
        pipeline_factory::makePipeline(_sharedState->resolvedPipeline, _fromExpCtx, pipelineOpts);
    _fromExpCtx->stopExpressionCounters();
}

void DocumentSourceLookUp::serializeToArray(std::vector<Value>& array,
                                            const query_shape::SerializationOptions& opts) const {
    // When serializing for a remote dispatch, fully resolve the foreign namespace. `inRouter`
    // covers cases where the router is serializing to a shard, and `isSerializingForRemoteDispatch`
    // covers cases where a shard is serializing to another shard.
    const bool serializeForRemote =
        getExpCtx()->getInRouter() || opts.isSerializingForRemoteDispatch;
    const auto& serializeFromNs = (_fromNsIsAView && serializeForRemote) ? _resolvedNs : _fromNs;

    // Do not include the tenantId in serialized 'from' namespace.
    auto fromValue = getExpCtx()->getNamespaceString().isEqualDb(serializeFromNs)
        ? Value(opts.serializeIdentifier(serializeFromNs.coll()))
        : Value(Document{{"db",
                          opts.serializeIdentifier(
                              serializeFromNs.dbName().serializeWithoutTenantPrefix_UNSAFE())},
                         {"coll", opts.serializeIdentifier(serializeFromNs.coll())}});

    MutableDocument output(Document{
        {getSourceName(), Document{{"from", fromValue}, {"as", opts.serializeFieldPath(_as)}}}});

    if (hasLocalFieldForeignFieldJoin()) {
        output[getSourceName()]["localField"] = Value(opts.serializeFieldPath(_localField.value()));
        output[getSourceName()]["foreignField"] =
            Value(opts.serializeFieldPath(_foreignField.value()));
        // We need to serialize the `fieldMatchPipelineIdx` when fully resolving views so that the
        // remote receiver knows where the $match stage is.
        if (_fromNsIsAView && serializeForRemote && _userPipeline &&
            _fieldMatchPipelineIdx.has_value() && !opts.isSerializingForQueryStats() &&
            !opts.isSerializingForExplain() && !opts.serializeForFLE2) {
            output[getSourceName()]["$_internalFieldMatchPipelineIdx"] =
                Value(static_cast<long long>(*_fieldMatchPipelineIdx));
        }
    }

    // Save whether or not this `fromNs` was a view or not so that the remote receiver can make
    // optimization choices (specifically for identity-view $lookups and SBE-lowering to EQ_LOOKUP).
    if (_fromNsIsAView && serializeForRemote && !opts.isSerializingForQueryStats() &&
        !opts.isSerializingForExplain() && !opts.serializeForFLE2) {
        output[getSourceName()]["$_internalFromIsAView"] = Value(true);
    }

    // Add a pipeline field if only-pipeline syntax was used (to ensure the output is valid $lookup
    // syntax) or if a $match was absorbed.
    auto serializedPipeline = [&]() -> std::vector<BSONObj> {
        if (!_userPipeline) {
            return std::vector<BSONObj>{};
        }
        if (opts.isSerializingForQueryStats()) {
            // TODO SERVER-94227 we don't need to do any validation as part of this parsing pass.
            return pipeline_factory::makePipeline(
                       *_userPipeline, _fromExpCtx, pipeline_factory::kOptionsMinimal)
                ->serializeToBson(opts);
        }
        if (opts.isSerializingForExplain()) {
            // We must optimize a clone rather than the original resolvedIntrospectionPipeline
            // because optimization can resolve 'let' variables to constants, which would cause
            // dependency tracking to no longer see correlated references. That in turn would
            // break cache placement for correlated nested $lookups.
            auto optimizedCopy = _sharedState->resolvedIntrospectionPipeline->clone(_fromExpCtx);
            pipeline_optimization::optimizePipeline(*optimizedCopy);
            return optimizedCopy->serializeToBson(opts);
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
             * _fieldMatchPipelineIdx placeholder to the _sharedState->resolvedPipeline after it has
             * been serialized. 2) From query_rewriter_test.cpp, where we would like to verify the
             * _sharedState->resolvedPipeline was successfully rewritten.
             *
             * In both of these cases, we would like to serialize the
             * _sharedState->resolvedPipeline, since doGetNext() uses the resolved pipeline for the
             * query execution. Using the resolved pipeline also ensures we serialize nested
             * _pipelines (i.e from nested lookups) in their FLE2 rewritten form as well. However,
             * in both of these cases we don't want the index match placeholder. We don't want the
             * empty $match stage, because when $lookup's pipeline is parsed, the match stage is
             * added in the DocumentSourceLookUp constructor, leading to a duplicate empty match
             * stage.
             */
            if (_fieldMatchPipelineIdx) {
                resolvedPipelineWithoutIndexMatchPlaceholder.erase(
                    resolvedPipelineWithoutIndexMatchPlaceholder.begin() + *_fieldMatchPipelineIdx);
            }
            return resolvedPipelineWithoutIndexMatchPlaceholder;
        }
        return _sharedState->resolvedIntrospectionPipeline->serializeToBson(opts);
    }();
    if (_additionalFilter) {
        auto serializedFilter = [&]() -> BSONObj {
            if (opts.isSerializingForQueryStats()) {
                auto filter =
                    uassertStatusOK(MatchExpressionParser::parse(*_additionalFilter, getExpCtx()));
                return filter->serialize(opts);
            }
            return *_additionalFilter;
        }();
        serializedPipeline.emplace_back(BSON("$match" << serializedFilter));
    }
    // Even if the user did not provide a pipeline, we still need to serialize the view's pipeline.
    if (_fromNsIsAView && serializeForRemote && !_userPipeline &&
        _fieldMatchPipelineIdx.has_value() && *_fieldMatchPipelineIdx > 0 &&
        !opts.isSerializingForQueryStats() && !opts.isSerializingForExplain() &&
        !opts.serializeForFLE2) {
        std::vector<BSONObj> rewrittenViewStages =
            _sharedState->resolvedIntrospectionPipeline->serializeToBson(opts);
        serializedPipeline.insert(
            serializedPipeline.begin(), rewrittenViewStages.begin(), rewrittenViewStages.end());
    }
    if (!hasLocalFieldForeignFieldJoin() || serializedPipeline.size() > 0) {
        MutableDocument exprList;
        for (const auto& letVar : _letVariables) {
            exprList.addField(opts.serializeFieldPathFromString(letVar.name),
                              letVar.expression->serialize(opts));
        }
        output[getSourceName()]["let"] = Value(exprList.freeze());

        output[getSourceName()]["pipeline"] = Value(serializedPipeline);

        if (!opts.isSerializingForExplain() &&
            hybrid_scoring_util::isHybridSearchPipeline(
                _userPipeline.value_or(std::vector<BSONObj>()))) {
            output[getSourceName()][hybrid_scoring_util::kIsHybridSearchFlagFieldName] =
                Value(true);
        }
    }

    if (opts.isSerializingForExplain()) {
        if (_unwindSrc) {
            const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] =
                Value(DOC("preserveNullAndEmptyArrays"
                          << _unwindSrc->preserveNullAndEmptyArrays() << "includeArrayIndex"
                          << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        array.push_back(output.freezeToValue());
    } else if (opts.serializeForCloning && _unwindSrc) {
        output[getSourceName()]["$_internalUnwind"] = _unwindSrc->serialize(opts);
        array.push_back(output.freezeToValue());
    } else {
        array.push_back(output.freezeToValue());

        if (_unwindSrc) {
            _unwindSrc->serializeToArray(array);
        }
    }
}

DepsTracker::State DocumentSourceLookUp::getDependencies(DepsTracker* deps) const {
    if (hasPipeline() || _letVariables.size() > 0) {
        // We will use the introspection pipeline which we prebuilt during construction.
        tassert(11282979,
                "Expecting introspection pipeline prebuilt",
                _sharedState->resolvedIntrospectionPipeline);

        DepsTracker subDeps;

        // Get the subpipeline dependencies. Subpipeline stages may reference both 'let' variables
        // declared by this $lookup and variables declared externally.
        for (auto&& source : _sharedState->resolvedIntrospectionPipeline->getSources()) {
            source->getDependencies(&subDeps);
        }

        // Add the 'let' dependencies to the tracker.
        for (auto&& letVar : _letVariables) {
            expression::addDependencies(letVar.expression.get(), deps);
        }
    }

    if (hasLocalFieldForeignFieldJoin()) {
        const FieldRef ref(_localField->fullPath());
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

    // Purposely ignore '_matchSrc' and '_unwindSrc', since those should only be absorbed if we know
    // they are only operating on the "as" field which will be generated by this stage.

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
    for (auto&& letVar : _letVariables) {
        expression::addVariableRefs(letVar.expression.get(), refs);
        refs->erase(letVar.id);
    }
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceLookUp::distributedPlanLogic(
    const DistributedPlanContext* ctx) {
    // If $lookup into a sharded foreign collection is allowed and the foreign namespace is sharded,
    // top-level $lookup stages can run in parallel on the shards.
    //
    // Note that this decision is inherently racy and subject to become stale. This is okay because
    // either choice will work correctly; we are simply applying a heuristic optimization.
    if (foreignShardedLookupAllowed() && getExpCtx()->getSubPipelineDepth() == 0 &&
        !_fromNs.isCollectionlessAggregateNS() &&
        getExpCtx()->getMongoProcessInterface()->isSharded(_fromExpCtx->getOperationContext(),
                                                           _fromNs)) {
        tassert(
            8725000,
            "Should not attempt to nominate merging shard when $lookup is not acting as a merger",
            !mergeShardId.isInitialized() ||
                (mergeShardId.isInitialized() && getMergeShardId() == boost::none));
        return boost::none;
    }

    if (_fromExpCtx->getNamespaceString().isConfigDotCacheDotChunks()) {
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
        // '_fromExpCtx->getOperationContext()'.
        tassert(10713706,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->detachFromOperationContext();
        _sharedState->pipeline->detachFromOperationContext();
        tassert(10713707,
                "expecting _fromExpCtx->getOperationContext() == nullptr",
                _fromExpCtx->getOperationContext() == nullptr);
    }
    if (_fromExpCtx) {
        _fromExpCtx->setOperationContext(nullptr);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->detachFromOperationContext();
    }
}

void DocumentSourceLookUp::reattachSourceToOperationContext(OperationContext* opCtx) {
    if (_sharedState->pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating
        // '_fromExpCtx->getOperationContext()'.
        tassert(10713710,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->reattachToOperationContext(opCtx);
        _sharedState->pipeline->reattachToOperationContext(opCtx);
        tassert(10713711,
                "expecting _fromExpCtx->getOperationContext() == opCtx",
                _fromExpCtx->getOperationContext() == opCtx);
    }
    if (_fromExpCtx) {
        _fromExpCtx->setOperationContext(opCtx);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceLookUp::validateSourceOperationContext(const OperationContext* opCtx) const {
    if (getExpCtx()->getOperationContext() != opCtx ||
        (_fromExpCtx && _fromExpCtx->getOperationContext() != opCtx)) {
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

    // The isHybridSearch flag is internal-only: it is set when a desugared hybrid-search
    // sub-pipeline is serialized across the wire, and re-parsed by internal clients. Reject it when
    // a user supplies it directly.
    hybrid_scoring_util::validateIsHybridSearchNotSetByUser(pExpCtx, elem.Obj());

    auto lookupSpec = DocumentSourceLookupSpec::parse(elem.Obj(), IDLParserContext(kStageName));

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
        LiteParsedLookUp::validateLookupCollectionlessPipeline(pipeline);
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

        // TODO SERVER-121094 Remove this assertion when featureFlagExtensionsInsideHybridSearch is
        // removed (this whole code path will be dead code as well)
        auto ifrCtx = pExpCtx->getIfrContext();
        bool hybridSearchFlagEnabled = ifrCtx &&
            ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
        uassert(12982600,
                "$lookup with $rankFusion/$scoreFusion cannot use localField/foreignField syntax.",
                hybridSearchFlagEnabled || (localField.empty() && foreignField.empty()));
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
        // No pipeline specified, both localField and foreignField must be specified.
        uassert(ErrorCodes::FailedToParse,
                "$lookup requires either 'pipeline' or both 'localField' and 'foreignField' to be "
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
        lookupStage->_unwindSrc = boost::dynamic_pointer_cast<DocumentSourceUnwind>(
            DocumentSourceUnwind::createFromBson(unwindSpec.firstElement(), pExpCtx));
    }

    if (const auto& idx = lookupSpec.getInternalFieldMatchPipelineIdx())
        relocateFieldMatchPlaceholder(lookupStage, static_cast<size_t>(*idx));

    if (lookupSpec.getInternalFromIsAView().value_or(false)) {
        lookupStage->_fromNsIsAView = true;
    }

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
    query_shape::SerializationOptions opts{.serializeForFLE2 = true};
    _sharedState->resolvedPipeline =
        _sharedState->resolvedIntrospectionPipeline->serializeToBson(opts);
    _sharedState->resolvedPipelineViewBinding = LookupResolvedPipelineViewBinding::kAlreadyBound;

    // The introspection pipeline does not contain the placeholder match stage or the additional
    // filter. Add those back in here if applicable.
    if (_fieldMatchPipelineIdx) {
        _sharedState->resolvedPipeline.insert(_sharedState->resolvedPipeline.begin() +
                                                  *_fieldMatchPipelineIdx,
                                              BSON("$match" << BSONObj()));
    }

    if (_additionalFilter) {
        auto matchObj = BSON("$match" << *_additionalFilter);
        _sharedState->resolvedPipeline.push_back(matchObj);
    }
}

}  // namespace mongo
