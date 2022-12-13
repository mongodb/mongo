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

#include <stack>

#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

class ExpressionAlgebrizerContext {
public:
    ExpressionAlgebrizerContext(bool assertExprSort,
                                bool assertPathSort,
                                const ProjectionName& rootProjection,
                                PrefixId& prefixId);

    /**
     * Push an ABT onto the stack. Optionally perform a check on the type of the ABT based on
     * 'assertExprSort' and 'assertPathSort'
     */
    template <typename T, typename... Args>
    inline auto push(Args&&... args) {
        push(ABT::make<T>(std::forward<Args>(args)...));
    }
    void push(ABT node);

    /*
     * Pop the most recent ABT from the stack. Asserts if there is no node in the stack.
     */
    ABT pop();

    /*
     * Asserts if there are not at least 'arity' nodes in the stack.
     */
    void ensureArity(size_t arity);

    const ProjectionName& getRootProjection() const;
    const ABT& getRootProjVar() const;

    PrefixId& getPrefixId();

    /**
     * Returns a unique projection. It will be prefixed by 'uniqueIdPrefix'.
     */
    template <size_t N>
    ProjectionName getNextId(const char (&prefix)[N]) {
        return _prefixId.getNextId(prefix);
    }

    void enterElemMatch(const MatchExpression::MatchType matchType) {
        _elemMatchStack.push_back(matchType);
    }

    void exitElemMatch() {
        tassert(6809501, "Attempting to exit out of elemMatch that was not entered", inElemMatch());
        _elemMatchStack.pop_back();
    }

    bool inElemMatch() {
        return !_elemMatchStack.empty();
    }

    /**
     * Returns whether the current $elemMatch should consider its path for translation. This
     * function assumes that 'enterElemMatch' has been called before visiting the current
     * expression.
     */
    bool shouldGeneratePathForElemMatch() const {
        return _elemMatchStack.size() == 1 ||
            _elemMatchStack[_elemMatchStack.size() - 2] ==
            MatchExpression::MatchType::ELEM_MATCH_OBJECT;
    }

    /**
     * Returns true if the current expression should consider its path for translation based on
     * whether it's contained within an ElemMatchObjectExpression.
     */
    bool shouldGeneratePath() const {
        return _elemMatchStack.empty() ||
            _elemMatchStack.back() == MatchExpression::MatchType::ELEM_MATCH_OBJECT;
    }

private:
    const bool _assertExprSort;
    const bool _assertPathSort;

    // The name of the input projection on which the top-level expression will be evaluated.
    const ProjectionName _rootProjection;
    const ABT _rootProjVar;

    // Used to vend out unique strings for projection names.
    PrefixId& _prefixId;

    // Used to track the parts of the expression tree that have so far been translated to ABT.
    // Maintained as a stack so parent expressions can easily compose the ABTs representing their
    // child expressions.
    std::stack<ABT> _stack;

    // Used to track expressions contained under an $elemMatch. Each entry is either an
    // ELEM_MATCH_OBJECT or ELEM_MATCH_VALUE.
    std::vector<MatchExpression::MatchType> _elemMatchStack;
};

}  // namespace mongo::optimizer
