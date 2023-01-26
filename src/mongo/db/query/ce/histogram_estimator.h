/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/stats/collection_statistics.h"

namespace mongo::optimizer::ce {

enum IntervalEstimationMode { kUseHistogram, kUseTypeCounts, kFallback };
using IntervalEstimation =
    std::tuple<IntervalEstimationMode,
               boost::optional<std::reference_wrapper<const BoundRequirement>>,
               boost::optional<std::reference_wrapper<const BoundRequirement>>>;

/**
 * Analyzes an interval to define an estimation mode and summarize the bounds. This method is in the
 * header for unit tests to use.
 */
IntervalEstimation analyzeIntervalEstimationMode(const stats::ArrayHistogram* histogram,
                                                 const IntervalRequirement& interval);

class HistogramTransport;

class HistogramEstimator : public cascades::CardinalityEstimator {
public:
    HistogramEstimator(std::shared_ptr<stats::CollectionStatistics> stats,
                       std::unique_ptr<cascades::CardinalityEstimator> fallbackCE);
    ~HistogramEstimator();

    CEType deriveCE(const Metadata& metadata,
                    const cascades::Memo& memo,
                    const properties::LogicalProps& logicalProps,
                    ABT::reference_type logicalNodeRef) const final;

private:
    std::unique_ptr<HistogramTransport> _transport;
};

}  // namespace mongo::optimizer::ce
