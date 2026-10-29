// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/gen_abt_helpers.h"

#include "mongo/db/query/query_feature_flags_gen.h"

#include <cstddef>

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
