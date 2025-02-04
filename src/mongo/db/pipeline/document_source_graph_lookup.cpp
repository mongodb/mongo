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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <iterator>
#include <limits>
#include <list>
#include <memory>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_source_graph_lookup.h"
#include "mongo/db/pipeline/document_source_merge_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers_targeting_policy.h"
#include "mongo/db/pipeline/sort_reorder_helpers.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/bson/dotted_path_support.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

// Parses $graphLookup 'from' field. The 'from' field must be a string.
NamespaceString parseGraphLookupFromAndResolveNamespace(const BSONElement& elem,
                                                        const DatabaseName& defaultDb) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "$graphLookup 'from' field must be a string, but found "
                          << typeName(elem.type()),
            elem.type() == String);

    NamespaceString fromNss(NamespaceStringUtil::deserialize(defaultDb, elem.valueStringData()));
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "invalid $graphLookup namespace: " << fromNss.toStringForErrorMsg(),
            fromNss.isValid());
    return fromNss;
}

}  // namespace

using boost::intrusive_ptr;

std::unique_ptr<DocumentSourceGraphLookUp::LiteParsed> DocumentSourceGraphLookUp::LiteParsed::parse(
    const NamespaceString& nss, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the $graphLookup stage specification must be an object, but found "
                          << typeName(spec.type()),
            spec.type() == BSONType::Object);

    auto specObj = spec.Obj();
    auto fromElement = specObj["from"];
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "missing 'from' option to $graphLookup stage specification: "
                          << specObj,
            fromElement);

    return std::make_unique<LiteParsed>(
        spec.fieldName(), parseGraphLookupFromAndResolveNamespace(fromElement, nss.dbName()));
}

REGISTER_DOCUMENT_SOURCE(graphLookup,
                         DocumentSourceGraphLookUp::LiteParsed::parse,
                         DocumentSourceGraphLookUp::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(graphLookup, DocumentSourceGraphLookUp::id)

const char* DocumentSourceGraphLookUp::getSourceName() const {
    return kStageName.rawData();
}

DocumentSource::GetNextResult DocumentSourceGraphLookUp::doGetNext() {
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
    size_t totalSize = sizeof(Value) * _visited.size();

    const auto& uassertTotalSize = [&]() {
        uassert(8442700,
                str::stream() << "Total size of the output document exceeds " << maxOutputSize
                              << " bytes. Consider using $unwind to split the output.",
                totalSize <= maxOutputSize);
    };

    uassertTotalSize();
    std::vector<Value> results;
    results.reserve(_visited.size());
    for (auto it = _visited.begin(); it != _visited.end(); _visited.eraseIfInMemoryAndAdvance(it)) {
        totalSize += it->getApproximateSize();
        uassertTotalSize();
        results.emplace_back(std::move(*it));
    }
    _visited.clear();

    MutableDocument output(*_input);
    output.setNestedField(_as, Value(std::move(results)));

    invariant(_visited.empty());

    return output.freeze();
}

DocumentSource::GetNextResult DocumentSourceGraphLookUp::getNextUnwound() {
    const boost::optional<FieldPath> indexPath((*_unwind)->indexPath());

    // If the unwind is not preserving empty arrays, we might have to process multiple inputs before
    // we get one that will produce an output.
    while (true) {
        if (_unwindIterator == _visited.end()) {
            _visited.clear();
            // No results are left for the current input, so we should move on to the next one and
            // perform a new search.

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
            _unwindIterator = _visited.begin();
        }
        MutableDocument unwound(*_input);

        if (_visited.empty()) {
            if ((*_unwind)->preserveNullAndEmptyArrays()) {
                // Since "preserveNullAndEmptyArrays" was specified, output a document even though
                // we had no result.
                unwound.setNestedField(_as, Value());
                if (indexPath) {
                    unwound.setNestedField(*indexPath, Value(BSONNULL));
                }
            } else {
                // $unwind would not output anything, since the '_as' field would not exist. We
                // should loop until we have something to return.
                continue;
            }
        } else {
            unwound.setNestedField(_as, Value{std::move(*_unwindIterator)});
            if (indexPath) {
                unwound.setNestedField(*indexPath, Value(_outputIndex));
                ++_outputIndex;
            }
            _visited.eraseIfInMemoryAndAdvance(_unwindIterator);
        }

        return unwound.freeze();
    }
}

void DocumentSourceGraphLookUp::doDispose() {
    _cache.clear();
    _queue.finalize();
    _visited.dispose();
}

boost::optional<ShardId> DocumentSourceGraphLookUp::computeMergeShardId() const {
    // Note that we can only check sharding state when we're on router as we may be holding
    // locks on mongod (which would inhibit looking up sharding state in the catalog cache).
    if (pExpCtx->getInRouter()) {
        // Only nominate a merging shard if the outer collection is unsharded.
        if (!pExpCtx->getMongoProcessInterface()->isSharded(pExpCtx->getOperationContext(),
                                                            pExpCtx->getNamespaceString())) {
            return pExpCtx->getMongoProcessInterface()->determineSpecificMergeShard(
                pExpCtx->getOperationContext(), _from);
        }
    } else {
        auto shardId = ShardingState::get(pExpCtx->getOperationContext())->shardId();
        // If the command is executed on a mongos, we might get an empty shardId. We should return a
        // shardId only if it is valid (non-empty).
        if (shardId.isValid()) {
            return shardId;
        } else {
            return boost::none;
        }
    }
    return boost::none;
}

bool DocumentSourceGraphLookUp::foreignShardedGraphLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !pExpCtx->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceGraphLookUp::distributedPlanLogic() {
    // If $graphLookup into a sharded foreign collection is allowed, top-level $graphLookup
    // stages can run in parallel on the shards.
    if (foreignShardedGraphLookupAllowed() && pExpCtx->getSubPipelineDepth() == 0) {
        if (getMergeShardId()) {
            return DistributedPlanLogic{nullptr, this, boost::none};
        }
        return boost::none;
    }

    // {shardsStage, mergingStage, sortPattern}
    return DistributedPlanLogic{nullptr, this, boost::none};
}

void DocumentSourceGraphLookUp::doBreadthFirstSearch() {
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
            while (auto next = pipeline->getNext()) {
                uassert(40271,
                        str::stream()
                            << "Documents in the '" << _from.toStringForErrorMsg()
                            << "' namespace must contain an _id for de-duplication in $graphLookup",
                        !(*next)["_id"].missing());

                addToVisitedAndQueue(*next, query.depth);
                addToCache(*next, query.queried);
            }
            checkMemoryUsage();
            accumulatePipelinePlanSummaryStats(*pipeline, _stats.planSummaryStats);
        }
        updateSpillingStats();
    }
}

std::unique_ptr<Pipeline, PipelineDeleter> DocumentSourceGraphLookUp::makePipeline(
    BSONObj match, bool allowForeignSharded) {
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

    try {
        return Pipeline::makePipeline(_fromPipeline, _fromExpCtx, pipelineOpts);
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
        // This exception returns the information we need to resolve a sharded view. Update
        // the pipeline with the resolved view definition, but don't optimize or attach the
        // cursor source yet.
        MakePipelineOptions opts;
        opts.optimize = false;
        opts.attachCursorSource = false;
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline =
            Pipeline::makePipelineFromViewDefinition(
                _fromExpCtx,
                ResolvedNamespace{e->getNamespace(), e->getPipeline()},
                _fromPipeline,
                opts);

        // Update '_fromPipeline' with the resolved view definition to avoid triggering this
        // exception next time.
        _fromPipeline = pipeline->serializeToBson();

        // Update the expression context with any new namespaces the resolved pipeline has
        // introduced.
        LiteParsedPipeline liteParsedPipeline(e->getNamespace(), e->getPipeline());
        _fromExpCtx = _fromExpCtx->copyWith(e->getNamespace());
        _fromExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

        LOGV2_DEBUG(5865400,
                    3,
                    "$graphLookup found view definition. ns: {namespace}, pipeline: {pipeline}. "
                    "New $graphLookup sub-pipeline: {new_pipe}",
                    logAttrs(e->getNamespace()),
                    "pipeline"_attr = Value(e->getPipeline()),
                    "new_pipe"_attr = _fromPipeline);

        // We can now safely optimize and reattempt attaching the cursor source.
        return Pipeline::makePipeline(_fromPipeline, _fromExpCtx, pipelineOpts);
    }
}

Document DocumentSourceGraphLookUp::wrapFrontierValue(Value value, long long depth) const {
    MutableDocument document;
    document.addField(kFrontierValueField, std::move(value));
    document.addField(kDepthField, Value{depth});
    return document.freeze();
}

void DocumentSourceGraphLookUp::addToVisitedAndQueue(Document result, long long depth) {
    auto id = result.getField("_id");

    if (_visited.contains(id)) {
        // We've already seen this object, don't repeat any work.
        return;
    }

    // We have not seen this node before. If '_depthField' was specified, add the field to the
    // object.
    if (_depthField) {
        MutableDocument mutableDoc(std::move(result));
        mutableDoc.setNestedField(*_depthField, Value(depth));
        result = mutableDoc.freeze();
    }

    // Add the 'connectFromField' of 'result' into '_queue'. If the 'connectFromField' is an
    // array, we treat it as connecting to multiple values, so we must add each element to
    // '_queue'.
    if (!_maxDepth || depth < *_maxDepth) {
        document_path_support::visitAllValuesAtPath(
            result, _connectFromField, [&](const Value& nextFrontierValue) {
                _queue.addDocument(wrapFrontierValue(nextFrontierValue, depth + 1));
            });
    }

    // Add the object to our '_visited' list and update the size of '_visited' appropriately.
    _visited.add(result);
}

void DocumentSourceGraphLookUp::addToCache(const Document& result,
                                           const ValueFlatUnorderedSet& queried) {
    document_path_support::visitAllValuesAtPath(
        result, _connectToField, [this, &queried, &result](const Value& connectToValue) {
            // It is possible that 'connectToValue' is a single value, but was not queried for. For
            // instance, with a connectToField of "a.b" and a document with the structure:
            // {a: [{b: 1}, {b: 0}]}, this document will be retrieved by querying for "{b: 1}", but
            // the outer for loop will split this into two separate connectToValues. {b: 0} was not
            // queried for, and thus, we cannot cache under it.
            if (queried.find(connectToValue) != queried.end()) {
                _cache.insert(connectToValue, result);
            }
        });
}

auto DocumentSourceGraphLookUp::makeQueryFromQueue() -> Query {
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

    // $graphLookup and regular $match semantics differ in treatment of null/missing. Regular $match
    // stages may conflate null/missing values. Here, null only matches null.

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
            if (_additionalFilter) {
                andObj << *_additionalFilter;
            }

            {
                BSONObjBuilder connectToObj(andObj.subobjStart());
                {
                    BSONObjBuilder subObj(connectToObj.subobjStart(_connectToField.fullPath()));
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
                                uassert(
                                    2398001,
                                    "A single lookup value does not fit into BSONObjMaxUserSize",
                                    needToQuery);
                                break;
                            }

                            needToQuery = true;
                            if (value.getType() == BSONType::jstNULL) {
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
                auto existsMatch = BSON(_connectToField.fullPath() << BSON("$exists" << true));
                andObj << existsMatch;
            }
        }
    }

    if (needToQuery) {
        result.match = match.obj();
    }
    return result;
}

void DocumentSourceGraphLookUp::performSearch() {
    // Make sure _input is set before calling performSearch().
    invariant(_input);

    Value startingValue = _startWith->evaluate(*_input, &pExpCtx->variables);

    // If _startWith evaluates to an array, treat each value as a separate starting point.
    _queue.clear();
    if (startingValue.isArray()) {
        for (const auto& value : startingValue.getArray()) {
            _queue.addDocument(wrapFrontierValue(value, 0 /*depth*/));
        }
    } else {
        _queue.addDocument(wrapFrontierValue(std::move(startingValue), 0 /*depth*/));
    }

    try {
        doBreadthFirstSearch();
    } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
        // If lookup on a sharded collection is disallowed and the foreign collection is sharded,
        // throw a custom exception.
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

DocumentSource::GetModPathsReturn DocumentSourceGraphLookUp::getModifiedPaths() const {
    OrderedPathSet modifiedPaths{_as.fullPath()};
    if (_unwind) {
        auto pathsModifiedByUnwind = _unwind.value()->getModifiedPaths();
        invariant(pathsModifiedByUnwind.type == GetModPathsReturn::Type::kFiniteSet);
        modifiedPaths.insert(pathsModifiedByUnwind.paths.begin(),
                             pathsModifiedByUnwind.paths.end());
    }
    return {GetModPathsReturn::Type::kFiniteSet, std::move(modifiedPaths), {}};
}

StageConstraints DocumentSourceGraphLookUp::constraints(Pipeline::SplitState pipeState) const {
    // $graphLookup can execute on a mongos or a shard, so its host type requirement is
    // 'kNone'. If it needs to execute on a specific merging shard, it can request this
    // later.
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kAllowed,
                                 TransactionRequirement::kAllowed,
                                 LookupRequirement::kAllowed,
                                 UnionRequirement::kAllowed);

    constraints.canSwapWithMatch = true;
    constraints.canSwapWithSkippingOrLimitingStage = !_unwind;

    // If this $graphLookup is on the merging half of the pipeline and the inner collection
    // isn't sharded (that is, it is either unsplittable or untracked), then we should merge
    // on the shard which owns the inner collection.
    if (pipeState == Pipeline::SplitState::kSplitForMerge) {
        constraints.mergeShardId = getMergeShardId();
    }

    return constraints;
}

Pipeline::SourceContainer::iterator DocumentSourceGraphLookUp::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    // If we are not already handling an $unwind stage internally, we can combine with the
    // following $unwind stage.
    auto nextUnwind = dynamic_cast<DocumentSourceUnwind*>((*std::next(itr)).get());
    if (nextUnwind && !_unwind && nextUnwind->getUnwindPath() == _as.fullPath()) {
        _unwind = std::move(nextUnwind);
        container->erase(std::next(itr));
        return itr;
    }

    // If the following stage is $sort and there is no internal $unwind, consider pushing it
    // ahead of $graphLookup.
    if (!_unwind) {
        itr = tryReorderingWithSort(itr, container);
        if (*itr != this) {
            return itr;
        }
    }

    return std::next(itr);
}

void DocumentSourceGraphLookUp::checkMemoryUsage() {
    if (_memoryUsageTracker.withinMemoryLimit()) {
        _cache.evictDownTo(_memoryUsageTracker.maxAllowedMemoryUsageBytes() -
                           _memoryUsageTracker.currentMemoryBytes());
    } else {
        spill(_memoryUsageTracker.maxAllowedMemoryUsageBytes());
    }
}

void DocumentSourceGraphLookUp::spill(int64_t maximumMemoryUsage) {
    const auto& needToSpill = [&]() {
        return _memoryUsageTracker.currentMemoryBytes() > maximumMemoryUsage;
    };
    if (_queue.getApproximateSize() > _visited.getApproximateSize()) {
        _queue.spillToDisk();
        if (needToSpill()) {
            _visited.spillToDisk();
        }
    } else {
        _visited.spillToDisk();
        if (needToSpill()) {
            _queue.spillToDisk();
        }
    }
    _cache.evictDownTo(
        needToSpill() ? 0 : maximumMemoryUsage - _memoryUsageTracker.currentMemoryBytes());
    updateSpillingStats();
}

void DocumentSourceGraphLookUp::updateSpillingStats() {
    _stats.maxMemoryUsageBytes = _memoryUsageTracker.maxMemoryBytes();
    _stats.spillingStats = _queue.getSpillingStats();
    _stats.spillingStats.accumulate(_visited.getSpillingStats());
}

void DocumentSourceGraphLookUp::serializeToArray(std::vector<Value>& array,
                                                 const SerializationOptions& opts) const {
    // Do not include tenantId in serialized 'from' namespace.
    auto fromValue = pExpCtx->getNamespaceString().isEqualDb(_from)
        ? Value(opts.serializeIdentifier(_from.coll()))
        : Value(Document{
              {"db",
               opts.serializeIdentifier(_from.dbName().serializeWithoutTenantPrefix_UNSAFE())},
              {"coll", opts.serializeIdentifier(_from.coll())}});

    // Serialize default options.
    MutableDocument spec(DOC("from" << fromValue << "as" << opts.serializeFieldPath(_as)
                                    << "connectToField" << opts.serializeFieldPath(_connectToField)
                                    << "connectFromField"
                                    << opts.serializeFieldPath(_connectFromField) << "startWith"
                                    << _startWith->serialize(opts)));

    // depthField is optional; serialize it if it was specified.
    if (_depthField) {
        spec["depthField"] = Value(opts.serializeFieldPath(*_depthField));
    }

    if (_maxDepth) {
        spec["maxDepth"] = Value(opts.serializeLiteral(*_maxDepth));
    }

    if (_additionalFilter) {
        if (opts.transformIdentifiers ||
            opts.literalPolicy != LiteralSerializationPolicy::kUnchanged) {
            auto matchExpr =
                uassertStatusOK(MatchExpressionParser::parse(*_additionalFilter, pExpCtx));
            spec["restrictSearchWithMatch"] = Value(matchExpr->serialize(opts));
        } else {
            spec["restrictSearchWithMatch"] = Value(*_additionalFilter);
        }
    }

    // If we are explaining, include an absorbed $unwind inside the $graphLookup
    // specification.
    if (_unwind && opts.verbosity) {
        const boost::optional<FieldPath> indexPath = (*_unwind)->indexPath();
        spec["unwinding"] =
            Value(DOC("preserveNullAndEmptyArrays"
                      << opts.serializeLiteral((*_unwind)->preserveNullAndEmptyArrays())
                      << "includeArrayIndex"
                      << (indexPath ? Value(opts.serializeFieldPath(*indexPath)) : Value())));
    }

    MutableDocument out;
    out[getSourceName()] = spec.freezeToValue();

    if (opts.verbosity && *opts.verbosity >= ExplainOptions::Verbosity::kExecStats) {
        out["usedDisk"] = opts.serializeLiteral(_stats.spillingStats.getSpills() > 0);
        out["spills"] =
            opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpills()));
        out["spilledDataStorageSize"] = opts.serializeLiteral(
            static_cast<long long>(_stats.spillingStats.getSpilledDataStorageSize()));
        out["spilledBytes"] =
            opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledBytes()));
        out["spilledRecords"] =
            opts.serializeLiteral(static_cast<long long>(_stats.spillingStats.getSpilledRecords()));
    }

    array.push_back(out.freezeToValue());

    // If we are not explaining, the output of this method must be parseable, so serialize
    // our $unwind into a separate stage.
    if (_unwind && !opts.verbosity) {
        (*_unwind)->serializeToArray(array, opts);
    }
}

void DocumentSourceGraphLookUp::detachFromOperationContext() {
    _fromExpCtx->setOperationContext(nullptr);
}

void DocumentSourceGraphLookUp::reattachToOperationContext(OperationContext* opCtx) {
    _fromExpCtx->setOperationContext(opCtx);
}

bool DocumentSourceGraphLookUp::validateOperationContext(const OperationContext* opCtx) const {
    return getContext()->getOperationContext() == opCtx &&
        _fromExpCtx->getOperationContext() == opCtx;
}

DocumentSourceGraphLookUp::DocumentSourceGraphLookUp(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString from,
    std::string as,
    std::string connectFromField,
    std::string connectToField,
    boost::intrusive_ptr<Expression> startWith,
    boost::optional<BSONObj> additionalFilter,
    boost::optional<FieldPath> depthField,
    boost::optional<long long> maxDepth,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc)
    : DocumentSource(kStageName, expCtx),
      _from(std::move(from)),
      _as(std::move(as)),
      _connectFromField(std::move(connectFromField)),
      _connectToField(std::move(connectToField)),
      _startWith(std::move(startWith)),
      _additionalFilter(additionalFilter),
      _depthField(depthField),
      _maxDepth(maxDepth),
      _memoryUsageTracker(pExpCtx->getAllowDiskUse(),
                          internalDocumentSourceGraphLookupMaxMemoryBytes.load()),
      _queue(pExpCtx.get(), &_memoryUsageTracker),
      _visited(pExpCtx.get(), &_memoryUsageTracker),
      _cache(pExpCtx->getValueComparator()),
      _unwind(unwindSrc),
      _variables(expCtx->variables),
      _variablesParseState(expCtx->variablesParseState.copyWith(_variables.useIdGenerator())) {
    if (!_from.isOnInternalDb()) {
        serviceOpCounters(expCtx->getOperationContext()).gotNestedAggregate();
    }

    const auto& resolvedNamespace = pExpCtx->getResolvedNamespace(_from);
    _fromExpCtx = pExpCtx->copyForSubPipeline(resolvedNamespace.ns, resolvedNamespace.uuid);
    _fromExpCtx->setInLookup(true);

    // We append an additional BSONObj to '_fromPipeline' as a placeholder for the $match
    // stage we'll eventually construct from the input document.
    _fromPipeline = resolvedNamespace.pipeline;
    _fromPipeline.reserve(_fromPipeline.size() + 1);
    _fromPipeline.push_back(BSON("$match" << BSONObj()));
}

DocumentSourceGraphLookUp::DocumentSourceGraphLookUp(
    const DocumentSourceGraphLookUp& original,
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(kStageName, newExpCtx),
      _from(original._from),
      _as(original._as),
      _connectFromField(original._connectFromField),
      _connectToField(original._connectToField),
      _startWith(original._startWith),
      _additionalFilter(original._additionalFilter),
      _depthField(original._depthField),
      _maxDepth(original._maxDepth),
      _fromExpCtx(
          original._fromExpCtx->copyWith(original.pExpCtx->getResolvedNamespace(_from).ns,
                                         original.pExpCtx->getResolvedNamespace(_from).uuid)),
      _fromPipeline(original._fromPipeline),
      _memoryUsageTracker(pExpCtx->getAllowDiskUse(),
                          internalDocumentSourceGraphLookupMaxMemoryBytes.load()),
      _queue(pExpCtx.get(), &_memoryUsageTracker),
      _visited(pExpCtx.get(), &_memoryUsageTracker),
      _cache(pExpCtx->getValueComparator()),
      _variables(original._variables),
      _variablesParseState(original._variablesParseState.copyWith(_variables.useIdGenerator())) {
    if (original._unwind) {
        _unwind =
            static_cast<DocumentSourceUnwind*>(original._unwind.value()->clone(pExpCtx).get());
    }
}

DocumentSourceGraphLookUp::~DocumentSourceGraphLookUp() {
    const SpillingStats& stats = _stats.spillingStats;
    graphLookupCounters.incrementPerSpilling(stats.getSpills(),
                                             stats.getSpilledBytes(),
                                             stats.getSpilledRecords(),
                                             stats.getSpilledDataStorageSize());
}

intrusive_ptr<DocumentSourceGraphLookUp> DocumentSourceGraphLookUp::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString fromNs,
    std::string asField,
    std::string connectFromField,
    std::string connectToField,
    intrusive_ptr<Expression> startWith,
    boost::optional<BSONObj> additionalFilter,
    boost::optional<FieldPath> depthField,
    boost::optional<long long> maxDepth,
    boost::optional<boost::intrusive_ptr<DocumentSourceUnwind>> unwindSrc) {
    intrusive_ptr<DocumentSourceGraphLookUp> source(
        new DocumentSourceGraphLookUp(expCtx,
                                      std::move(fromNs),
                                      std::move(asField),
                                      std::move(connectFromField),
                                      std::move(connectToField),
                                      std::move(startWith),
                                      additionalFilter,
                                      depthField,
                                      maxDepth,
                                      unwindSrc));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceGraphLookUp::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    NamespaceString from;
    std::string as;
    boost::intrusive_ptr<Expression> startWith;
    std::string connectFromField;
    std::string connectToField;
    boost::optional<FieldPath> depthField;
    boost::optional<long long> maxDepth;
    boost::optional<BSONObj> additionalFilter;

    VariablesParseState vps = expCtx->variablesParseState;

    for (auto&& argument : elem.Obj()) {
        const auto argName = argument.fieldNameStringData();

        if (argName == "startWith") {
            startWith = Expression::parseOperand(expCtx.get(), argument, vps);
            continue;
        } else if (argName == "maxDepth") {
            uassert(40100,
                    str::stream() << "maxDepth must be numeric, found type: "
                                  << typeName(argument.type()),
                    argument.isNumber());
            maxDepth = argument.safeNumberLong();
            uassert(40101,
                    str::stream() << "maxDepth requires a nonnegative argument, found: "
                                  << *maxDepth,
                    *maxDepth >= 0);
            uassert(40102,
                    str::stream() << "maxDepth could not be represented as a long long: "
                                  << *maxDepth,
                    *maxDepth == argument.number());
            continue;
        } else if (argName == "restrictSearchWithMatch") {
            uassert(40185,
                    str::stream() << "restrictSearchWithMatch must be an object, found "
                                  << typeName(argument.type()),
                    argument.type() == Object);

            // We don't need to keep ahold of the MatchExpression, but we do need to ensure
            // that the specified object is parseable and does not contain extensions.
            uassertStatusOKWithContext(
                MatchExpressionParser::parse(argument.embeddedObject(), expCtx),
                "Failed to parse 'restrictSearchWithMatch' option to $graphLookup");

            additionalFilter = argument.embeddedObject().getOwned();
            continue;
        }

        if (argName == "from" || argName == "as" || argName == "connectFromField" ||
            argName == "depthField" || argName == "connectToField") {
            // All remaining arguments to $graphLookup are expected to be strings.
            uassert(40103,
                    str::stream() << "expected string as argument for " << argName
                                  << ", found: " << typeName(argument.type()),
                    argument.type() == String);
        }

        if (argName == "from") {
            from = parseGraphLookupFromAndResolveNamespace(argument,
                                                           expCtx->getNamespaceString().dbName());
        } else if (argName == "as") {
            as = argument.String();
        } else if (argName == "connectFromField") {
            connectFromField = argument.String();
        } else if (argName == "connectToField") {
            connectToField = argument.String();
        } else if (argName == "depthField") {
            depthField = boost::optional<FieldPath>(FieldPath(argument.String()));
        } else {
            uasserted(40104,
                      str::stream()
                          << "Unknown argument to $graphLookup: " << argument.fieldName());
        }
    }

    const bool isMissingRequiredField = from.isEmpty() || as.empty() || !startWith ||
        connectFromField.empty() || connectToField.empty();

    uassert(40105,
            str::stream() << "$graphLookup requires 'from', 'as', 'startWith', 'connectFromField', "
                          << "and 'connectToField' to be specified.",
            !isMissingRequiredField);

    intrusive_ptr<DocumentSourceGraphLookUp> newSource(
        new DocumentSourceGraphLookUp(expCtx,
                                      std::move(from),
                                      std::move(as),
                                      std::move(connectFromField),
                                      std::move(connectToField),
                                      std::move(startWith),
                                      additionalFilter,
                                      depthField,
                                      maxDepth,
                                      boost::none));

    return newSource;
}

boost::intrusive_ptr<DocumentSource> DocumentSourceGraphLookUp::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    return make_intrusive<DocumentSourceGraphLookUp>(*this, newExpCtx);
}

void DocumentSourceGraphLookUp::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_fromExpCtx->getNamespaceString());
    auto introspectionPipeline = Pipeline::parse(_fromPipeline, _fromExpCtx);
    for (auto&& stage : introspectionPipeline->getSources()) {
        stage->addInvolvedCollections(collectionNames);
    }
}

}  // namespace mongo
