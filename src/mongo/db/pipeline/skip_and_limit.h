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

#pragma once

#include <boost/optional.hpp>

#include "mongo/db/pipeline/pipeline.h"

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
boost::optional<long long> extractLimitForPushdown(Pipeline::SourceContainer::iterator itr,
                                                   Pipeline::SourceContainer* container);

/**
 * This is similar to extractLimitForPushdown, except that it should be used when the caller does
 * not want to modify the pipeline but still obtain the calculated limit value of the query.
 */
boost::optional<long long> getUserLimit(Pipeline::SourceContainer::iterator itr,
                                        Pipeline::SourceContainer* container);

/**
 * If there are any $skip stages that could be logically swapped forward to the position of the
 * pipeline pointed to by 'itr' without changing the meaning of the query, removes these $skip
 * stages from the Pipeline and returns the resulting skip. A single skip value is computed by
 * taking the sum of all $skip stages that participate in swap.
 *
 * This method does NOT swap $skip before $limit. One can use 'extractLimitForPushdown' method to
 * extract all $limit stages and then call this method if it is applicable.
 */
boost::optional<long long> extractSkipForPushdown(Pipeline::SourceContainer::iterator itr,
                                                  Pipeline::SourceContainer* container);

}  // namespace mongo
