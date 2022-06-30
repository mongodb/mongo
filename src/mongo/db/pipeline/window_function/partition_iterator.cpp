/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/window_function/partition_iterator.h"
#include "mongo/util/overloaded_visitor.h"

using boost::optional;

namespace mongo {

namespace {
/**
 * Create an Expression from a SortPattern, if the sort is simple enough.
 *
 * The sort must have one, ascending, non-expression field.
 * The field may be dotted.
 *
 * For example: {ab.c: 1} becomes "$ab.c", but {a: -1} becomes boost::none.
 */
static optional<boost::intrusive_ptr<ExpressionFieldPath>> exprFromSort(
    ExpressionContext* expCtx, const optional<SortPattern>& sortPattern) {
    if (!sortPattern)
        return boost::none;
    if (sortPattern->size() != 1)
        return boost::none;
    const SortPattern::SortPatternPart& part = *sortPattern->begin();

    bool hasFieldPath = part.fieldPath != boost::none;
    bool hasExpression = part.expression != nullptr;
    tassert(5429403,
            "SortPatternPart is supposed to have exactly one: fieldPath, or expression.",
            hasFieldPath != hasExpression);

    if (hasExpression)
        return boost::none;

    // Descending sorts are not allowed with range-based bounds.
    //
    // We think this would be confusing.
    // Does [x, y] mean [lower, upper] or [left, right] ?
    //
    // For example, suppose you sort by {time: -1} to put recent documents first.
    // Would you write 'range: [-5, +2]', with the smaller value first?
    // Or would you write 'range: [+2, -5]', with the more recent value first?
    if (!part.isAscending)
        return boost::none;

    return ExpressionFieldPath::createPathFromString(
        expCtx, part.fieldPath->fullPath(), expCtx->variablesParseState);
}
}  // namespace

PartitionIterator::PartitionIterator(ExpressionContext* expCtx,
                                     DocumentSource* source,
                                     MemoryUsageTracker* tracker,
                                     optional<boost::intrusive_ptr<Expression>> partitionExpr,
                                     const optional<SortPattern>& sortPattern)
    : _expCtx(expCtx),
      _source(source),
      _partitionExpr(std::move(partitionExpr)),
      _sortExpr(exprFromSort(_expCtx, sortPattern)),
      _state(IteratorState::kNotInitialized),
      _cache(std::make_unique<SpillableCache>(_expCtx, tracker)),
      _tracker(tracker) {}

optional<Document> PartitionIterator::operator[](int index) {
    auto docDesired = _indexOfCurrentInPartition + index;

    if (_state == IteratorState::kAdvancedToEOF) {
        return boost::none;
    }

    // Case 0: Outside of lower bound of partition.
    if (docDesired < 0)
        return boost::none;
    // Case 1: Document is in the cache already.
    if (_cache->isIdInCache(docDesired)) {
        return _cache->getDocumentById(docDesired);
    }

    // Case 2: Attempting to access index greater than what the cache currently holds. If we've
    // already exhausted the partition, then early return. Otherwise continue to pull in
    // documents from the prior stage until we get to the desired index or reach the next partition.
    if (_state == IteratorState::kAwaitingAdvanceToNext ||
        _state == IteratorState::kAwaitingAdvanceToEOF) {
        return boost::none;
    }
    for (int i = _cache->getHighestIndex(); i < docDesired; i++) {
        // Pull in document from prior stage.
        getNextDocument();
        // Check for EOF or the next partition.
        if (_state == IteratorState::kAwaitingAdvanceToNext ||
            _state == IteratorState::kAwaitingAdvanceToEOF) {
            return boost::none;
        }
    }
    return _cache->getDocumentById(docDesired);
}

void PartitionIterator::releaseExpired() {
    if (_slots.size() == 0)
        return;

    // The mapping of SlotId -> cacheIndex represents the highest index document in the cache which
    // the executor no longer requires. To be able to safely free the document at index N from the
    // cache, the following conditions must be met:
    // * All executors have expired at least index N
    // * The current index has advanced past N. We need to keep around the "current" document since
    //   the aggregation stage hasn't projected the output fields yet.
    auto minIndex = std::min(_slots[0], _indexOfCurrentInPartition - 1);
    for (auto&& cacheIndex : _slots) {
        minIndex = std::min(minIndex, cacheIndex);
    }

    _cache->freeUpTo(minIndex);
}

PartitionIterator::AdvanceResult PartitionIterator::advance() {
    auto retVal = advanceInternal();
    // After advancing the iterator, check whether there are any documents that can be
    // released from the cache.
    releaseExpired();
    return retVal;
}

PartitionIterator::AdvanceResult PartitionIterator::advanceInternal() {
    // Check if the next document is in the cache.
    if ((_indexOfCurrentInPartition + 1) <= _cache->getHighestIndex()) {
        // Same partition, update the current index.
        _indexOfCurrentInPartition++;
        return AdvanceResult::kAdvanced;
    }

    // At this point, the requested document is not in the cache and we need to consider
    // whether to pull from the prior stage.
    switch (_state) {
        case IteratorState::kNotInitialized:
        case IteratorState::kIntraPartition:
            // Pull in the next document and advance the pointer.
            getNextDocument();
            if (_state == IteratorState::kAwaitingAdvanceToEOF) {
                resetCache();
                _state = IteratorState::kAdvancedToEOF;
                return AdvanceResult::kEOF;
            } else if (_state == IteratorState::kAwaitingAdvanceToNext) {
                advanceToNextPartition();
                return AdvanceResult::kNewPartition;
            } else {
                // Same partition, update the current index.
                _indexOfCurrentInPartition++;
                return AdvanceResult::kAdvanced;
            }
        case IteratorState::kAwaitingAdvanceToNext:
            // The doc in the next partition has already been read.
            advanceToNextPartition();
            return AdvanceResult::kNewPartition;
        case IteratorState::kAwaitingAdvanceToEOF:
        case IteratorState::kAdvancedToEOF:
            // In either of these states, there's no point in reading from the prior document source
            // because we've already hit EOF.
            resetCache();
            return AdvanceResult::kEOF;
        default:
            MONGO_UNREACHABLE_TASSERT(5340102);
    }
}

namespace {
optional<int> numericBound(WindowBounds::Bound<int> bound) {
    return stdx::visit(
        OverloadedVisitor{
            [](WindowBounds::Unbounded) -> optional<int> { return boost::none; },
            [](WindowBounds::Current) -> optional<int> { return 0; },
            [](int i) -> optional<int> { return i; },
        },
        bound);
}

// Assumes both arguments are numeric, and performs Decimal128 addition on them.
Value decimalAdd(const Value& left, const Value& right) {
    // Widening to Decimal128 is a convenient way to avoid having many cases for different numeric
    // types. The 'threshold' values we compute are only used to choose a set of documents; the
    // user can't observe the type.
    return Value(left.coerceToDecimal().add(right.coerceToDecimal()));
}


}  // namespace

optional<std::pair<int, int>> PartitionIterator::getEndpointsRangeBased(
    const WindowBounds::RangeBased& range, const optional<std::pair<int, int>>& hint) {

    tassert(5429404, "Missing _sortExpr with range-based bounds", _sortExpr != boost::none);

    auto lessThan = _expCtx->getValueComparator().getLessThan();

    Value base = (*_sortExpr)->evaluate(*(*this)[0], &_expCtx->variables);
    if (range.unit) {
        uassert(
            5429513,
            str::stream() << "Invalid range: Expected the sortBy field to be a Date, but it was "
                          << base.getType(),
            base.getType() == BSONType::Date);
    } else {
        uassert(
            5429413,
            "Invalid range: For windows that involve date or time ranges, a unit must be provided.",
            base.getType() != BSONType::Date);
        uassert(
            5429414,
            str::stream() << "Invalid range: Expected the sortBy field to be a number, but it was "
                          << base.getType(),
            base.numeric());
    }
    auto add = [&](const Value& base, const Value& delta) -> Value {
        if (range.unit) {
            return Value{
                dateAdd(base.coerceToDate(), *range.unit, delta.coerceToInt(), TimeZone())};
        } else {
            tassert(5429406, "Range-based bounds are specified as a number", delta.numeric());
            return decimalAdd(base, delta);
        }
    };
    auto hasExpectedType = [&](const Value& v) -> bool {
        if (range.unit) {
            return v.getType() == BSONType::Date;
        } else {
            return v.numeric();
        }
    };

    // 'lower' is the smallest offset in the partition that's within the lower bound of the window.
    optional<int> lower = stdx::visit(
        OverloadedVisitor{
            [&](WindowBounds::Current) -> optional<int> {
                // 'range: ["current", _]' means the current document, which is always offset 0.
                return 0;
            },
            [&](WindowBounds::Unbounded) -> optional<int> {
                // Find the leftmost document whose sortBy field evaluates to a numeric value.

                // Start from the beginning, or the hint, whichever is higher.
                // Note that the hint may no longer be a valid offset, if some documents were
                // released from the cache.
                int start = getMinCachedOffset();
                if (hint) {
                    start = std::max(hint->first, start);
                }

                for (int i = start;; ++i) {
                    auto doc = (*this)[i];
                    if (!doc) {
                        return boost::none;
                    }
                    Value v = (*_sortExpr)->evaluate(*doc, &_expCtx->variables);
                    if (hasExpectedType(v)) {
                        return i;
                    }
                }
            },
            [&](const Value& delta) -> optional<int> {
                Value threshold = add(base, delta);

                // Start from the beginning, or the hint, whichever is higher.
                // Note that the hint may no longer be a valid offset, if some documents were
                // released from the cache.
                int start = getMinCachedOffset();
                if (hint) {
                    start = std::max(hint->first, start);
                }

                boost::optional<Document> doc;
                for (int i = start; (doc = (*this)[i]); ++i) {
                    Value v = (*_sortExpr)->evaluate(*doc, &_expCtx->variables);
                    if (!lessThan(v, threshold)) {
                        // This is the first doc we've scanned that crossed the threshold.
                        return i;
                    }
                }
                // We scanned every document in the partition, and none crossed the
                // threshold. So the window must be shifted so far to the right that no
                // documents fall in it.
                return boost::none;
            },
        },
        range.lower);

    if (!lower)
        return boost::none;

    // 'upper' is the largest offset in the partition that's within the upper bound of the window.
    optional<int> upper = stdx::visit(
        OverloadedVisitor{
            [&](WindowBounds::Current) -> optional<int> {
                // 'range: [_, "current"]' means the current document, which is offset 0.
                return 0;
            },
            [&](WindowBounds::Unbounded) -> optional<int> {
                // Find the rightmost document whose sortBy field evaluates to a numeric value.

                // We know that the current document, the lower bound, and the hint (if present)
                // are all numeric, so start scanning from whichever is highest.
                int start = std::max(0, *lower);
                if (hint) {
                    start = std::max(hint->second, start);
                }

                boost::optional<Document> doc;
                for (int i = start; (doc = (*this)[i]); ++i) {
                    Value v = (*_sortExpr)->evaluate(*doc, &_expCtx->variables);
                    if (!hasExpectedType(v)) {
                        // The previously scanned doc is the rightmost numeric one. Since we start
                        // from '0', 'hint', or 'lower', which are all numeric, we should never hit
                        // this case on the first iteration.
                        tassert(5429412,
                                "Failed to find the rightmost numeric document, "
                                "while computing window bounds",
                                i != start);
                        return i - 1;
                    }
                }
                return getMaxCachedOffset();
            },
            [&](const Value& delta) -> optional<int> {
                // Pull in documents until the sortBy value crosses 'base + delta'.
                Value threshold = add(base, delta);

                // If there's no hint, start scanning from the lower bound.
                // If there is a hint, start from whichever is greater: lower bound or hint.
                // Usually the hint is greater, but with bounds like [0, 0] the new lower bound
                // will be greater than the old upper bound.
                int start = *lower;
                if (hint) {
                    start = std::max(hint->second, start);
                }

                for (int i = start;; ++i) {
                    auto doc = (*this)[i];
                    if (!doc) {
                        // We scanned every document in the partition, and none crossed the upper
                        // bound. So the upper bound contains everything up to the end of the
                        // partition.
                        return getMaxCachedOffset();
                    }
                    Value v = (*_sortExpr)->evaluate(*doc, &_expCtx->variables);
                    if (lessThan(threshold, v)) {
                        // This doc exceeded the upper bound.
                        // The previously scanned doc (if any) is the greatest in-bounds one.
                        if (i == start) {
                            // This case can happen, for example, at the beginning of a partition
                            // when the window is 'range: [-100, -5]'. There can be documents
                            // within the lower bound of -100, but none within the upper bound of
                            // -5.
                            return boost::none;
                        } else {
                            return i - 1;
                        }
                    }
                }
            },
        },
        range.upper);

    if (!upper)
        return boost::none;

    return std::pair{*lower, *upper};
}

optional<std::pair<int, int>> PartitionIterator::getEndpointsDocumentBased(
    const WindowBounds::DocumentBased& docBounds,
    const optional<std::pair<int, int>>& hint = boost::none) {
    optional<int> lowerBound = numericBound(docBounds.lower);
    optional<int> upperBound = numericBound(docBounds.upper);
    tassert(5423302,
            "Bounds should never be inverted",
            !lowerBound || !upperBound || lowerBound <= upperBound);

    // Pull documents into the cache until it contains the whole window.
    // We want to know whether the window reaches the end of the partition.
    if (upperBound) {
        // For a right-bounded window we only need to pull in documents up to the bound.
        (*this)[*upperBound];
    } else {
        // For a right-unbounded window we need to pull in the whole partition. operator[] reports
        // end of partition by returning boost::none instead of a document.
        cacheWholePartition();
    }

    // Valid offsets into the cache are any 'i' such that
    // '_cache->getDocumentById(_indexOfCurrentInPartition + i)' is valid. We know the cache is
    // nonempty because it contains the current document.
    int cacheOffsetMin = getMinCachedOffset();
    int cacheOffsetMax = getMaxCachedOffset();

    // The window can only be empty if the bounds are shifted completely out of the partition.
    if (lowerBound && lowerBound > cacheOffsetMax)
        return boost::none;
    if (upperBound && upperBound < cacheOffsetMin)
        return boost::none;

    // Now we know that the window is nonempty, and the cache contains it.
    // All we have to do is clamp the bounds to fall within the cache.
    auto clamp = [&](int offset) {
        // Return the closest offset from the interval '[cacheOffsetMin, cacheOffsetMax]'.
        return std::max(cacheOffsetMin, std::min(offset, cacheOffsetMax));
    };
    int lowerOffset = lowerBound ? clamp(*lowerBound) : cacheOffsetMin;
    int upperOffset = upperBound ? clamp(*upperBound) : cacheOffsetMax;

    return {{lowerOffset, upperOffset}};
}

optional<std::pair<int, int>> PartitionIterator::getEndpoints(
    const WindowBounds& bounds, const optional<std::pair<int, int>>& hint = boost::none) {

    tassert(5423301, "getEndpoints assumes there is a current document", (*this)[0] != boost::none);

    return stdx::visit(
        OverloadedVisitor{
            [&](const WindowBounds::DocumentBased docBounds) {
                return getEndpointsDocumentBased(docBounds, hint);
            },
            [&](const WindowBounds::RangeBased rangeBounds) {
                return getEndpointsRangeBased(rangeBounds, hint);
            },
        },
        bounds.bounds);
}

void PartitionIterator::getNextDocument() {
    tassert(5340103,
            "Invalid call to PartitionIterator::getNextDocument",
            _state != IteratorState::kAdvancedToEOF);

    auto getNextRes = _source->getNext();
    if (getNextRes.isEOF()) {
        _state = IteratorState::kAwaitingAdvanceToEOF;
        return;
    }

    if (!getNextRes.isAdvanced())
        return;

    auto doc = getNextRes.releaseDocument();

    // Greedily populate the internal document cache to enable easier memory tracking versus
    // detecting the changing document size during execution of each function.
    doc.fillCache();

    if (_partitionExpr) {
        if (_state == IteratorState::kNotInitialized) {
            _partitionComparator =
                std::make_unique<PartitionKeyComparator>(_expCtx, *_partitionExpr, doc);
            _nextPartitionDoc = std::move(doc);
            _tracker->update(getNextPartitionStateSize());
            advanceToNextPartition();
        } else if (_partitionComparator->isDocumentNewPartition(doc)) {
            _nextPartitionDoc = std::move(doc);
            _tracker->update(getNextPartitionStateSize());
            _state = IteratorState::kAwaitingAdvanceToNext;
        } else {
            _cache->addDocument(std::move(doc));
        }
    } else {
        _cache->addDocument(std::move(doc));
        _state = IteratorState::kIntraPartition;
    }
}

}  // namespace mongo
