// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class SkipAndLimit {
public:
    boost::optional<long long> getLimit() const;

    boost::optional<long long> getSkip() const;

protected:
    boost::optional<long long> _skip;
    boost::optional<long long> _limit;
};

class SkipThenLimit;
class LimitThenSkip;

/**
 * A struct representing a skip and a limit, with the skip to be applied before the limit.
 */
class SkipThenLimit final : public SkipAndLimit {
public:
    SkipThenLimit(boost::optional<long long> skip, boost::optional<long long> limit);
};

/**
 * A struct representing a limit and a skip, with the limit to be applied before the skip.
 */
class LimitThenSkip final : public SkipAndLimit {
public:
    /**
     * Initiates struct with given limit and skip sizes. If skip size is greater than limit size,
     * it will take minimum of two values for skip size. This is done because we cannot skip more
     * documents than limit returned.
     */
    LimitThenSkip(boost::optional<long long> limit, boost::optional<long long> skip);

    /**
     * Returns SkipThenLimit structure representing logically the same operation, but by performing
     * skip before the limit.
     */
    SkipThenLimit flip() const;
};

/**
 * If there are any $limit stages that could be logically swapped forward to the position of the
 * pipeline pointed to by 'itr' without changing the meaning of the query, removes these $limit
 * stages from the Pipeline and returns the resulting limit. A single limit value is computed by
 * taking the minimum after swapping each individual $limit stage forward.
 *
 * This method also implements the ability to swap a $limit before a $skip, by adding the value of
 * the $skip to the value of the $limit.
 */
boost::optional<long long> extractLimitForPushdown(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container);

/**
 * This is similar to extractLimitForPushdown, except that it should be used when the caller does
 * not want to modify the pipeline but still obtain the calculated limit value of the query.
 */
boost::optional<long long> getUserLimit(DocumentSourceContainer::iterator itr,
                                        DocumentSourceContainer* container);

/**
 * If there are any $skip stages that could be logically swapped forward to the position of the
 * pipeline pointed to by 'itr' without changing the meaning of the query, removes these $skip
 * stages from the Pipeline and returns the resulting skip. A single skip value is computed by
 * taking the sum of all $skip stages that participate in swap.
 *
 * This method does NOT swap $skip before $limit. One can use 'extractLimitForPushdown' method to
 * extract all $limit stages and then call this method if it is applicable.
 */
boost::optional<long long> extractSkipForPushdown(DocumentSourceContainer::iterator itr,
                                                  DocumentSourceContainer* container);

}  // namespace mongo
