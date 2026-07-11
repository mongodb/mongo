// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <utility>
#include <vector>

namespace mongo::stage_builder {

// Prefer the EFn overloads below for any statically-known function name. These std::string_view
// overloads are intended only for the runtime-name path (e.g. dynamically constructed names).
inline auto makeABTFunction(std::string_view name, abt::ABTVector args) {
    return abt::make<abt::FunctionCall>(name, std::move(args));
}

template <typename... Args>
inline auto makeABTFunction(std::string_view name, Args&&... args) {
    return abt::make<abt::FunctionCall>(name, abt::makeSeq(std::forward<Args>(args)...));
}

inline auto makeABTFunction(sbe::EFn fn, abt::ABTVector args) {
    return abt::make<abt::FunctionCall>(fn, std::move(args));
}

template <typename... Args>
inline auto makeABTFunction(sbe::EFn fn, Args&&... args) {
    return abt::make<abt::FunctionCall>(fn, abt::makeSeq(std::forward<Args>(args)...));
}

inline auto makeABTConstant(sbe::value::TypeTags tag, sbe::value::Value value) {
    return abt::make<abt::Constant>(tag, value);
}

inline auto makeABTConstant(std::string_view str) {
    auto [tag, value] = sbe::value::makeNewString(str);
    return makeABTConstant(tag, value);
}

abt::ABT makeVariable(abt::ProjectionName var);

abt::ABT makeUnaryOp(abt::Operations unaryOp, abt::ABT operand);

abt::ABT makeBinaryOp(abt::Operations binaryOp, abt::ABT lhs, abt::ABT rhs);

abt::ABT makeIf(abt::ABT condExpr, abt::ABT thenExpr, abt::ABT elseExpr);

abt::ABT makeLet(const abt::ProjectionName& name, abt::ABT bindExpr, abt::ABT expr);

abt::ABT makeLet(std::vector<abt::ProjectionName> bindNames,
                 abt::ABTVector bindExprs,
                 abt::ABT inExpr);

}  // namespace mongo::stage_builder
