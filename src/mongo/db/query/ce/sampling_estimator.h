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

#include <cstdint>
#include <memory>

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"

namespace mongo::optimizer::ce {

class SamplingTransport;

/**
 * Interface used by 'SamplingEstimator' for executing queries.
 */
class SamplingExecutor {
public:
    virtual ~SamplingExecutor() = default;

    /**
     * Executes the given query, expecting zero or one values in the result.
     *
     * The query must bind a single projection, and must return zero or one rows.
     * This function returns the one value, or Nothing.
     *
     * Caller must destroy the returned SBE value.
     */
    virtual std::pair<sbe::value::TypeTags, sbe::value::Value> execute(
        const Metadata& metadata,
        const QueryParameterMap& queryParameters,
        const PlanAndProps& planAndProps) const = 0;
};

/**
 * Cardinality estimator based on sampling. We recieve from the optimizer a node, a memo, and
 * logical properties, and we estimate the cardinality of the node's new group. Internally
 * potentially many sampling queries are issued for estimation, and those are handled by the
 * provided executor.
 */
class SamplingEstimator : public cascades::CardinalityEstimator {
public:
    SamplingEstimator(OptPhaseManager phaseManager,
                      int64_t numRecords,
                      DebugInfo debugInfo,
                      PrefixId& prefixId,
                      std::unique_ptr<cascades::CardinalityEstimator> fallbackCE,
                      std::unique_ptr<SamplingExecutor> executor);
    ~SamplingEstimator() override;

    CERecord deriveCE(const Metadata& metadata,
                      const cascades::Memo& memo,
                      const properties::LogicalProps& logicalProps,
                      const QueryParameterMap& queryParameters,
                      ABT::reference_type logicalNodeRef) const final;

private:
    std::unique_ptr<SamplingTransport> _transport;
};

}  // namespace mongo::optimizer::ce
