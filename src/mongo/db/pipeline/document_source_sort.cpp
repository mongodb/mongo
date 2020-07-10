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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_sort.h"

#include <algorithm>

#include "mongo/base/exact_cast.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/collation/collation_index_key.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/s/query/document_source_merge_cursors.h"

namespace mongo {

using boost::intrusive_ptr;
using std::make_pair;
using std::string;
using std::unique_ptr;
using std::vector;

constexpr StringData DocumentSourceSort::kStageName;

DocumentSourceSort::DocumentSourceSort(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       const BSONObj& sortOrder,
                                       uint64_t limit,
                                       uint64_t maxMemoryUsageBytes)
    : DocumentSource(kStageName, pExpCtx),
      _sortExecutor({{sortOrder, pExpCtx},
                     limit,
                     maxMemoryUsageBytes,
                     pExpCtx->tempDir,
                     pExpCtx->allowDiskUse}),
      // The SortKeyGenerator expects the expressions to be serialized in order to detect a sort
      // by a metadata field.
      _sortKeyGen({{sortOrder, pExpCtx}, pExpCtx->getCollator()}) {
    uassert(15976,
            "$sort stage must have at least one sort key",
            !_sortExecutor->sortPattern().empty());
}

REGISTER_DOCUMENT_SOURCE(sort,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSort::createFromBson);

DocumentSource::GetNextResult DocumentSourceSort::doGetNext() {
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
    uint64_t limit = _sortExecutor->getLimit();
    if (explain) {  // always one Value for combined $sort + $limit
        array.push_back(Value(DOC(
            kStageName << DOC("sortKey"
                              << _sortExecutor->sortPattern().serialize(
                                     SortPattern::SortKeySerialization::kForExplain)
                              << "limit"
                              << (_sortExecutor->hasLimit() ? Value(static_cast<long long>(limit))
                                                            : Value())))));
    } else {  // one Value for $sort and maybe a Value for $limit
        MutableDocument inner(_sortExecutor->sortPattern().serialize(
            SortPattern::SortKeySerialization::kForPipelineSerialization));
        array.push_back(Value(DOC(kStageName << inner.freeze())));

        if (_sortExecutor->hasLimit()) {
            auto limitSrc = DocumentSourceLimit::create(pExpCtx, limit);
            limitSrc->serializeToArray(array);
        }
    }
}

boost::optional<long long> DocumentSourceSort::getLimit() const {
    return _sortExecutor->hasLimit() ? boost::optional<long long>{_sortExecutor->getLimit()}
                                     : boost::none;
}

boost::optional<long long> DocumentSourceSort::extractLimitForPushdown(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    int64_t skipSum = 0;
    boost::optional<long long> minLimit;
    while (itr != container->end()) {
        auto nextStage = (*itr).get();
        auto nextSkip = exact_pointer_cast<DocumentSourceSkip*>(nextStage);
        auto nextLimit = exact_pointer_cast<DocumentSourceLimit*>(nextStage);
        int64_t safeSum = 0;

        // The skip and limit values can be very large, so we need to make sure the sum doesn't
        // overflow before applying an optimization to swap the $limit with the $skip.
        if (nextSkip && !overflow::add(skipSum, nextSkip->getSkip(), &safeSum)) {
            skipSum = safeSum;
            ++itr;
        } else if (nextLimit && !overflow::add(nextLimit->getLimit(), skipSum, &safeSum)) {
            if (!minLimit) {
                minLimit = safeSum;
            } else {
                minLimit = std::min(static_cast<long long>(safeSum), *minLimit);
            }

            itr = container->erase(itr);
            // If the removed stage wasn't the last in the pipeline, make sure that the stage
            // followed the erased stage has a valid pointer to the previous document source.
            if (itr != container->end()) {
                (*itr)->setSource(itr != container->begin() ? std::prev(itr)->get() : nullptr);
            }
        } else if (!nextStage->constraints().canSwapWithLimitAndSample) {
            break;
        } else {
            ++itr;
        }
    }

    return minLimit;
}

Pipeline::SourceContainer::iterator DocumentSourceSort::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto stageItr = std::next(itr);
    auto limit = extractLimitForPushdown(stageItr, container);
    if (limit) {
        _sortExecutor->setLimit(*limit);
    }

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    if (auto nextSort = dynamic_cast<DocumentSourceSort*>((*std::next(itr)).get())) {
        // If subsequent $sort stage exists, optimize by erasing the initial one.
        // Since $sort is not guaranteed to be stable, we can blindly remove the first $sort.
        auto thisLim = _sortExecutor->getLimit();
        auto otherLim = nextSort->_sortExecutor->getLimit();
        // When coalescing subsequent $sort stages, retain the existing/lower limit.
        if (thisLim && (!otherLim || otherLim > thisLim)) {
            nextSort->_sortExecutor->setLimit(thisLim);
        }
        Pipeline::SourceContainer::iterator ret = std::next(itr);
        container->erase(itr);
        return ret;
    }
    return std::next(itr);
}

DepsTracker::State DocumentSourceSort::getDependencies(DepsTracker* deps) const {
    for (auto&& keyPart : _sortExecutor->sortPattern()) {
        if (keyPart.expression) {
            keyPart.expression->addDependencies(deps);
        } else {
            deps->fields.insert(keyPart.fieldPath->fullPath());
        }
    }
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
    BSONObj sortOrder,
    uint64_t limit,
    boost::optional<uint64_t> maxMemoryUsageBytes) {
    auto resolvedMaxBytes = maxMemoryUsageBytes
        ? *maxMemoryUsageBytes
        : internalQueryMaxBlockingSortMemoryUsageBytes.load();
    intrusive_ptr<DocumentSourceSort> pSort(
        new DocumentSourceSort(pExpCtx, sortOrder.getOwned(), limit, resolvedMaxBytes));
    return pSort;
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
    DistributedPlanLogic split;
    split.shardsStage = this;
    split.inputSortPattern = _sortExecutor->sortPattern()
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

}  // namespace mongo
