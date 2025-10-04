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

#include "mongo/db/query/util/named_enum.h"
#include "mongo/util/assert_util.h"

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
