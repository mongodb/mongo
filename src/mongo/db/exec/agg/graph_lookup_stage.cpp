// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/graph_lookup_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_desugarer.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/resolved_namespace.h"  // IWYU pragma: keep
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec::agg {
MONGO_FAIL_POINT_DEFINE(graphLookupStageKickbackFailpoint);
}  // namespace mongo::exec::agg

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceGraphLookUpToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* graphLookupDS = dynamic_cast<DocumentSourceGraphLookUp*>(documentSource.get());

    tassert(10424200, "expected 'DocumentSourceGraphLookUp' type", graphLookupDS);

    // TODO: SERVER-105521 Check if we can std::move '_variables' and '_variablesParseState' instead
    // of copy.
    return make_intrusive<exec::agg::GraphLookUpStage>(graphLookupDS->kStageName,
                                                       graphLookupDS->getExpCtx(),
                                                       graphLookupDS->_params,
                                                       graphLookupDS->_fromExpCtx,
                                                       graphLookupDS->_unwind,
                                                       graphLookupDS->_variables,
                                                       graphLookupDS->_variablesParseState);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(graphLookup,
                           DocumentSourceGraphLookUp::id,
                           documentSourceGraphLookUpToStageFn);

GraphLookUpStage::GraphLookUpStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    GraphLookUpParams params,
    boost::intrusive_ptr<ExpressionContext> fromExpCtx,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwind,
    Variables variables,
    VariablesParseState variablesParseState)
    : Stage(stageName, pExpCtx),
      _params(std::move(params)),
      _fromExpCtx(std::move(fromExpCtx)),
      _unwind(std::move(unwind)),
      _variables(std::move(variables)),
      _variablesParseState(std::move(variablesParseState)),
      _memoryUsageTracker(OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
          *pExpCtx,
          pExpCtx->getAllowDiskUse(),
          loadMemoryLimit(StageMemoryLimit::DocumentSourceGraphLookupMaxMemoryBytes))),
      _queue(pExpCtx.get(), &_memoryUsageTracker),
      _visitedDocuments(pExpCtx.get(), &_memoryUsageTracker, "VisitedDocumentsMap"),
      _visitedFromValues(pExpCtx.get(), &_memoryUsageTracker, "VisitedFromValuesSet"),
      _cache(pExpCtx->getValueComparator()) {
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() &&
        feature_flags::gFeatureFlagExpressionMemoryTracking.isEnabled()) {
        _expressionEvalCtx.tracker = &_memoryUsageTracker["expressionEvaluation"];
    }
    _expressionEvalCtx.stageName = _commonStats.stageTypeStr;
}

GraphLookUpStage::~GraphLookUpStage() {
    const SpillingStats& stats = _stats.spillingStats;
    graphLookupCounters.incrementPerSpilling(stats.getSpills(),
                                             stats.getSpilledBytes(),
                                             stats.getSpilledRecords(),
                                             stats.getSpilledDataStorageSize());
}

void GraphLookUpStage::detachFromOperationContext() {
    _fromExpCtx->setOperationContext(nullptr);
}

void GraphLookUpStage::reattachToOperationContext(OperationContext* opCtx) {
    _fromExpCtx->setOperationContext(opCtx);
}

bool GraphLookUpStage::validateOperationContext(const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        _fromExpCtx->getOperationContext() == opCtx;
}

Document GraphLookUpStage::getExplainOutput(const query_shape::SerializationOptions& opts) const {
    auto out = MutableDocument(Stage::getExplainOutput(opts));
    out["usedDisk"] = opts.serializeLiteral(_stats.spillingStats.getSpills() > 0);
    out["spills"] = opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpills()));
    out["spilledDataStorageSize"] = opts.serializeLiteral(
        static_cast<long long>(_stats.spillingStats.getSpilledDataStorageSize()));
    out["spilledBytes"] =
        opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledBytes()));
    out["spilledRecords"] =
        opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledRecords()));
    if (feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        out["peakTrackedMemBytes"] =
            opts.serializeLiteral(static_cast<long long>(_stats.maxMemoryUsageBytes));
        if (_expressionEvalCtx.tracker) {
            out["expressionEvaluationPeakMemoryBytes"] = opts.serializeLiteral(
                static_cast<long long>(_expressionEvalCtx.tracker->peakTrackedMemoryBytes()));
        }
    }

    return out.freeze();
}

GetNextResult GraphLookUpStage::doGetNext() {
    if (_unwind) {
        return getNextUnwound();
    }

    // We aren't handling a $unwind, process the input document normally.
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        if (input.isEOF()) {
            dispose();
        }
        return input;
    }

    _input = input.releaseDocument();

    performSearch();

    const size_t maxOutputSize =
        static_cast<size_t>(internalGraphLookupStageIntermediateDocumentMaxSizeBytes.load());
    size_t totalSize = sizeof(Value) * _visitedDocuments.size();

    const auto& uassertTotalSize = [&]() {
        uassert(8442700,
                str::stream() << "Total size of the output document exceeds " << maxOutputSize
                              << " bytes. Consider using $unwind to split the output.",
                totalSize <= maxOutputSize);
    };

    uassertTotalSize();
    std::vector<Value> results;
    results.reserve(_visitedDocuments.size());
    for (auto it = _visitedDocuments.begin(); it != _visitedDocuments.end();
         _visitedDocuments.eraseIfInMemoryAndAdvance(it)) {
        totalSize += it->getApproximateSize();
        uassertTotalSize();
        results.emplace_back(std::move(*it));
    }
    _visitedDocuments.clear();

    MutableDocument output(*_input);
    output.setNestedField(_params.as, Value(std::move(results)));

    return output.freeze();
}

void GraphLookUpStage::doDispose() {
    updateSpillingStats();
    _cache.clear();
    _queue.finalize();
    _visitedDocuments.dispose();
    _visitedFromValues.dispose();
    _queryState.reset();
}

void GraphLookUpStage::spill(int64_t maximumMemoryUsage) {
    const auto needToSpill = [&]() {
        return _memoryUsageTracker.inUseTrackedMemoryBytes() > maximumMemoryUsage;
    };

    if (needToSpill() && _unwindIterator.has_value() &&
        *_unwindIterator != _visitedDocuments.end()) {
        return spillDuringVisitedUnwinding();
    }

    if (needToSpill()) {
        _visitedDocuments.spillToDisk();
    }
    if (needToSpill()) {
        _visitedFromValues.spillToDisk();
    }
    if (needToSpill()) {
        _queue.spillToDisk();
    }

    _cache.evictDownTo(
        needToSpill() ? 0 : maximumMemoryUsage - _memoryUsageTracker.inUseTrackedMemoryBytes());
    updateSpillingStats();
}

GetNextResult GraphLookUpStage::getNextUnwound() {
    const boost::optional<FieldPath>& indexPath((*_unwind)->indexPath());

    // If the unwind is not preserving empty arrays, we might have to process multiple inputs
    // before we get one that will produce an output.
    while (true) {
        if (!_unwindIterator.has_value() || *_unwindIterator == _visitedDocuments.end()) {
            _visitedDocuments.clear();
            // No results are left for the current input, so we should move on to the next one
            // and perform a new search.

            auto input = pSource->getNext();
            if (!input.isAdvanced()) {
                if (input.isEOF()) {
                    dispose();
                }
                return input;
            }

            _input = input.releaseDocument();
            performSearch();
            _outputIndex = 0;
            _unwindIterator = _visitedDocuments.begin();
        }
        MutableDocument unwound(*_input);

        if (_visitedDocuments.empty()) {
            if ((*_unwind)->preserveNullAndEmptyArrays()) {
                // Since "preserveNullAndEmptyArrays" was specified, output a document even
                // though we had no result.
                unwound.setNestedField(_params.as, Value());
                if (indexPath) {
                    unwound.setNestedField(*indexPath, Value(BSONNULL));
                }
            } else {
                // $unwind would not output anything, since the '_as' field would not exist. We
                // should loop until we have something to return.
                continue;
            }
        } else {
            auto& it = *_unwindIterator;
            unwound.setNestedField(_params.as, Value{std::move(*it)});
            if (indexPath) {
                unwound.setNestedField(*indexPath, Value(_outputIndex));
                ++_outputIndex;
            }
            _visitedDocuments.eraseIfInMemoryAndAdvance(it);
        }

        return unwound.freeze();
    }
}

void GraphLookUpStage::performSearch() {
    // Make sure _input is set before calling performSearch().
    invariant(_input);

    Value startingValue =
        _params.startWith->evaluate(*_input, &pExpCtx->variables, _expressionEvalCtx);

    // If _startWith evaluates to an array, treat each value as a separate starting point.
    _queue.clear();
    if (startingValue.isArray()) {
        for (const auto& value : startingValue.getArray()) {
            addFromValueToQueueIfNeeded(value, 0 /*depth*/);
        }
    } else {
        addFromValueToQueueIfNeeded(std::move(startingValue), 0 /*depth*/);
    }

    try {
        doBreadthFirstSearch();
        _visitedFromValues.clear();
    } catch (const ExceptionFor<ErrorCategory::StaleShardVersionError>& ex) {
        // If lookup on a sharded collection is disallowed and the foreign collection is
        // sharded, throw a custom exception.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>(); staleInfo &&
            staleInfo->getVersionWanted() &&
            staleInfo->getVersionWanted() != ShardVersion::UNTRACKED()) {
            uassert(3904801,
                    "Cannot run $graphLookup with a sharded foreign collection in a transaction",
                    foreignShardedGraphLookupAllowed());
        }
        throw;
    }
}

void GraphLookUpStage::updateSpillingStats() {
    _stats.maxMemoryUsageBytes = _memoryUsageTracker.peakTrackedMemoryBytes();
    _stats.spillingStats = _queue.getSpillingStats();
    _stats.spillingStats.accumulate(_visitedDocuments.getSpillingStats());
    _stats.spillingStats.accumulate(_visitedFromValues.getSpillingStats());
}

void GraphLookUpStage::spillDuringVisitedUnwinding() {
    if (_visitedDocuments.hasInMemoryData() && _visitedDocuments.begin() == _unwindIterator) {
        _visitedDocuments.spillToDisk();
        _unwindIterator = _visitedDocuments.begin();
        updateSpillingStats();
    } else {
        _unwindIterator->releaseMemory();
    }
}

void GraphLookUpStage::doBreadthFirstSearch() {
    const auto allowForeignSharded = foreignShardedGraphLookupAllowed();

    std::unique_ptr<MongoProcessInterface::ScopedExpectUntrackedCollection>
        expectUntrackedCollectionInScope;

    if (!allowForeignSharded && !_fromExpCtx->getInRouter()) {
        // Enforce that the foreign collection must be unsharded for $graphLookup.
        expectUntrackedCollectionInScope =
            _fromExpCtx->getMongoProcessInterface()->expectUntrackedCollectionInScope(
                _fromExpCtx->getOperationContext(), _fromExpCtx->getNamespaceString(), boost::none);
    }

    while (!_queue.empty()) {
        // Check whether next key in the queue exists in the cache or needs to be queried until
        // queue is empty or the query is too big.
        auto query = makeQueryFromQueue();

        // Process cached values, populating '_queue' for the next iteration of search.
        while (!query.cached.empty()) {
            auto doc = std::move(*query.cached.begin());
            query.cached.erase(query.cached.begin());
            addToVisitedAndQueue(std::move(doc), query.depth);
            checkMemoryUsage();
        }

        if (query.match) {
            // Query for all keys that were in the frontier and not in the cache, populating
            // '_queue' for the next iteration of search.
            auto pipeline = makePipeline(std::move(*query.match), allowForeignSharded);
            auto execPipeline = buildPipeline(pipeline->freeze());
            while (auto next = execPipeline->getNext()) {
                uassert(40271,
                        str::stream()
                            << "Documents in the '" << _params.from.toStringForErrorMsg()
                            << "' namespace must contain an _id for de-duplication in $graphLookup",
                        !(*next)["_id"].missing());

                addToVisitedAndQueue(*next, query.depth);
                addToCache(*next, query.queried);
            }
            checkMemoryUsage();
            execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);
        }
    }
    updateSpillingStats();
}

void GraphLookUpStage::checkMemoryUsage() {
    if (_memoryUsageTracker.withinMemoryLimit(getContext()->getOperationContext())) {
        _cache.evictDownTo(
            _memoryUsageTracker.maxAllowedMemoryUsageBytes(getContext()->getOperationContext()) -
            _memoryUsageTracker.inUseTrackedMemoryBytes());
    } else {
        spill(_memoryUsageTracker.maxAllowedMemoryUsageBytes(getContext()->getOperationContext()));
    }
}

void GraphLookUpStage::addToCache(const Document& result, const ValueFlatUnorderedSet& queried) {
    document_path_support::visitAllValuesAtPath(
        result, _params.connectToField, [this, &queried, &result](const Value& connectToValue) {
            // It is possible that 'connectToValue' is a single value, but was not queried for.
            // For instance, with a connectToField of "a.b" and a document with the structure:
            // {a: [{b: 1}, {b: 0}]}, this document will be retrieved by querying for "{b: 1}",
            // but the outer for loop will split this into two separate connectToValues. {b: 0}
            // was not queried for, and thus, we cannot cache under it.
            if (queried.contains(connectToValue)) {
                _cache.insert(connectToValue, result);
            }
        });
}

void GraphLookUpStage::addToVisitedAndQueue(Document result, long long depth) {
    auto id = result.getField("_id");

    if (_visitedDocuments.contains(id)) {
        // We've already seen this object, don't repeat any work.
        return;
    }

    // We have not seen this node before. If '_depthField' was specified, add the field to the
    // object.
    if (_params.depthField) {
        MutableDocument mutableDoc(std::move(result));
        mutableDoc.setNestedField(*_params.depthField, Value(depth));
        result = mutableDoc.freeze();
    }

    _visitedDocuments.add(result);

    // Add the 'connectFromField' of 'result' into '_queue'. If the 'connectFromField' is an array,
    // we treat it as connecting to multiple values, so we must add each element to '_queue'.
    if (!_params.maxDepth || depth < *_params.maxDepth) {
        document_path_support::visitAllValuesAtPath(
            result, _params.connectFromField, [&](const Value& nextFrontierValue) {
                addFromValueToQueueIfNeeded(nextFrontierValue, depth + 1);
            });
    }
}

void GraphLookUpStage::addFromValueToQueueIfNeeded(Value fromValue, long long depth) {
    if (!_visitedFromValues.contains(fromValue)) {
        _queue.addDocument(wrapFrontierValue(fromValue, depth));
        _visitedFromValues.add(std::move(fromValue));
    }
}

auto GraphLookUpStage::makeQueryFromQueue() -> Query {
    static constexpr long long kUninitializedDepth = -1;

    // Recycle the containers in the '_queryState' instance across all the queries that are
    // performed.
    if (_queryState) {
        // '_queryState' is already initialized, thus clear out the existing containers.
        _queryState->clear();
    } else {
        // No '_queryState' exists yet. Create it with the necessary containers for the environment.
        _queryState.emplace(pExpCtx->getDocumentComparator().makeUnorderedDocumentSet(),
                            pExpCtx->getValueComparator().makeFlatUnorderedValueSet());
    }

    Query result = {
        .match = boost::none,

        // The following two members are references to members of the graph lookup stage. Thus this
        // 'Query' object must never outlive 'this'.
        .cached = _queryState->cached,
        .queried = _queryState->queried,

        .depth = kUninitializedDepth,
    };

    // Create a query of the form {$and: [_additionalFilter, {_connectToField: {$in: [...]}}]}.
    //
    // We wrap the query in a $match so that it can be parsed into a DocumentSourceMatch when
    // constructing a pipeline to execute.

    // $graphLookup and regular $match semantics differ in treatment of null/missing. Regular
    // $match stages may conflate null/missing values. Here, null only matches null.

    // Keep track of whether we see null or missing in the queue.
    bool matchNull = false;
    bool seenMissing = false;

    // Will be set to true if we encounter an uncached document that we need to query.
    bool needToQuery = false;
    BSONObjBuilder match;
    {
        BSONObjBuilder query(match.subobjStart("$match"));
        {
            BSONArrayBuilder andObj(query.subarrayStart("$and"));
            if (_params.additionalFilter) {
                andObj << *_params.additionalFilter;
            }

            {
                BSONObjBuilder connectToObj(andObj.subobjStart());
                {
                    BSONObjBuilder subObj(
                        connectToObj.subobjStart(_params.connectToField.fullPath()));
                    {
                        BSONArrayBuilder in(subObj.subarrayStart("$in"));
                        while (!_queue.empty()) {
                            Document queueDocument = _queue.peekFront();

                            long long queueDepth = queueDocument.getField(kDepthField).getLong();
                            if (result.depth == kUninitializedDepth) {
                                result.depth = queueDepth;
                            } else if (queueDepth != result.depth) {
                                // Only values from the same depth can be queried together.
                                break;
                            }

                            Value value = queueDocument.getField(kFrontierValueField);

                            // Add any cached values to 'cached' and do not extend the query.
                            if (auto entry = _cache[value]) {
                                result.cached.insert(entry->begin(), entry->end());
                                _queue.popFront();
                                continue;
                            }
                            if (match.len() + value.getApproximateSize() > BSONObjMaxUserSize) {
                                uassert(2398001,
                                        "A single lookup value does not fit into "
                                        "BSONObjMaxUserSize",
                                        needToQuery);
                                break;
                            }

                            needToQuery = true;
                            if (value.getType() == BSONType::null) {
                                matchNull = true;
                            } else if (value.missing()) {
                                seenMissing = true;
                            }
                            in << value;
                            result.queried.emplace(std::move(value));
                            _queue.popFront();
                        }
                    }
                }
            }
            // We never want to see documents where the 'connectToField' is missing. Only
            // add a check for it in situations where we might match it accidentally.
            if (matchNull || seenMissing) {
                auto existsMatch =
                    BSON(_params.connectToField.fullPath() << BSON("$exists" << true));
                andObj << existsMatch;
            }
        }
    }

    if (needToQuery) {
        result.match = match.obj();
    }
    return result;
}

Document GraphLookUpStage::wrapFrontierValue(Value value, long long depth) const {
    return Document{{kFrontierValueField, std::move(value)}, {kDepthField, Value{depth}}};
}

bool GraphLookUpStage::foreignShardedGraphLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !pExpCtx->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

namespace {
void tryOptimizeMatchOnlyPipelineDirectly(mongo::Pipeline* pipe) {
    // pipeline_optimization::optimizePipeline() has small overhead compared to optimizing the only
    // stage directly. This overhead can be noticeable when repeated for every document in the
    // result set. Unless executed against a view, the 'from' pipeline will only contain a $match
    // connecting the 'to' and 'from' fields.
    if (DocumentSource* match = pipe->peekFront();
        match && pipe->size() == 1 && match->getId() == DocumentSourceMatch::id) {
        // $match is the only stage, optimize directly.
        if (!static_cast<DocumentSourceMatch&>(*match).optimize()) {
            (void)pipe->popFront();
        }
        pipe->validateCommon(true /* alreadyOptimized */);
    } else {
        // Running on against a view, need to optimize the whole pipeline.
        pipeline_optimization::optimizeAndValidatePipeline(pipe);
    }
}
}  // namespace

std::unique_ptr<mongo::Pipeline> GraphLookUpStage::makePipeline(BSONObj match,
                                                                bool allowForeignSharded) {
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());

    const ShardTargetingPolicy shardTargetingPolicy =
        allowForeignSharded ? ShardTargetingPolicy::kAllowed : ShardTargetingPolicy::kNotAllowed;

    // Attempt pipeline construction. On retry due to a sharded view, _params.fromLpp and
    // _fromExpCtx reflect the resolved definition. A collection may be remapped into a view while
    // the query is executing, leading to subsequent CommandOnShardedViewNotSupportedOnMongod
    // throws. Handle this race condition gracefully until we exhaust retries.
    constexpr int kMaxAttempts = 5;
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        // Combine the view stages from _params.fromLpp (possibly empty for a plain collection)
        // with the per-iteration $match. _params.fromLpp is already desugared at its storage sites
        // (the DocumentSourceGraphLookUp constructor, and the sharded-view rebuild in the catch
        // block below), and the appended $match is not an extension stage, so no desugar is needed
        // here.
        auto lpp = (*_params.fromLpp)->clone();
        lpp.appendStage(LiteParsedDocumentSource::parse(_fromExpCtx->getNamespaceString(), match));

        try {
            // Test-only: force a sharded-view kickback on every attempt so the retry loop runs to
            // exhaustion. The kickback resolves the foreign namespace to itself (empty pipeline) so
            // each attempt re-throws. At the top of the attempt so it fires wherever the stage runs
            // (a shard node or the merging mongos).
            if (MONGO_unlikely(graphLookupStageKickbackFailpoint.shouldFail())) {
                uassertStatusOK(Status(
                    ResolvedNamespace(_fromExpCtx->getNamespaceString(), std::vector<BSONObj>{}),
                    "graphLookupStageKickbackFailpoint forced sharded view kickback"));
            }
            return pExpCtx->getMongoProcessInterface()->finalizeAndMaybePreparePipelineForExecution(
                _fromExpCtx,
                mongo::Pipeline::parseFromLiteParsed(lpp, _fromExpCtx),
                true /* attachCursorAfterOptimizing */,
                tryOptimizeMatchOnlyPipelineDirectly,
                shardTargetingPolicy);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            if (attempt == kMaxAttempts) {
                // Still resolving to a sharded view after every retry (e.g. the view keeps being
                // concurrently remapped). Propagate rather than falling through to the tassert.
                throw;
            }

            // This exception returns the information we need to resolve a sharded view. Rebuild
            // _params.fromLpp from the resolved view definition so subsequent iterations don't
            // re-throw.
            const std::vector<BSONObj>& resolvedViewPipe =
                (e->isTimeseries() && isRawDataOperation(pExpCtx->getOperationContext()))
                ? std::vector<BSONObj>{}
                : e->getBsonPipeline();

            // The exception owns the BSONObjs in resolvedViewPipe; they are freed when 'e' goes
            // out of scope at catch-block exit. Construct _params.fromLpp from those stages now,
            // while they are still valid, so that subsequent makePipeline() calls use the resolved
            // view definition rather than re-throwing.
            LiteParserOptions lppOpts;
            lppOpts.ifrContext = _fromExpCtx->getIfrContext();
            _params.fromLpp.emplace(e->getResolvedNamespace(), resolvedViewPipe, lppOpts);
            _fromExpCtx = makeCopyFromExpressionContext(_fromExpCtx, e->getResolvedNamespace());
            _fromExpCtx->addResolvedNamespaces((*_params.fromLpp)->getInvolvedNamespaces());

            // Preserve the storage-site invariant: _params.fromLpp is always stored desugared.
            LiteParsedDesugarer::desugar(&(**_params.fromLpp), _fromExpCtx->getIfrContext());

            LOGV2_DEBUG(
                5865400,
                3,
                "$graphLookup found view definition. ns: {namespace}, pipeline: {pipeline}. "
                "New $graphLookup sub-pipeline: {new_pipe}",
                logAttrs(e->getResolvedNamespace()),
                "pipeline"_attr =
                    mongo::Pipeline::serializePipelineForLogging(e->getBsonPipeline()),
                "new_pipe"_attr = mongo::Pipeline::serializePipelineForLogging(resolvedViewPipe));
        }
    }
    uasserted(ErrorCodes::CollectionBecameView,
              "view configuration changed too many times during $graphLookup execution");
}

}  // namespace exec::agg
}  // namespace mongo
