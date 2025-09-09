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

#include "mongo/db/exec/agg/graph_lookup_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

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
                                                       graphLookupDS->_fromPipeline,
                                                       graphLookupDS->_unwind,
                                                       graphLookupDS->_variables,
                                                       graphLookupDS->_variablesParseState);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(graphLookup,
                           DocumentSourceGraphLookUp::id,
                           documentSourceGraphLookUpToStageFn);

GraphLookUpStage::GraphLookUpStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    GraphLookUpParams params,
    boost::intrusive_ptr<ExpressionContext> fromExpCtx,
    std::vector<BSONObj> fromPipeline,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwind,
    Variables variables,
    VariablesParseState variablesParseState)
    : Stage(stageName, pExpCtx),
      _params(params),
      _fromExpCtx(std::move(fromExpCtx)),
      _fromPipeline(std::move(fromPipeline)),
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
      _cache(pExpCtx->getValueComparator()) {};

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

Document GraphLookUpStage::getExplainOutput(const SerializationOptions& opts) const {
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
}

void GraphLookUpStage::spill(int64_t maximumMemoryUsage) {
    const auto& needToSpill = [&]() {
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
    const boost::optional<FieldPath> indexPath((*_unwind)->indexPath());

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

    Value startingValue = _params.startWith->evaluate(*_input, &pExpCtx->variables);

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
            staleInfo->getVersionWanted() != ShardVersion::UNSHARDED()) {
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
    while (!_queue.empty()) {
        std::unique_ptr<MongoProcessInterface::ScopedExpectUnshardedCollection>
            expectUnshardedCollectionInScope;

        const auto allowForeignSharded = foreignShardedGraphLookupAllowed();
        if (!allowForeignSharded && !_fromExpCtx->getInRouter()) {
            // Enforce that the foreign collection must be unsharded for $graphLookup.
            expectUnshardedCollectionInScope =
                _fromExpCtx->getMongoProcessInterface()->expectUnshardedCollectionInScope(
                    _fromExpCtx->getOperationContext(),
                    _fromExpCtx->getNamespaceString(),
                    boost::none);
        }

        // Check whether next key in the queue exists in the cache or needs to be queried until
        // queue is empty or the query is too big.
        auto query = makeQueryFromQueue();

        // Process cached values, populating '_queue' for the next iteration of search.
        while (!query.cached.empty()) {
            auto doc = *query.cached.begin();
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
    if (_memoryUsageTracker.withinMemoryLimit()) {
        _cache.evictDownTo(_memoryUsageTracker.maxAllowedMemoryUsageBytes() -
                           _memoryUsageTracker.inUseTrackedMemoryBytes());
    } else {
        spill(_memoryUsageTracker.maxAllowedMemoryUsageBytes());
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
            if (queried.find(connectToValue) != queried.end()) {
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

    // Add the 'connectFromField' of 'result' into '_queue'. If the 'connectFromField' is an
    // array, we treat it as connecting to multiple values, so we must add each element to
    // '_queue'.
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
    Query result = {
        boost::none,
        pExpCtx->getDocumentComparator().makeUnorderedDocumentSet(),
        pExpCtx->getValueComparator().makeFlatUnorderedValueSet(),
        kUninitializedDepth,
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
    MutableDocument document;
    document.addField(kFrontierValueField, std::move(value));
    document.addField(kDepthField, Value{depth});
    return document.freeze();
}

bool GraphLookUpStage::foreignShardedGraphLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !pExpCtx->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

std::unique_ptr<mongo::Pipeline> GraphLookUpStage::makePipeline(BSONObj match,
                                                                bool allowForeignSharded) {
    // We've already allocated space for the trailing $match stage in '_fromPipeline'.
    _fromPipeline.back() = std::move(match);
    MakePipelineOptions pipelineOpts;
    pipelineOpts.optimize = true;
    pipelineOpts.attachCursorSource = true;
    // By default, $graphLookup doesn't support a sharded 'from' collection.
    pipelineOpts.shardTargetingPolicy =
        allowForeignSharded ? ShardTargetingPolicy::kAllowed : ShardTargetingPolicy::kNotAllowed;
    _variables.copyToExpCtx(_variablesParseState, _fromExpCtx.get());

    // Query settings are looked up after parsing and therefore are not populated in the
    // '_fromExpCtx' as part of DocumentSourceGraphLookUp constructor. Assign query settings
    // to the '_fromExpCtx' by copying them from the parent query ExpressionContext.
    _fromExpCtx->setQuerySettingsIfNotPresent(pExpCtx->getQuerySettings());

    std::unique_ptr<mongo::Pipeline> pipeline;
    try {
        return mongo::Pipeline::makePipeline(_fromPipeline, _fromExpCtx, pipelineOpts);
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
        // This exception returns the information we need to resolve a sharded view. Update
        // the pipeline with the resolved view definition, but don't optimize or attach the
        // cursor source yet.
        MakePipelineOptions opts;
        opts.optimize = false;
        opts.attachCursorSource = false;
        pipeline = mongo::Pipeline::makePipelineFromViewDefinition(
            _fromExpCtx,
            ResolvedNamespace{e->getNamespace(), e->getPipeline()},
            _fromPipeline,
            opts,
            _params.from);

        // Update '_fromPipeline' with the resolved view definition to avoid triggering this
        // exception next time.
        _fromPipeline = std::vector<BSONObj>(pipeline->serializeToBson());

        // Update the expression context with any new namespaces the resolved pipeline has
        // introduced.
        LiteParsedPipeline liteParsedPipeline(e->getNamespace(), e->getPipeline());
        _fromExpCtx = makeCopyFromExpressionContext(_fromExpCtx, e->getNamespace());
        _fromExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

        LOGV2_DEBUG(5865400,
                    3,
                    "$graphLookup found view definition. ns: {namespace}, pipeline: {pipeline}. "
                    "New $graphLookup sub-pipeline: {new_pipe}",
                    logAttrs(e->getNamespace()),
                    "pipeline"_attr =
                        mongo::Pipeline::serializePipelineForLogging(e->getPipeline()),
                    "new_pipe"_attr = mongo::Pipeline::serializePipelineForLogging(_fromPipeline));

        // We can now safely optimize and reattempt attaching the cursor source.
        return mongo::Pipeline::makePipeline(_fromPipeline, _fromExpCtx, pipelineOpts);
    }
}

}  // namespace exec::agg
}  // namespace mongo
