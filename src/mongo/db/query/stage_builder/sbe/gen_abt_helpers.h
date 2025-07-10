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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/expr.h"
#include "mongo/db/query/stage_builder/sbe/abt/syntax/syntax.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"

#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>

namespace mongo::stage_builder {

inline auto makeABTFunction(StringData name, abt::ABTVector args) {
    return abt::make<abt::FunctionCall>(std::string{name}, std::move(args));
}

template <typename... Args>
inline auto makeABTFunction(StringData name, Args&&... args) {
    return abt::make<abt::FunctionCall>(std::string{name},
                                        abt::makeSeq(std::forward<Args>(args)...));
}

inline auto makeABTConstant(sbe::value::TypeTags tag, sbe::value::Value value) {
    return abt::make<abt::Constant>(tag, value);
}

inline auto makeABTConstant(StringData str) {
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
