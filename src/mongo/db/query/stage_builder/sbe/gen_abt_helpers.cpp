/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/stage_builder/sbe/gen_abt_helpers.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/query/algebra/polyvalue.h"
#include "mongo/db/query/bson_typemask.h"
#include "mongo/db/query/stage_builder/sbe/abt_defs.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
abt::ABT makeVariable(abt::ProjectionName var) {
    return abt::make<abt::Variable>(std::move(var));
}

abt::ABT makeUnaryOp(abt::Operations unaryOp, abt::ABT operand) {
    return abt::make<abt::UnaryOp>(unaryOp, std::move(operand));
}

abt::ABT makeBinaryOp(abt::Operations binaryOp, abt::ABT lhs, abt::ABT rhs) {
    return abt::make<abt::BinaryOp>(binaryOp, std::move(lhs), std::move(rhs));
}

abt::ABT makeIf(abt::ABT condExpr, abt::ABT thenExpr, abt::ABT elseExpr) {
    return abt::make<abt::If>(std::move(condExpr), std::move(thenExpr), std::move(elseExpr));
}

abt::ABT makeLet(const abt::ProjectionName& name, abt::ABT bindExpr, abt::ABT expr) {
    return abt::make<abt::Let>(name, std::move(bindExpr), std::move(expr));
}

abt::ABT makeLet(std::vector<abt::ProjectionName> bindNames,
                 abt::ABTVector bindExprs,
                 abt::ABT inExpr) {
    if (!feature_flags::gFeatureFlagSbeUpgradeBinaryTrees.checkEnabled()) {
        for (size_t idx = bindExprs.size(); idx > 0;) {
            --idx;
            inExpr = abt::make<abt::Let>(
                std::move(bindNames[idx]), std::move(bindExprs[idx]), std::move(inExpr));
        }
        return inExpr;
    } else {
        bindExprs.emplace_back(std::move(inExpr));
        return abt::make<abt::MultiLet>(std::move(bindNames), std::move(bindExprs));
    }
}
}  // namespace mongo::stage_builder
