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

#include "mongo/db/query/optimizer/utils/unit_test_utils.h"

#include <absl/container/node_hash_map.h>
#include <cstddef>
#include <fstream>  // IWYU pragma: keep
#include <iostream>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"

namespace mongo::optimizer {

static constexpr bool kDebugAsserts = false;


void maybePrintABT(const ABT::reference_type abt) {
    // Always print using the supported versions to make sure we don't crash.
    const std::string strV1 = ExplainGenerator::explain(abt);
    const std::string strV2 = ExplainGenerator::explainV2(abt);
    const std::string strV2Compact = ExplainGenerator::explainV2Compact(abt);
    const std::string strBSON = ExplainGenerator::explainBSONStr(abt);

    if constexpr (kDebugAsserts) {
        std::cout << "V1: " << strV1 << "\n";
        std::cout << "V2: " << strV2 << "\n";
        std::cout << "V2Compact: " << strV2Compact << "\n";
        std::cout << "BSON: " << strBSON << "\n";
    }
}

ABT makeIndexPath(FieldPathType fieldPath, bool isMultiKey) {
    ABT result = make<PathIdentity>();

    for (size_t i = fieldPath.size(); i-- > 0;) {
        if (isMultiKey) {
            result = make<PathTraverse>(PathTraverse::kSingleLevel, std::move(result));
        }
        result = make<PathGet>(std::move(fieldPath.at(i)), std::move(result));
    }

    return result;
}

ABT makeIndexPath(FieldNameType fieldName) {
    return makeIndexPath(FieldPathType{std::move(fieldName)});
}

ABT makeNonMultikeyIndexPath(FieldNameType fieldName) {
    return makeIndexPath(FieldPathType{std::move(fieldName)}, false /*isMultiKey*/);
}

bool planComparator(const PlanAndProps& e1, const PlanAndProps& e2) {
    // Sort plans by estimated cost. If costs are equal, sort lexicographically by plan explain.
    // This allows us to break ties if costs are equal.
    const auto c1 = e1.getRootAnnotation()._cost;
    const auto c2 = e2.getRootAnnotation()._cost;
    if (c1 < c2) {
        return true;
    }
    if (c2 < c1) {
        return false;
    }

    const auto explain1 = ExplainGenerator::explainV2(e1._node);
    const auto explain2 = ExplainGenerator::explainV2(e2._node);
    return explain1 < explain2;
}
}  // namespace mongo::optimizer
