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

#include <map>
#include <utility>

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/abt_compare.h"

namespace mongo::optimizer::ce {

using PartialSchemaSelHints =
    std::map<PartialSchemaKey, SelectivityType, PartialSchemaKeyComparator::Less>;
using PartialSchemaInterval = std::pair<PartialSchemaKey, IntervalReqExpr::Node>;

struct PartialSchemaIntervalComparator {
    bool operator()(const PartialSchemaInterval& k1, const PartialSchemaInterval& k2) const;
};

using PartialSchemaIntervalSelHints =
    std::map<PartialSchemaInterval, SelectivityType, PartialSchemaIntervalComparator>;

/**
 * Estimation based on hints. The hints are organized in a PartialSchemaSelHints structure.
 * SargableNodes are estimated based on the matching PartialSchemaKeys.
 */
class HintedEstimator : public cascades::CardinalityEstimator {
public:
    HintedEstimator(PartialSchemaSelHints pathHints, PartialSchemaIntervalSelHints intervalHints)
        : _pathHints(std::move(pathHints)), _intervalHints(std::move(intervalHints)) {}

    CERecord deriveCE(const Metadata& metadata,
                      const cascades::Memo& memo,
                      const properties::LogicalProps& logicalProps,
                      const QueryParameterMap& queryParameters,
                      ABT::reference_type logicalNodeRef) const final;

private:
    // Selectivity hints per PartialSchemaKey.
    PartialSchemaSelHints _pathHints;
    // Selectivity hints per PartialSchemaKey and IntervalReqExpr::Node.
    PartialSchemaIntervalSelHints _intervalHints;
};

}  // namespace mongo::optimizer::ce
