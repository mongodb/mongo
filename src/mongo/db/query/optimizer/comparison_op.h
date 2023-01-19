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

#include "mongo/db/query/optimizer/utils/printable_enum.h"

namespace mongo::optimizer {

#define PATHSYNTAX_OPNAMES(F)   \
    /* comparison operations */ \
    F(Eq)                       \
    F(EqMember)                 \
    F(Neq)                      \
    F(Gt)                       \
    F(Gte)                      \
    F(Lt)                       \
    F(Lte)                      \
    F(Cmp3w)                    \
                                \
    /* binary operations */     \
    F(Add)                      \
    F(Sub)                      \
    F(Mult)                     \
    F(Div)                      \
                                \
    /* unary operations */      \
    F(Neg)                      \
                                \
    /* Nothing-handling */      \
    F(FillEmpty)                \
                                \
    /* logical operations */    \
    F(And)                      \
    F(Or)                       \
    F(Not)

MAKE_PRINTABLE_ENUM(Operations, PATHSYNTAX_OPNAMES);
MAKE_PRINTABLE_ENUM_STRING_ARRAY(OperationsEnum, Operations, PATHSYNTAX_OPNAMES);
#undef PATHSYNTAX_OPNAMES

inline constexpr bool isUnaryOp(Operations op) {
    return op == Operations::Neg || op == Operations::Not;
}

inline constexpr bool isBinaryOp(Operations op) {
    return !isUnaryOp(op);
}

inline constexpr bool isComparisonOp(Operations op) {
    switch (op) {
        case Operations::Eq:
        case Operations::EqMember:
        case Operations::Neq:
        case Operations::Gt:
        case Operations::Gte:
        case Operations::Lt:
        case Operations::Lte:
        case Operations::Cmp3w:
            return true;
        default:
            return false;
    }
}

/**
 * Flip the argument order of a comparison op.
 *
 * Not to be confused with boolean negation: see 'negateComparisonOp'.
 */
inline constexpr Operations flipComparisonOp(Operations op) {
    switch (op) {
        case Operations::Eq:
        case Operations::Neq:
            return op;

        case Operations::Lt:
            return Operations::Gt;
        case Operations::Lte:
            return Operations::Gte;
        case Operations::Gt:
            return Operations::Lt;
        case Operations::Gte:
            return Operations::Lte;

        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * Negate a comparison op, such that negate(op)(x, y) == not(op(x, y)).
 *
 * If the op is not a comparison, return none.
 * If the op can't be negated (for example EqMember), return none.
 *
 * Not to be confused with flipping the argument order: see 'flipComparisonOp'.
 */
inline boost::optional<Operations> negateComparisonOp(Operations op) {
    switch (op) {
        case Operations::Lt:
            return Operations::Gte;
        case Operations::Lte:
            return Operations::Gt;
        case Operations::Eq:
            return Operations::Neq;
        case Operations::Gte:
            return Operations::Lt;
        case Operations::Gt:
            return Operations::Lte;
        case Operations::Neq:
            return Operations::Eq;
        default:
            return {};
    }
}

}  // namespace mongo::optimizer
