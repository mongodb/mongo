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

#include "mongo/base/exact_cast.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/skip_and_limit.h"
#include "mongo/platform/overflow_arithmetic.h"

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

namespace {

Pipeline::SourceContainer::iterator eraseAndStich(Pipeline::SourceContainer::iterator itr,
                                                  Pipeline::SourceContainer* container) {
    itr = container->erase(itr);
    // If the removed stage wasn't the last in the pipeline, make sure that the stage followed the
    // erased stage has a valid pointer to the previous document source.
    if (itr != container->end()) {
        (*itr)->setSource(itr != container->begin() ? std::prev(itr)->get() : nullptr);
    }
    return itr;
}

}  // namespace

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
boost::optional<long long> extractLimitForPushdownHelper(Pipeline::SourceContainer::iterator itr,
                                                         Pipeline::SourceContainer* container,
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
                itr = eraseAndStich(itr, container);
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

boost::optional<long long> extractLimitForPushdown(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container) {
    return extractLimitForPushdownHelper(itr, container, true /* shouldModifyPipeline */);
}

boost::optional<long long> getUserLimit(Pipeline::SourceContainer::iterator itr,
                                        Pipeline::SourceContainer* container) {
    return extractLimitForPushdownHelper(itr, container, false /* shouldModifyPipeline */);
}

boost::optional<long long> extractSkipForPushdown(Pipeline::SourceContainer::iterator itr,
                                                  Pipeline::SourceContainer* container) {
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
            itr = eraseAndStich(itr, container);
        } else if (!nextSkip && !nextStage->constraints().canSwapWithSkippingOrLimitingStage) {
            break;
        } else {
            ++itr;
        }
    }

    return skipSum;
}

}  // namespace mongo
