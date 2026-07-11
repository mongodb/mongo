// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/skip_and_limit.h"

#include "mongo/base/exact_cast.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/platform/overflow_arithmetic.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::optional<long long> SkipAndLimit::getLimit() const {
    return _limit;
}

boost::optional<long long> SkipAndLimit::getSkip() const {
    return _skip;
}

SkipThenLimit::SkipThenLimit(boost::optional<long long> skip, boost::optional<long long> limit) {
    _skip = skip;
    _limit = limit;
}

LimitThenSkip::LimitThenSkip(boost::optional<long long> limit, boost::optional<long long> skip) {
    _limit = limit;
    // We cannot skip more documents than received after applying limit. So if both limit and skip
    // are defined, skip size must be not greater than limit size.
    if (skip) {
        _skip = std::min(*skip, limit.get_value_or(std::numeric_limits<long long>::max()));
    }
}

SkipThenLimit LimitThenSkip::flip() const {
    if (_limit) {
        return {_skip, *_limit - _skip.get_value_or(0)};
    }

    return {_skip, boost::none};
}

/**
 * If there are any $limit stages that could be logically swapped forward to the position of the
 * pipeline pointed to by 'itr' without changing the meaning of the query, removes these $limit
 * stages from the Pipeline and returns the resulting limit. A single limit value is computed by
 * taking the minimum after swapping each individual $limit stage forward.
 *
 * This method also implements the ability to swap a $limit before a $skip, by adding the value of
 * the $skip to the value of the $limit.
 *
 * If shouldModifyPipeline is false, this method does not swap any stages but rather just returns
 * the single limit value described above.
 */
boost::optional<long long> extractLimitForPushdownHelper(DocumentSourceContainer::iterator itr,
                                                         DocumentSourceContainer* container,
                                                         bool shouldModifyPipeline) {
    int64_t skipSum = 0;
    boost::optional<long long> minLimit;
    while (itr != container->end()) {
        auto nextStage = itr->get();
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

            if (shouldModifyPipeline) {
                itr = container->erase(itr);
            } else {
                ++itr;
            }
        } else if (!nextStage->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        } else {
            ++itr;
        }
    }

    return minLimit;
}

boost::optional<long long> extractLimitForPushdown(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) {
    return extractLimitForPushdownHelper(itr, container, true /* shouldModifyPipeline */);
}

boost::optional<long long> getUserLimit(DocumentSourceContainer::iterator itr,
                                        DocumentSourceContainer* container) {
    return extractLimitForPushdownHelper(itr, container, false /* shouldModifyPipeline */);
}

boost::optional<long long> extractSkipForPushdown(DocumentSourceContainer::iterator itr,
                                                  DocumentSourceContainer* container) {
    boost::optional<long long> skipSum;
    while (itr != container->end()) {
        auto nextStage = itr->get();
        auto nextSkip = exact_pointer_cast<DocumentSourceSkip*>(nextStage);
        int64_t safeSum = 0;

        // The skip values can be very large, so we need to make sure the sum doesn't overflow
        // before extracting skip stage for pushdown. Even if we failed to extract $skip stage due
        // to overflow, we still want to continue our analysis after it. If there is multiple $skip
        // stages one after another, only total sum of skipped documents matters.
        if (nextSkip && !overflow::add(skipSum.get_value_or(0), nextSkip->getSkip(), &safeSum)) {
            skipSum = safeSum;
            itr = container->erase(itr);
        } else if (!nextSkip && !nextStage->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        } else {
            ++itr;
        }
    }

    return skipSum;
}

}  // namespace mongo
