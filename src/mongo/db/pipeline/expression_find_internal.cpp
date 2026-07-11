// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression_find_internal.h"

#include "mongo/db/exec/expression/evaluate.h"

namespace mongo {

Value ExpressionInternalFindPositional::evaluate(const Document& root,
                                                 Variables* variables,
                                                 const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionInternalFindSlice::evaluate(const Document& root,
                                            Variables* variables,
                                            const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

Value ExpressionInternalFindElemMatch::evaluate(const Document& root,
                                                Variables* variables,
                                                const EvaluationContext& ctx) const {
    return exec::expression::evaluate(*this, root, variables, ctx);
}

}  // namespace mongo
