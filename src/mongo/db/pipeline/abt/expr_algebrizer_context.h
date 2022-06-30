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

#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

class ExpressionAlgebrizerContext {
public:
    ExpressionAlgebrizerContext(bool assertExprSort,
                                bool assertPathSort,
                                const std::string& rootProjection,
                                const std::string& uniqueIdPrefix);

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

    const std::string& getRootProjection() const;
    const ABT& getRootProjVar() const;

    const std::string& getUniqueIdPrefix() const;

    /**
     * Returns a unique string for a new projection name. It will be prefixed by 'uniqueIdPrefix'.
     */
    std::string getNextId(const std::string& key);

private:
    const bool _assertExprSort;
    const bool _assertPathSort;

    // The name of the input projection on which the top-level expression will be evaluated.
    const std::string _rootProjection;
    const ABT _rootProjVar;

    // Used to vend out unique strings for projection names.
    const std::string _uniqueIdPrefix;
    PrefixId _prefixId;

    // Used to track the parts of the expression tree that have so far been translated to ABT.
    // Maintained as a stack so parent expressions can easily compose the ABTs representing their
    // child expressions.
    std::stack<ABT> _stack;
};

}  // namespace mongo::optimizer
