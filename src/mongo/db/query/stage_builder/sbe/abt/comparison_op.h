// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::abt {

#define PATHSYNTAX_OPNAMES(F)                                                                      \
    /* Binary Operations */                                                                        \
    /* comparison operations */                                                                    \
    F(Eq)                                                                                          \
    F(EqMember)                                                                                    \
    F(Neq)                                                                                         \
    /*                                                                                             \
     * In Bonsai, the comparison operators Gt/Gte/Lt/Lte form a total order (they are non-type     \
     * bracketing). This means that for operands of different canonical BSON types, they will use  \
     * the total BSON order to determine the result of the comparison. This is opposed to          \
     * type-bracketing operators which return Nothing when applied to operands of different types. \
     *                                                                                             \
     * NOTE: The SBE stage builders use ABTs as intermediate representations of expressions. In    \
     * this context, the comparison operators have type-bracketing semantics. There is no meaning  \
     * to this, it appears to be a historical accident. The result of this is configurable         \
     * semantics during ABT lowering, see abt_lower.h & `SBEExpressionLowering` for more details.  \
     */                                                                                            \
    F(Gt)                                                                                          \
    F(Gte)                                                                                         \
    F(Lt)                                                                                          \
    F(Lte)                                                                                         \
    F(Cmp3w)                                                                                       \
                                                                                                   \
    /* arithmetic operations */                                                                    \
    F(Add)                                                                                         \
    F(Sub)                                                                                         \
    F(Mult)                                                                                        \
    F(Div)                                                                                         \
                                                                                                   \
    /* Nothing-handling */                                                                         \
    F(FillEmpty)                                                                                   \
                                                                                                   \
    /* logical operations */                                                                       \
    F(And)                                                                                         \
    F(Or)                                                                                          \
    F(Not) /* unary operation */                                                                   \
                                                                                                   \
    /* Unary Operations */                                                                         \
    F(Neg)

QUERY_UTIL_NAMED_ENUM_DEFINE(Operations, PATHSYNTAX_OPNAMES);
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
}  // namespace mongo::abt
