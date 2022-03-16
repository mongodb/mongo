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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_sort.h"

#include <algorithm>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using boost::intrusive_ptr;
using std::make_pair;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {
struct BoundMakerAsc {
    const Seconds bound;

    DocumentSourceSort::SortableDate operator()(DocumentSourceSort::SortableDate key) const {
        return {key.date - bound};
    }

    long long serialize() const {
        return duration_cast<Seconds>(bound).count();
    }
};

struct BoundMakerDesc {
    const Seconds bound;

    DocumentSourceSort::SortableDate operator()(DocumentSourceSort::SortableDate key) const {
        return {key.date + bound};
    }

    long long serialize() const {
        return duration_cast<Seconds>(bound).count();
    }
};
struct CompAsc {
    int operator()(DocumentSourceSort::SortableDate x, DocumentSourceSort::SortableDate y) const {
        // compare(x, y) op 0 means x op y, for any comparator 'op'.
        if (x.date.toMillisSinceEpoch() < y.date.toMillisSinceEpoch())
            return -1;
        if (x.date.toMillisSinceEpoch() > y.date.toMillisSinceEpoch())
            return 1;
        return 0;
    }
};
struct CompDesc {
    int operator()(DocumentSourceSort::SortableDate x, DocumentSourceSort::SortableDate y) const {
        // compare(x, y) op 0 means x op y, for any comparator 'op'.
        if (x.date.toMillisSinceEpoch() > y.date.toMillisSinceEpoch())
            return -1;
        if (x.date.toMillisSinceEpoch() < y.date.toMillisSinceEpoch())
            return 1;
        return 0;
    }
};

using TimeSorterAsc =
    BoundedSorter<DocumentSourceSort::SortableDate, Document, CompAsc, BoundMakerAsc>;
using TimeSorterDesc =
    BoundedSorter<DocumentSourceSort::SortableDate, Document, CompDesc, BoundMakerDesc>;
}  // namespace

constexpr StringData DocumentSourceSort::kStageName;

DocumentSourceSort::DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       const SortPattern& sortOrder,
                                       uint64_t limit,
                                       uint64_t maxMemoryUsageBytes)
    : DocumentSource(kStageName, pExpCtx),
      _sortExecutor(
          {sortOrder, limit, maxMemoryUsageBytes, pExpCtx->tempDir, pExpCtx->allowDiskUse}),
      // The SortKeyGenerator expects the expressions to be serialized in order to detect a sort
      // by a metadata field.
      _sortKeyGen({sortOrder, pExpCtx->getCollator()}) {
    uassert(15976,
            "$sort stage must have at least one sort key",
            !_sortExecutor->sortPattern().empty());
}

REGISTER_DOCUMENT_SOURCE(sort,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSort::createFromBson,
                         AllowedWithApiStrict::kAlways);
REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    _internalBoundedSort,
    LiteParsedDocumentSourceDefault::parse,
    DocumentSourceSort::parseBoundedSort,
    AllowedWithApiStrict::kNeverInVersion1,
    AllowedWithClientType::kAny,
    boost::
        none /* TODO SERVER-52286 feature_flags::gFeatureFlagBucketUnpackWithSort.getVersion() */,
    feature_flags::gFeatureFlagBucketUnpackWithSort50.isEnabledAndIgnoreFCV());


DocumentSource::GetNextResult DocumentSourceSort::doGetNext() {
    if (_timeSorter) {
        // Only pull input as necessary to get _timeSorter to have a result.
        while (_timeSorter->getState() == TimeSorterInterface::State::kWait) {
            auto input = pSource->getNext();
            switch (input.getStatus()) {
                case GetNextResult::ReturnStatus::kPauseExecution:
                    return input;
                case GetNextResult::ReturnStatus::kEOF:
                    // Tell _timeSorter there will be no more input. In response, its state
                    // will never be kWait again, and so we'll never call pSource->getNext() again.
                    _timeSorter->done();
                    continue;
                case GetNextResult::ReturnStatus::kAdvanced:
                    Document doc = input.getDocument();
                    auto time = doc.getField(_sortExecutor->sortPattern()[0].fieldPath->fullPath());
                    uassert(6369909,
                            "$_internalBoundedSort only handles Date values",
                            time.getType() == Date);
                    _timeSorter->add({time.getDate()}, doc);
                    continue;
            }
        }

        if (_timeSorter->getState() == TimeSorterInterface::State::kDone)
            return GetNextResult::makeEOF();

        return _timeSorter->next().second;
    }

    if (!_populated) {
        const auto populationResult = populate();
        if (populationResult.isPaused()) {
            return populationResult;
        }
        invariant(populationResult.isEOF());
    }

    if (!_sortExecutor->hasNext()) {
        return GetNextResult::makeEOF();
    }

    return GetNextResult{_sortExecutor->getNext().second};
}

void DocumentSourceSort::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    if (_timeSorter) {
        tassert(6369900,
                "$_internalBoundedSort should not absorb a $limit",
                !_sortExecutor->hasLimit());

        // TODO SERVER-63637 Implement execution stats.

        // {$_internalBoundedSort: {sortKey, bound}}
        auto sortKey = _sortExecutor->sortPattern().serialize(
            SortPattern::SortKeySerialization::kForPipelineSerialization);
        array.push_back(Value{Document{{
            {"$_internalBoundedSort"_sd,
             Document{{
                 {"sortKey"_sd, std::move(sortKey)},
                 {"bound"_sd, _timeSorter->serializeBound()},
             }}},
        }}});
        return;
    }
    uint64_t limit = _sortExecutor->getLimit();


    if (!explain) {  // one Value for $sort and maybe a Value for $limit
        MutableDocument inner(_sortExecutor->sortPattern().serialize(
            SortPattern::SortKeySerialization::kForPipelineSerialization));
        array.push_back(Value(DOC(kStageName << inner.freeze())));

        if (_sortExecutor->hasLimit()) {
            auto limitSrc = DocumentSourceLimit::create(pExpCtx, limit);
            limitSrc->serializeToArray(array);
        }
        return;
    }

    MutableDocument mutDoc(
        DOC(kStageName << DOC("sortKey"
                              << _sortExecutor->sortPattern().serialize(
                                     SortPattern::SortKeySerialization::kForExplain)
                              << "limit"
                              << (_sortExecutor->hasLimit() ? Value(static_cast<long long>(limit))
                                                            : Value()))));

    if (explain >= ExplainOptions::Verbosity::kExecStats) {
        auto& stats = _sortExecutor->stats();

        mutDoc["totalDataSizeSortedBytesEstimate"] =
            Value(static_cast<long long>(stats.totalDataSizeBytes));
        mutDoc["usedDisk"] = Value(stats.spills > 0 ? true : false);
    }

    array.push_back(Value(mutDoc.freeze()));
}

boost::optional<long long> DocumentSourceSort::getLimit() const {
    return _sortExecutor->hasLimit() ? boost::optional<long long>{_sortExecutor->getLimit()}
                                     : boost::none;
}

Pipeline::SourceContainer::iterator DocumentSourceSort::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (_timeSorter) {
        // Do not absorb a limit, or combine with other sort stages.
        return std::next(itr);
    }

    auto stageItr = std::next(itr);
    auto limit = extractLimitForPushdown(stageItr, container);
    if (limit)
        _sortExecutor->setLimit(*limit);

    auto nextStage = std::next(itr);
    if (nextStage == container->end()) {
        return container->end();
    }

    limit = getLimit();

    // Since $sort is not guaranteed to be stable, we can blindly remove the first $sort only when
    // there's no limit on the current sort.
    auto nextSort = dynamic_cast<DocumentSourceSort*>((*nextStage).get());
    if (!limit && nextSort) {
        container->erase(itr);
        return nextStage;
    }

    if (limit && nextSort) {
        // If there's a limit between two adjacent sorts with the same key pattern it's safe to
        // merge the two sorts and take the minimum of the limits.
        if (dynamic_cast<DocumentSourceSort*>((*itr).get())->getSortKeyPattern() ==
            nextSort->getSortKeyPattern()) {
            // When coalescing subsequent $sort stages, the existing/lower limit is retained in
            // 'setLimit'.
            nextSort->_sortExecutor->setLimit(*limit);
            container->erase(itr);
        }
    }
    return nextStage;
}

DepsTracker::State DocumentSourceSort::getDependencies(DepsTracker* deps) const {
    _sortExecutor->sortPattern().addDependencies(deps);

    if (pExpCtx->needsMerge) {
        // Include the sort key if we will merge several sorted streams later.
        deps->setNeedsMetadata(DocumentMetadataFields::kSortKey, true);
    }

    return DepsTracker::State::SEE_NEXT;
}

intrusive_ptr<DocumentSource> DocumentSourceSort::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(15973, "the $sort key specification must be an object", elem.type() == Object);
    return create(pExpCtx, elem.embeddedObject());
}

intrusive_ptr<DocumentSourceSort> DocumentSourceSort::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    const SortPattern& sortOrder,
    uint64_t limit,
    boost::optional<uint64_t> maxMemoryUsageBytes) {
    auto resolvedMaxBytes = maxMemoryUsageBytes
        ? *maxMemoryUsageBytes
        : internalQueryMaxBlockingSortMemoryUsageBytes.load();
    intrusive_ptr<DocumentSourceSort> pSort(
        new DocumentSourceSort(pExpCtx, sortOrder, limit, resolvedMaxBytes));
    return pSort;
}

intrusive_ptr<DocumentSourceSort> DocumentSourceSort::parseBoundedSort(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(6369905,
            "the $_internalBoundedSort key specification must be an object",
            elem.type() == Object);
    BSONObj args = elem.embeddedObject();

    BSONElement key = args["sortKey"];
    uassert(6369904, "$_internalBoundedSort sortKey must be an object", key.type() == Object);
    SortPattern pat{key.embeddedObject(), expCtx};
    uassert(6369903, "$_internalBoundedSort doesn't support compound sort", pat.size() == 1);
    uassert(6369901,
            "$_internalBoundedSort doesn't support an expression in the sortKey",
            pat[0].expression == nullptr);
    uassert(6369907,
            "$_internalBoundedSort doesn't support dotted field names",
            pat[0].fieldPath->getPathLength() == 1);

    BSONElement bound = args["bound"];
    int boundN = uassertStatusOK(bound.parseIntegerElementToNonNegativeLong());

    SortOptions opts;
    opts.maxMemoryUsageBytes = internalQueryMaxBlockingSortMemoryUsageBytes.load();
    if (expCtx->allowDiskUse) {
        opts.extSortAllowed = true;
        opts.tempDir = expCtx->tempDir;
    }

    auto ds = DocumentSourceSort::create(expCtx, pat);
    if (pat[0].isAscending) {
        ds->_timeSorter.reset(new TimeSorterAsc{opts, CompAsc{}, BoundMakerAsc{Seconds{boundN}}});
    } else {
        ds->_timeSorter.reset(
            new TimeSorterDesc{opts, CompDesc{}, BoundMakerDesc{Seconds{boundN}}});
    }
    return ds;
}

DocumentSource::GetNextResult DocumentSourceSort::populate() {
    auto nextInput = pSource->getNext();
    for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
        loadDocument(nextInput.releaseDocument());
    }
    if (nextInput.isEOF()) {
        loadingDone();
    }
    return nextInput;
}

void DocumentSourceSort::loadDocument(Document&& doc) {
    invariant(!_populated);

    Value sortKey;
    Document docForSorter;
    // We always need to extract the sort key if we've reached this point. If the query system had
    // already computed the sort key we'd have split the pipeline there, would be merging presorted
    // documents, and wouldn't use this method.
    std::tie(sortKey, docForSorter) = extractSortKey(std::move(doc));
    _sortExecutor->add(sortKey, docForSorter);
}

void DocumentSourceSort::loadingDone() {
    _sortExecutor->loadingDone();
    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(pExpCtx->opCtx);
    metricsCollector.incrementKeysSorted(_sortExecutor->stats().keysSorted);
    metricsCollector.incrementSorterSpills(_sortExecutor->stats().spills);
    _populated = true;
}

bool DocumentSourceSort::usedDisk() {
    return _sortExecutor->wasDiskUsed();
}

std::pair<Value, Document> DocumentSourceSort::extractSortKey(Document&& doc) const {
    Value sortKey = _sortKeyGen->computeSortKeyFromDocument(doc);

    if (pExpCtx->needsMerge) {
        // If this sort stage is part of a merged pipeline, make sure that each Document's sort key
        // gets saved with its metadata.
        MutableDocument toBeSorted(std::move(doc));
        toBeSorted.metadata().setSortKey(sortKey, _sortKeyGen->isSingleElementKey());

        return std::make_pair(std::move(sortKey), toBeSorted.freeze());
    } else {
        return std::make_pair(std::move(sortKey), std::move(doc));
    }
}

boost::optional<DocumentSource::DistributedPlanLogic> DocumentSourceSort::distributedPlanLogic() {
    uassert(6369906,
            "$_internalBoundedSort cannot be the first stage on the merger, because it requires "
            "almost-sorted input, which the shardsPart of a pipeline can't provide",
            !_timeSorter);

    DistributedPlanLogic split;
    split.shardsStage = this;
    split.mergeSortPattern = _sortExecutor->sortPattern()
                                 .serialize(SortPattern::SortKeySerialization::kForSortKeyMerging)
                                 .toBson();
    if (auto limit = getLimit()) {
        split.mergingStage = DocumentSourceLimit::create(pExpCtx, *limit);
    }
    return split;
}

bool DocumentSourceSort::canRunInParallelBeforeWriteStage(
    const std::set<std::string>& nameOfShardKeyFieldsUponEntryToStage) const {
    // This is an interesting special case. If there are no further stages which require merging the
    // streams into one, a $sort should not require it. This is only the case because the sort order
    // doesn't matter for a pipeline ending with a write stage. We may encounter it here as an
    // intermediate stage before a final $group with a $sort, which would make sense. Should we
    // extend our analysis to detect if an exchange is appropriate in a general pipeline, a $sort
    // would generally require merging the streams before producing output.
    return false;
}

namespace {
/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number.
 *
 * Each user of the Sorter must implement this function to ensure that all temporary files that the
 * Sorter instances produce are uniquely identified using a unique file name extension with separate
 * atomic variable. This is necessary because the sorter.cpp code is separately included in multiple
 * places, rather than compiled in one place and linked, and so cannot provide a globally unique ID.
 */
std::string nextFileName() {
    static AtomicWord<unsigned> sortExecutorFileCounter;
    return "extsort-time-sorter." + std::to_string(sortExecutorFileCounter.fetchAndAdd(1));
}
}  // namespace
}  // namespace mongo

#include "mongo/db/sorter/sorter.cpp"
template class ::mongo::BoundedSorter<::mongo::DocumentSourceSort::SortableDate,
                                      ::mongo::Document,
                                      ::mongo::CompAsc,
                                      ::mongo::BoundMakerAsc>;
template class ::mongo::BoundedSorter<::mongo::DocumentSourceSort::SortableDate,
                                      ::mongo::Document,
                                      ::mongo::CompDesc,
                                      ::mongo::BoundMakerDesc>;
