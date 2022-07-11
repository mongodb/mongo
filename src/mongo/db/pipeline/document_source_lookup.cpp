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

#include "mongo/base/init.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/sort_reorder_helpers.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


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

void lookupPipeValidator(const Pipeline& pipeline) {
    const auto& sources = pipeline.getSources();
    std::for_each(sources.begin(), sources.end(), [](auto& src) {
        uassert(51047,
                str::stream() << src->getSourceName()
                              << " is not allowed within a $lookup's sub-pipeline",
                src->constraints().isAllowedInLookupPipeline());
    });
}

// Parses $lookup 'from' field. The 'from' field must be a string or one of the following
// exceptions:
// {from: {db: "config", coll: "cache.chunks.*"}, ...} or
// {from: {db: "local", coll: "oplog.rs"}, ...} or
// {from: {db: "local", coll: "tenantMigration.oplogView"}, ...} .
NamespaceString parseLookupFromAndResolveNamespace(const BSONElement& elem,
                                                   const DatabaseName& defaultDb) {
    // The object syntax only works for 'cache.chunks.*', 'local.oplog.rs', and
    // 'local.tenantMigration.oplogViewwhich' which are not user namespaces so object type is
    // omitted from the error message below.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$lookup 'from' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == BSONType::String || elem.type() == BSONType::Object);

    if (elem.type() == BSONType::String) {
        return NamespaceString(defaultDb, elem.valueStringData());
    }

    // Valdate the db and coll names.
    auto spec = NamespaceSpec::parse({elem.fieldNameStringData()}, elem.embeddedObject());
    // TODO SERVER-62491 Use system tenantId to construct nss if running in serverless.
    auto nss = NamespaceString(spec.getDb().value_or(""), spec.getColl().value_or(""));
    uassert(
        ErrorCodes::FailedToParse,
        str::stream() << "$lookup with syntax {from: {db:<>, coll:<>},..} is not supported for db: "
                      << nss.db() << " and coll: " << nss.coll(),
        nss.isConfigDotCacheDotChunks() || nss == NamespaceString::kRsOplogNamespace ||
            nss == NamespaceString::kTenantMigrationOplogView);
    return nss;
}

}  // namespace

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _variables(expCtx->variables),
      _variablesParseState(expCtx->variablesParseState.copyWith(_variables.useIdGenerator())) {
    const auto& resolvedNamespace = expCtx->getResolvedNamespace(_fromNs);
    _resolvedNs = resolvedNamespace.ns;
    _resolvedPipeline = resolvedNamespace.pipeline;

    _fromExpCtx = expCtx->copyForSubPipeline(resolvedNamespace.ns, resolvedNamespace.uuid);
    _fromExpCtx->inLookup = true;
    if (fromCollator) {
        _fromExpCtx->setCollator(std::move(fromCollator.get()));
        _hasExplicitCollation = true;
    }
}

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    std::string localField,
    std::string foreignField,
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, std::move(fromCollator), expCtx) {
    _localField = std::move(localField);
    _foreignField = std::move(foreignField);

    // We append an additional BSONObj to '_resolvedPipeline' as a placeholder for the $match stage
    // we'll eventually construct from the input document.
    _resolvedPipeline.reserve(_resolvedPipeline.size() + 1);
    _resolvedPipeline.push_back(BSON("$match" << BSONObj()));
    _fieldMatchPipelineIdx = _resolvedPipeline.size() - 1;

    initializeResolvedIntrospectionPipeline();
}

std::vector<BSONObj> extractSourceStage(const std::vector<BSONObj>& pipeline) {
    if (!pipeline.empty() &&
        (pipeline[0].hasField(DocumentSourceDocuments::kStageName) ||
         pipeline[0].hasField("$search"_sd))) {
        return {pipeline[0]};
    }
    return {};
}

DocumentSourceLookUp::DocumentSourceLookUp(
    NamespaceString fromNs,
    std::string as,
    std::vector<BSONObj> pipeline,
    BSONObj letVariables,
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator,
    boost::optional<std::pair<std::string, std::string>> localForeignFields,
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceLookUp(fromNs, as, std::move(fromCollator), expCtx) {
    // '_resolvedPipeline' will first be initialized by the constructor delegated to within this
    // constructor's initializer list. It will be populated with view pipeline prefix if 'fromNs'
    // represents a view. We will then append stages to ensure any view prefix is not overwritten.

    if (localForeignFields != boost::none) {
        std::tie(_localField, _foreignField) = *localForeignFields;

        // Append a BSONObj to '_resolvedPipeline' as a placeholder for the stage corresponding to
        // the local/foreignField $match. It must next after $documents if present.
        auto sourceStages = extractSourceStage(pipeline);
        _resolvedPipeline.insert(_resolvedPipeline.end(), sourceStages.begin(), sourceStages.end());
        _resolvedPipeline.push_back(BSON("$match" << BSONObj()));
        _fieldMatchPipelineIdx = _resolvedPipeline.size() - 1;
        // Add the user pipeline to '_resolvedPipeline' after any potential view prefix and $match
        _resolvedPipeline.insert(
            _resolvedPipeline.end(), pipeline.begin() + sourceStages.size(), pipeline.end());
    } else {
        // When local/foreignFields are included, we cannot enable the cache because the $match
        // is a correlated prefix that will not be detected. Here, local/foreignFields are absent,
        // so we enable the cache.
        _cache.emplace(internalDocumentSourceLookupCacheSizeBytes.load());
        // Add the user pipeline to '_resolvedPipeline' after any potential view prefix and $match
        _resolvedPipeline.insert(_resolvedPipeline.end(), pipeline.begin(), pipeline.end());
    }

    _userPipeline = std::move(pipeline);

    for (auto&& varElem : letVariables) {
        const auto varName = varElem.fieldNameStringData();
        variableValidation::validateNameForUserWrite(varName);

        _letVariables.emplace_back(
            varName.toString(),
            Expression::parseOperand(expCtx.get(), varElem, expCtx->variablesParseState),
            _variablesParseState.defineVariable(varName));
    }

    initializeResolvedIntrospectionPipeline();
}

DocumentSourceLookUp::DocumentSourceLookUp(const DocumentSourceLookUp& original,
                                           const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(
          kStageName,
          newExpCtx ? newExpCtx
                    : original.pExpCtx->copyWith(original.pExpCtx->ns, original.pExpCtx->uuid)),
      _fromNs(original._fromNs),
      _resolvedNs(original._resolvedNs),
      _as(original._as),
      _additionalFilter(original._additionalFilter),
      _localField(original._localField),
      _foreignField(original._foreignField),
      _fieldMatchPipelineIdx(original._fieldMatchPipelineIdx),
      _variables(original._variables),
      _variablesParseState(original._variablesParseState.copyWith(_variables.useIdGenerator())),
      _fromExpCtx(original._fromExpCtx->copyWith(_resolvedNs, original._fromExpCtx->uuid)),
      _hasExplicitCollation(original._hasExplicitCollation),
      _resolvedPipeline(original._resolvedPipeline),
      _userPipeline(original._userPipeline),
      _resolvedIntrospectionPipeline(original._resolvedIntrospectionPipeline->clone()),
      _letVariables(original._letVariables) {
    if (!_localField && !_foreignField) {
        _cache.emplace(internalDocumentSourceCursorBatchSizeBytes.load());
    }
    if (original._matchSrc) {
        _matchSrc = static_cast<DocumentSourceMatch*>(original._matchSrc->clone().get());
    }
    if (original._unwindSrc) {
        _unwindSrc = static_cast<DocumentSourceUnwind*>(original._unwindSrc->clone().get());
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
                // TODO SERVER-59628 We should be able to check for any valid data source here, not
                // just $documents.
                !pipeline[0].getField(DocumentSourceDocuments::kStageName).eoo());
}

void validateLookupCollectionlessPipeline(const BSONElement& pipeline) {
    uassert(ErrorCodes::FailedToParse, "must specify 'pipeline' when 'from' is empty", pipeline);
    auto parsedPipeline = parsePipelineFromBSON(pipeline);
    validateLookupCollectionlessPipeline(parsedPipeline);
}

std::unique_ptr<DocumentSourceLookUp::LiteParsed> DocumentSourceLookUp::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $lookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    auto pipelineElem = specObj["pipeline"];
    NamespaceString fromNss;
    if (!fromElement) {
        validateLookupCollectionlessPipeline(pipelineElem);
        fromNss = NamespaceString::makeCollectionlessAggregateNSS(nss.dbName());
    } else {
        fromNss = parseLookupFromAndResolveNamespace(fromElement, nss.dbName());
    }
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $lookup namespace: " << fromNss.ns(),
            fromNss.isValid());

    // Recursively lite parse the nested pipeline, if one exists.
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (pipelineElem) {
        auto pipeline = parsePipelineFromBSON(pipelineElem);
        liteParsedPipeline = LiteParsedPipeline(fromNss, pipeline);
    }

    bool hasInternalCollation = static_cast<bool>(specObj["_internalCollation"]);

    return std::make_unique<DocumentSourceLookUp::LiteParsed>(
        spec.fieldName(), std::move(fromNss), std::move(liteParsedPipeline), hasInternalCollation);
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
            &requiredPrivileges,
            std::move(pipeline.requiredPrivileges(isMongos, bypassDocumentValidation)));
    }

    return requiredPrivileges;
}

REGISTER_DOCUMENT_SOURCE(lookup,
                         DocumentSourceLookUp::LiteParsed::parse,
                         DocumentSourceLookUp::createFromBson,
                         AllowedWithApiStrict::kConditionally);

const char* DocumentSourceLookUp::getSourceName() const {
    return kStageName.rawData();
}

bool DocumentSourceLookUp::foreignShardedLookupAllowed() const {
    return !pExpCtx->opCtx->inMultiDocumentTransaction();
}

void DocumentSourceLookUp::determineSbeCompatibility() {
    _sbeCompatible =
        // This stage is SBE-compatible only if the context is compatible.
        pExpCtx->sbeCompatible
        // We currently only support lowering equi-join that uses localField/foreignField
        // syntax.
        && !_userPipeline && _localField &&
        _foreignField
        // SBE doesn't support match-like paths with numeric components. (Note: "as" field is a
        // project-like field and numbers in it are treated as literal names of fields rather
        // than indexes into arrays, which is compatible with SBE.)
        && !FieldRef(_localField->fullPath()).hasNumericPathComponents() &&
        !FieldRef(_foreignField->fullPath()).hasNumericPathComponents()
        // Setting a collator on an individual $lookup stage with _internalCollation isn't supported
        && !_hasExplicitCollation
        // We currently don't lower $lookup against views ('_fromNs' does not correspond to a
        // view).
        && pExpCtx->getResolvedNamespace(_fromNs).pipeline.empty();
}

StageConstraints DocumentSourceLookUp::constraints(Pipeline::SplitState pipeState) const {
    HostTypeRequirement hostRequirement;
    if (_fromNs.isConfigDotCacheDotChunks()) {
        // $lookup from config.cache.chunks* namespaces is permitted to run on each individual
        // shard, rather than just the primary, since each shard should have an identical copy of
        // the namespace.
        hostRequirement = HostTypeRequirement::kAnyShard;
    } else if (pipeState == Pipeline::SplitState::kSplitForShards) {
        // This stage will only be on the shards pipeline if $lookup on sharded foreign collections
        // is allowed.
        hostRequirement = HostTypeRequirement::kAnyShard;
    } else {
        // If the pipeline is unsplit or this stage is on the merging part of the pipeline,
        // when $lookup on sharded foreign collections is allowed, the foreign collection is
        // sharded, and the stage is executing on mongos, the stage can run on mongos or any shard.
        hostRequirement = (foreignShardedLookupAllowed() && pExpCtx->inMongos &&
                           pExpCtx->mongoProcessInterface->isSharded(pExpCtx->opCtx, _fromNs))
            ? HostTypeRequirement::kNone
            : HostTypeRequirement::kPrimaryShard;
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
            _resolvedIntrospectionPipeline->getSources(), constraints);
    }

    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = !_unwindSrc;

    return constraints;
}

DocumentSource::GetNextResult DocumentSourceLookUp::doGetNext() {
    if (_unwindSrc) {
        return unwindResult();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    auto inputDoc = nextInput.releaseDocument();

    // If we have not absorbed a $unwind, we cannot absorb a $match. If we have absorbed a $unwind,
    // '_unwindSrc' would be non-null, and we would not have made it here.
    invariant(!_matchSrc);

    if (hasLocalFieldForeignFieldJoin()) {
        auto matchStage =
            makeMatchStageFromInput(inputDoc, *_localField, _foreignField->fullPath(), BSONObj());
        // We've already allocated space for the trailing $match stage in '_resolvedPipeline'.
        _resolvedPipeline[*_fieldMatchPipelineIdx] = matchStage;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
    try {
        pipeline = buildPipeline(inputDoc);
    } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
        // If lookup on a sharded collection is disallowed and the foreign collection is sharded,
        // throw a custom exception.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>(); staleInfo &&
            staleInfo->getVersionWanted() &&
            staleInfo->getVersionWanted() != ChunkVersion::UNSHARDED()) {
            uassert(3904800,
                    "Cannot run $lookup with a sharded foreign collection in a transaction",
                    foreignShardedLookupAllowed());
        }
        throw;
    }

    std::vector<Value> results;
    long long objsize = 0;
    const auto maxBytes = internalLookupStageIntermediateDocumentMaxSizeBytes.load();

    while (auto result = pipeline->getNext()) {
        long long safeSum = 0;
        bool hasOverflowed = overflow::add(objsize, result->getApproximateSize(), &safeSum);
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll()
                              << " matching pipeline's $lookup stage exceeds " << maxBytes
                              << " bytes",

                !hasOverflowed && objsize <= maxBytes);
        objsize = safeSum;
        results.emplace_back(std::move(*result));
    }

    accumulatePipelinePlanSummaryStats(*pipeline, _stats.planSummaryStats);
    MutableDocument output(std::move(inputDoc));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

std::unique_ptr<Pipeline, PipelineDeleter> DocumentSourceLookUp::buildPipelineFromViewDefinition(
    std::vector<BSONObj> serializedPipeline,
    ExpressionContext::ResolvedNamespace resolvedNamespace) {
    // We don't want to optimize or attach a cursor source here because we need to update
    // _resolvedPipeline so we can reuse it on subsequent calls to getNext(), and we may need to
    // update _fieldMatchPipelineIdx as well in the case of a field join.
    MakePipelineOptions opts;
    opts.optimize = false;
    opts.attachCursorSource = false;
    opts.validator = lookupPipeValidator;

    // Resolve the view definition.
    auto pipeline = Pipeline::makePipelineFromViewDefinition(
        _fromExpCtx, resolvedNamespace, serializedPipeline, opts);

    // Store the pipeline with resolved namespaces so that we only trigger this exception on the
    // first input document.
    _resolvedPipeline = pipeline->serializeToBson();

    // The index of the field join match stage needs to be set to the length of the view
    // pipeline, as it is no longer the first stage in the resolved pipeline.
    if (hasLocalFieldForeignFieldJoin()) {
        _fieldMatchPipelineIdx = resolvedNamespace.pipeline.size();
    }

    // Update the expression context with any new namespaces the resolved pipeline has introduced.
    LiteParsedPipeline liteParsedPipeline(resolvedNamespace.ns, resolvedNamespace.pipeline);
    _fromExpCtx = _fromExpCtx->copyWith(resolvedNamespace.ns, resolvedNamespace.uuid);
    _fromExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

    return pipeline;
}

std::unique_ptr<Pipeline, PipelineDeleter> DocumentSourceLookUp::buildPipeline(
    const Document& inputDoc) {
    // Copy all 'let' variables into the foreign pipeline's expression context.
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());

    // Resolve the 'let' variables to values per the given input document.
    resolveLetVariables(inputDoc, &_fromExpCtx->variables);

    std::unique_ptr<MongoProcessInterface::ScopedExpectUnshardedCollection>
        expectUnshardedCollectionInScope;

    const auto allowForeignShardedColl = foreignShardedLookupAllowed();
    if (!allowForeignShardedColl) {
        // Enforce that the foreign collection must be unsharded for lookup.
        expectUnshardedCollectionInScope =
            _fromExpCtx->mongoProcessInterface->expectUnshardedCollectionInScope(
                _fromExpCtx->opCtx, _fromExpCtx->ns, boost::none);
    }

    // If we don't have a cache, build and return the pipeline immediately.
    if (!_cache || _cache->isAbandoned()) {
        MakePipelineOptions pipelineOpts;
        pipelineOpts.optimize = true;
        pipelineOpts.attachCursorSource = true;
        pipelineOpts.validator = lookupPipeValidator;
        // By default, $lookup doesnt support sharded 'from' collections.
        pipelineOpts.shardTargetingPolicy = allowForeignShardedColl
            ? ShardTargetingPolicy::kAllowed
            : ShardTargetingPolicy::kNotAllowed;
        try {
            return Pipeline::makePipeline(_resolvedPipeline, _fromExpCtx, pipelineOpts);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // This exception returns the information we need to resolve a sharded view. Update the
            // pipeline with the resolved view definition.
            auto pipeline = buildPipelineFromViewDefinition(
                _resolvedPipeline,
                ExpressionContext::ResolvedNamespace{e->getNamespace(), e->getPipeline()});

            LOGV2_DEBUG(3254800,
                        3,
                        "$lookup found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                        "$lookup sub-pipeline: {new_pipe}",
                        logAttrs(e->getNamespace()),
                        "pipeline"_attr = Value(e->getPipeline()),
                        "new_pipe"_attr = _resolvedPipeline);

            // We can now safely optimize and reattempt attaching the cursor source.
            pipeline = Pipeline::makePipeline(_resolvedPipeline, _fromExpCtx, pipelineOpts);

            return pipeline;
        }
    }

    // Construct the basic pipeline without a cache stage. Avoid optimizing here since we need to
    // add the cache first, as detailed below.
    MakePipelineOptions pipelineOpts;
    pipelineOpts.optimize = false;
    pipelineOpts.attachCursorSource = false;
    pipelineOpts.validator = lookupPipeValidator;
    auto pipeline = Pipeline::makePipeline(_resolvedPipeline, _fromExpCtx, pipelineOpts);

    // We can store the unoptimized serialization of the pipeline so that if we need to resolve
    // a sharded view later on, and we have a local-foreign field join, we will need to update
    // metadata tracking the position of this join in the _resolvedPipeline.
    auto serializedPipeline = pipeline->serializeToBson();

    addCacheStageAndOptimize(*pipeline);

    if (!_cache->isServing()) {
        // The cache has either been abandoned or has not yet been built. Attach a cursor.
        auto shardTargetingPolicy = allowForeignShardedColl ? ShardTargetingPolicy::kAllowed
                                                            : ShardTargetingPolicy::kNotAllowed;
        try {
            pipeline = pExpCtx->mongoProcessInterface->attachCursorSourceToPipeline(
                pipeline.release(), shardTargetingPolicy);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // This exception returns the information we need to resolve a sharded view. Update the
            // pipeline with the resolved view definition.
            pipeline = buildPipelineFromViewDefinition(
                serializedPipeline,
                ExpressionContext::ResolvedNamespace{e->getNamespace(), e->getPipeline()});

            // The serialized pipeline does not have a cache stage, so we will add it back to the
            // pipeline here if the cache has not been abandoned.
            if (_cache && !_cache->isAbandoned()) {
                addCacheStageAndOptimize(*pipeline);
            }

            LOGV2_DEBUG(3254801,
                        3,
                        "$lookup found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                        "$lookup sub-pipeline: {new_pipe}",
                        logAttrs(e->getNamespace()),
                        "pipeline"_attr = Value(e->getPipeline()),
                        "new_pipe"_attr = _resolvedPipeline);

            // Try to attach the cursor source again.
            pipeline = pExpCtx->mongoProcessInterface->attachCursorSourceToPipeline(
                pipeline.release(), shardTargetingPolicy);
        }
    }

    // If the cache has been abandoned, release it.
    if (_cache->isAbandoned()) {
        _cache.reset();
    }

    invariant(pipeline);
    return pipeline;
}

void DocumentSourceLookUp::addCacheStageAndOptimize(Pipeline& pipeline) {
    // Add the cache stage at the end and optimize. During the optimization process, the cache will
    // either move itself to the correct position in the pipeline, or will abandon itself if no
    // suitable cache position exists. Do it only if pipeline optimization is enabled, otherwise
    // Pipeline::optimizePipeline() will exit early and correct placement of the cache will not
    // occur.
    if (auto fp = globalFailPointRegistry().find("disablePipelineOptimization");
        fp && fp->shouldFail()) {
        _cache->abandon();
    } else {
        pipeline.addFinalSource(
            DocumentSourceSequentialDocumentCache::create(_fromExpCtx, _cache.get_ptr()));
    }

    pipeline.optimizePipeline();
}

DocumentSource::GetModPathsReturn DocumentSourceLookUp::getModifiedPaths() const {
    OrderedPathSet modifiedPaths{_as.fullPath()};
    if (_unwindSrc) {
        auto pathsModifiedByUnwind = _unwindSrc->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

Pipeline::SourceContainer::iterator DocumentSourceLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If the following stage is $sort and there is no internal $unwind, consider pushing it ahead
    // of $lookup.
    if (!_unwindSrc) {
        itr = tryReorderingWithSort(itr, container);
        if (*itr != this) {
            return itr;
        }
    }

    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    if (nextUnwind && !_unwindSrc && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwindSrc = std::move(nextUnwind);

        // We cannot push absorbed $unwind stages into SBE.
        _sbeCompatible = false;
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
        !CollatorInterface::collatorsMatch(pExpCtx->getCollator(), _fromExpCtx->getCollator())) {
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
        // If 'expression' is the child of a $elemMatch, we cannot internalize the $match. For
        // example, {b: {$elemMatch: {$gt: 1, $lt: 4}}}, where "b" is our "_as" field. This is
        // because there's no way to modify the expression to be a match just on 'b'--we cannot
        // change the path to an empty string, or remove the node entirely.
        if (expression->matchType() == MatchExpression::ELEM_MATCH_VALUE ||
            expression->matchType() == MatchExpression::ELEM_MATCH_OBJECT) {
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

    // We can internalize the $match. This $lookup should already be marked as SBE incompatible
    // because a $match can only be internalized if an $unwind, which is SBE incompatible, was
    // absorbed as well.
    tassert(5843701, "This $lookup cannot be compatible with SBE", !_sbeCompatible);
    if (!_matchSrc) {
        _matchSrc = nextMatch;
    } else {
        // We have already absorbed a $match. We need to join it with 'dependent'.
        _matchSrc->joinMatchWith(nextMatch);
    }

    // Remove the original $match.
    container->erase(std::next(itr));

    // We have internalized a $match, but have not yet computed the descended $match that should
    // be applied to our queries.
    _additionalFilter = DocumentSourceMatch::descendMatchOnPath(
                            _matchSrc->getMatchExpression(), _as.fullPath(), pExpCtx)
                            ->getQuery()
                            .getOwned();

    // Add '_additionalFilter' to '_resolvedPipeline' if there is a pipeline. If there is no
    // pipeline, '_additionalFilter' can safely be added to the local/foreignField $match stage
    // during 'doGetNext()'.
    if (hasPipeline()) {
        auto matchObj = BSON("$match" << *_additionalFilter);
        _resolvedPipeline.push_back(matchObj);
    }

    // There may be further optimization between this $lookup and the new neighbor, so we return an
    // iterator pointing to ourself.
    return itr;
}

bool DocumentSourceLookUp::usedDisk() {
    if (_pipeline)
        _stats.planSummaryStats.usedDisk =
            _stats.planSummaryStats.usedDisk || _pipeline->usedDisk();

    return _stats.planSummaryStats.usedDisk;
}

void DocumentSourceLookUp::doDispose() {
    if (_pipeline) {
        accumulatePipelinePlanSummaryStats(*_pipeline, _stats.planSummaryStats);
        _pipeline->dispose(pExpCtx->opCtx);
        _pipeline.reset();
    }
}

BSONObj DocumentSourceLookUp::makeMatchStageFromInput(const Document& input,
                                                      const FieldPath& localFieldPath,
                                                      const std::string& foreignFieldName,
                                                      const BSONObj& additionalFilter) {
    // Add the 'localFieldPath' of 'input' into 'localFieldList'. If 'localFieldPath' references a
    // field with an array in its path, we may need to join on multiple values, so we add each
    // element to 'localFieldList'.
    BSONArrayBuilder arrBuilder;
    bool containsRegex = false;
    document_path_support::visitAllValuesAtPath(input, localFieldPath, [&](const Value& nextValue) {
        arrBuilder << nextValue;
        if (!containsRegex && nextValue.getType() == BSONType::RegEx) {
            containsRegex = true;
        }
    });

    if (arrBuilder.arrSize() == 0) {
        // Missing values are treated as null.
        arrBuilder << BSONNULL;
    }

    const auto localFieldListSize = arrBuilder.arrSize();
    const auto localFieldList = arrBuilder.arr();

    // We construct a query of one of the following forms, depending on the contents of
    // 'localFieldList'.
    //
    //   {$and: [{<foreignFieldName>: {$eq: <localFieldList[0]>}}, <additionalFilter>]}
    //     if 'localFieldList' contains a single element.
    //
    //   {$and: [{<foreignFieldName>: {$in: [<value>, <value>, ...]}}, <additionalFilter>]}
    //     if 'localFieldList' contains more than one element but doesn't contain any that are
    //     regular expressions.
    //
    //   {$and: [{$or: [{<foreignFieldName>: {$eq: <value>}},
    //                  {<foreignFieldName>: {$eq: <value>}}, ...]},
    //           <additionalFilter>]}
    //     if 'localFieldList' contains more than one element and it contains at least one element
    //     that is a regular expression.

    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.
    BSONObjBuilder match;
    BSONObjBuilder query(match.subobjStart("$match"));

    BSONArrayBuilder andObj(query.subarrayStart("$and"));
    BSONObjBuilder joiningObj(andObj.subobjStart());

    if (localFieldListSize > 1) {
        // A $lookup on an array value corresponds to finding documents in the foreign collection
        // that have a value of any of the elements in the array value, rather than finding
        // documents that have a value equal to the entire array value. These semantics are
        // automatically provided to us by using the $in query operator.
        if (containsRegex) {
            // A regular expression inside the $in query operator will perform pattern matching on
            // any string values. Since we want regular expressions to only match other RegEx types,
            // we write the query as a $or of equality comparisons instead.
            BSONObj orQuery = buildEqualityOrQuery(foreignFieldName, localFieldList);
            joiningObj.appendElements(orQuery);
        } else {
            // { <foreignFieldName> : { "$in" : <localFieldList> } }
            BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
            subObj << "$in" << localFieldList;
            subObj.doneFast();
        }
    } else {
        // { <foreignFieldName> : { "$eq" : <localFieldList[0]> } }
        BSONObjBuilder subObj(joiningObj.subobjStart(foreignFieldName));
        subObj << "$eq" << localFieldList[0];
        subObj.doneFast();
    }

    joiningObj.doneFast();

    BSONObjBuilder additionalFilterObj(andObj.subobjStart());
    additionalFilterObj.appendElements(additionalFilter);
    additionalFilterObj.doneFast();

    andObj.doneFast();

    query.doneFast();
    return match.obj();
}

DocumentSource::GetNextResult DocumentSourceLookUp::unwindResult() {
    const boost::optional<FieldPath> indexPath(_unwindSrc->indexPath());

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_pipeline || !_nextValue) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _input = nextInput.releaseDocument();

        if (hasLocalFieldForeignFieldJoin()) {
            // At this point, if there is a pipeline, '_additionalFilter' was added to the end of
            // '_resolvedPipeline' in doOptimizeAt(). If there is no pipeline, we must add it to the
            // $match stage created here.
            BSONObj filter = hasPipeline() ? BSONObj() : _additionalFilter.value_or(BSONObj());
            auto matchStage =
                makeMatchStageFromInput(*_input, *_localField, _foreignField->fullPath(), filter);
            // We've already allocated space for the trailing $match stage in '_resolvedPipeline'.
            _resolvedPipeline[*_fieldMatchPipelineIdx] = matchStage;
        }

        if (_pipeline) {
            accumulatePipelinePlanSummaryStats(*_pipeline, _stats.planSummaryStats);
            _pipeline->dispose(pExpCtx->opCtx);
        }

        _pipeline = buildPipeline(*_input);

        // The $lookup stage takes responsibility for disposing of its Pipeline, since it will
        // potentially be used by multiple OperationContexts, and the $lookup stage is part of an
        // outer Pipeline that will propagate dispose() calls before being destroyed.
        _pipeline.get_deleter().dismissDisposal();

        _cursorIndex = 0;
        _nextValue = _pipeline->getNext();

        if (_unwindSrc->preserveNullAndEmptyArrays() && !_nextValue) {
            // There were no results for this cursor, but the $unwind was asked to preserve empty
            // arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly create objects in the prefix of '_as', to act as if we had
            // created an empty array and then removed it.
            output.setNestedField(_as, Value());
            if (indexPath) {
                output.setNestedField(*indexPath, Value(BSONNULL));
            }
            return output.freeze();
        }
    }

    invariant(bool(_input) && bool(_nextValue));
    auto currentValue = *_nextValue;
    _nextValue = _pipeline->getNext();

    // Move input document into output if this is the last or only result, otherwise perform a copy.
    MutableDocument output(_nextValue ? *_input : std::move(*_input));
    output.setNestedField(_as, Value(currentValue));

    if (indexPath) {
        output.setNestedField(*indexPath, Value(_cursorIndex));
    }

    ++_cursorIndex;
    return output.freeze();
}

void DocumentSourceLookUp::resolveLetVariables(const Document& localDoc, Variables* variables) {
    invariant(variables);

    for (auto& letVar : _letVariables) {
        auto value = letVar.expression->evaluate(localDoc, &pExpCtx->variables);
        variables->setConstantValue(letVar.id, value);
    }
}

void DocumentSourceLookUp::initializeResolvedIntrospectionPipeline() {
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());
    _fromExpCtx->startExpressionCounters();
    _resolvedIntrospectionPipeline =
        Pipeline::parse(_resolvedPipeline, _fromExpCtx, lookupPipeValidator);
    _fromExpCtx->stopExpressionCounters();
}

void DocumentSourceLookUp::appendSpecificExecStats(MutableDocument& doc) const {
    const PlanSummaryStats& stats = _stats.planSummaryStats;
    doc["totalDocsExamined"] = Value(static_cast<long long>(stats.totalDocsExamined));
    doc["totalKeysExamined"] = Value(static_cast<long long>(stats.totalKeysExamined));
    doc["collectionScans"] = Value(stats.collectionScans);
    std::vector<Value> indexesUsedVec;
    std::transform(stats.indexesUsed.begin(),
                   stats.indexesUsed.end(),
                   std::back_inserter(indexesUsedVec),
                   [](std::string idx) -> Value { return Value(idx); });
    doc["indexesUsed"] = Value{std::move(indexesUsedVec)};
}

void DocumentSourceLookUp::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {

    // Support alternative $lookup from config.cache.chunks* namespaces.
    //
    // Do not include the tenantId in serialized 'from' namespace.
    auto fromValue = (pExpCtx->ns.db() == _fromNs.db())
        ? Value(_fromNs.coll())
        : Value(Document{{"db", _fromNs.dbName().db()}, {"coll", _fromNs.coll()}});

    MutableDocument output(
        Document{{getSourceName(), Document{{"from", fromValue}, {"as", _as.fullPath()}}}});

    if (hasLocalFieldForeignFieldJoin()) {
        output[getSourceName()]["localField"] = Value(_localField->fullPath());
        output[getSourceName()]["foreignField"] = Value(_foreignField->fullPath());
    }

    // Add a pipeline field if only-pipeline syntax was used (to ensure the output is valid $lookup
    // syntax) or if a $match was absorbed.
    auto pipeline = _userPipeline.get_value_or(std::vector<BSONObj>());
    if (_additionalFilter) {
        pipeline.emplace_back(BSON("$match" << *_additionalFilter));
    }
    if (!hasLocalFieldForeignFieldJoin() || pipeline.size() > 0) {
        MutableDocument exprList;
        for (auto letVar : _letVariables) {
            exprList.addField(letVar.name,
                              letVar.expression->serialize(static_cast<bool>(explain)));
        }
        output[getSourceName()]["let"] = Value(exprList.freeze());

        output[getSourceName()]["pipeline"] = Value(pipeline);
    }

    if (_hasExplicitCollation) {
        output[getSourceName()]["_internalCollation"] = Value(_fromExpCtx->getCollatorBSON());
    }

    if (explain) {
        if (_unwindSrc) {
            const boost::optional<FieldPath> indexPath = _unwindSrc->indexPath();
            output[getSourceName()]["unwinding"] =
                Value(DOC("preserveNullAndEmptyArrays"
                          << _unwindSrc->preserveNullAndEmptyArrays() << "includeArrayIndex"
                          << (indexPath ? Value(indexPath->fullPath()) : Value())));
        }

        if (explain.get() >= ExplainOptions::Verbosity::kExecStats) {
            appendSpecificExecStats(output);
        }

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
        invariant(_resolvedIntrospectionPipeline);

        // We are not attempting to enforce that any referenced metadata are in fact unavailable,
        // this is done elsewhere. We only need to know what variable dependencies exist in the
        // subpipeline for the top-level pipeline. So without knowledge of what metadata is in fact
        // unavailable, we "lie" and say that all metadata is available to avoid tripping any
        // assertions.
        DepsTracker subDeps(DepsTracker::kNoMetadata);

        // Get the subpipeline dependencies. Subpipeline stages may reference both 'let' variables
        // declared by this $lookup and variables declared externally.
        for (auto&& source : _resolvedIntrospectionPipeline->getSources()) {
            source->getDependencies(&subDeps);
        }

        // Add the 'let' dependencies to the tracker. Because the caller is only interested in
        // references to external variables, filter out any subpipeline references to 'let'
        // variables declared by this $lookup.
        for (auto&& letVar : _letVariables) {
            letVar.expression->addDependencies(deps);
            subDeps.vars.erase(letVar.id);
        }

        // Add sub-pipeline variable dependencies. Do not add field dependencies, since these refer
        // to the fields from the foreign collection rather than the local collection.
        // Similarly, do not add SEARCH_META as a dependency, since it is scoped to one pipeline.
        for (auto&& varId : subDeps.vars) {
            if (varId != Variables::kSearchMetaId)
                deps->vars.insert(varId);
        }
    }

    if (hasLocalFieldForeignFieldJoin()) {
        deps->fields.insert(_localField->fullPath());
    }

    // Purposely ignore '_matchSrc' and '_unwindSrc', since those should only be absorbed if we know
    // they are only operating on the "as" field which will be generated by this stage.

    return DepsTracker::State::SEE_NEXT;
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceLookUp::distributedPlanLogic() {
    // If $lookup into a sharded foreign collection is allowed and the foreign namespace is sharded,
    // top-level $lookup stages can run in parallel on the shards.
    //
    // Note that this decision is inherently racy and subject to become stale. This is okay because
    // either choice will work correctly; we are simply applying a heuristic optimization.
    if (foreignShardedLookupAllowed() && pExpCtx->subPipelineDepth == 0 &&
        pExpCtx->mongoProcessInterface->isSharded(_fromExpCtx->opCtx, _fromNs)) {
        return boost::none;
    }

    if (_fromExpCtx->ns.isConfigDotCacheDotChunks()) {
        // When $lookup reads from config.cache.chunks.* namespaces, it should run on each
        // individual shard in parallel. This is a special case, and atypical for standard $lookup
        // since a full copy of config.cache.chunks.* collections exists on all shards.
        return boost::none;
    }

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{nullptr, this, boost::none};
}

void DocumentSourceLookUp::detachFromOperationContext() {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::detachFromOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->detachFromOperationContext();
        invariant(_fromExpCtx->opCtx == nullptr);
    } else if (_fromExpCtx) {
        _fromExpCtx->opCtx = nullptr;
    }
}

void DocumentSourceLookUp::reattachToOperationContext(OperationContext* opCtx) {
    if (_pipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating '_fromExpCtx->opCtx'.
        _pipeline->reattachToOperationContext(opCtx);
        invariant(_fromExpCtx->opCtx == opCtx);
    } else if (_fromExpCtx) {
        _fromExpCtx->opCtx = opCtx;
    }
}

boost::intrusive_ptr<DocumentSource> DocumentSourceLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            "the $lookup specification must be an Object",
            elem.type() == BSONType::Object);

    NamespaceString fromNs;
    std::string as;

    std::string localField;
    std::string foreignField;

    BSONObj letVariables;
    std::vector<BSONObj> pipeline;
    bool hasPipeline = false;
    bool hasLet = false;
    boost::optional<std::unique_ptr<CollatorInterface>> fromCollator;

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == kPipelineField) {
            pipeline = parsePipelineFromBSON(argument);
            hasPipeline = true;
            continue;
        }

        if (argName == "let"_sd) {
            uassert(ErrorCodes::FailedToParse,
                    str::stream() << "$lookup argument '" << argument
                                  << "' must be an object, is type " << argument.type(),
                    argument.type() == BSONType::Object);
            letVariables = argument.Obj();
            hasLet = true;
            continue;
        }

        if (argName == kFromField) {
            fromNs = parseLookupFromAndResolveNamespace(argument, pExpCtx->ns.dbName());
            continue;
        }

        if (argName == "_internalCollation"_sd) {
            const auto& collationSpec = argument.Obj();
            if (!collationSpec.isEmpty()) {
                fromCollator.emplace(uassertStatusOK(
                    CollatorFactoryInterface::get(pExpCtx->opCtx->getServiceContext())
                        ->makeFromBSON(collationSpec)));
            }
            continue;
        }

        uassert(ErrorCodes::FailedToParse,
                str::stream() << "$lookup argument '" << argName << "' must be a string, found "
                              << argument << ": " << argument.type(),
                argument.type() == BSONType::String);

        if (argName == kAsField) {
            as = argument.String();
        } else if (argName == kLocalField) {
            localField = argument.String();
        } else if (argName == kForeignField) {
            foreignField = argument.String();
        } else {
            uasserted(ErrorCodes::FailedToParse,
                      str::stream() << "unknown argument to $lookup: " << argument.fieldName());
        }
    }

    if (fromNs.ns().empty()) {
        validateLookupCollectionlessPipeline(pipeline);
        fromNs = NamespaceString::makeCollectionlessAggregateNSS(pExpCtx->ns.dbName());
    }
    uassert(ErrorCodes::FailedToParse, "must specify 'as' field for a $lookup", !as.empty());

    boost::intrusive_ptr<DocumentSourceLookUp> lookupStage = nullptr;
    if (hasPipeline) {
        if (localField.empty() && foreignField.empty()) {
            // $lookup specified with only pipeline syntax.
            lookupStage = new DocumentSourceLookUp(std::move(fromNs),
                                                   std::move(as),
                                                   std::move(pipeline),
                                                   std::move(letVariables),
                                                   std::move(fromCollator),
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
                                         std::move(fromCollator),
                                         std::pair(std::move(localField), std::move(foreignField)),
                                         pExpCtx);
        }
    } else {
        // $lookup specified with only local/foreignField syntax.
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
                                               std::move(fromCollator),
                                               pExpCtx);
    }
    lookupStage->determineSbeCompatibility();
    return lookupStage;
}

void DocumentSourceLookUp::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_resolvedNs);
    for (auto&& stage : _resolvedIntrospectionPipeline->getSources()) {
        stage->addInvolvedCollections(collectionNames);
    }
}

}  // namespace mongo
