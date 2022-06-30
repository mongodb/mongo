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

#include "mongo/db/pipeline/abt/expr_algebrizer_context.h"

namespace mongo::optimizer {

ExpressionAlgebrizerContext::ExpressionAlgebrizerContext(const bool assertExprSort,
                                                         const bool assertPathSort,
                                                         const std::string& rootProjection,
                                                         const std::string& uniqueIdPrefix)
    : _assertExprSort(assertExprSort),
      _assertPathSort(assertPathSort),
      _rootProjection(rootProjection),
      _rootProjVar(make<Variable>(_rootProjection)),
      _uniqueIdPrefix(uniqueIdPrefix),
      _prefixId() {}

void ExpressionAlgebrizerContext::push(ABT node) {
    if (_assertExprSort) {
        assertExprSort(node);
    } else if (_assertPathSort) {
        assertPathSort(node);
    }

    _stack.emplace(node);
}

ABT ExpressionAlgebrizerContext::pop() {
    uassert(6624428, "Arity violation", !_stack.empty());

    ABT node = _stack.top();
    _stack.pop();
    return node;
}

void ExpressionAlgebrizerContext::ensureArity(const size_t arity) {
    uassert(6624429, "Arity violation", _stack.size() >= arity);
}

const std::string& ExpressionAlgebrizerContext::getRootProjection() const {
    return _rootProjection;
}

const ABT& ExpressionAlgebrizerContext::getRootProjVar() const {
    return _rootProjVar;
}

const std::string& ExpressionAlgebrizerContext::getUniqueIdPrefix() const {
    return _uniqueIdPrefix;
}

std::string ExpressionAlgebrizerContext::getNextId(const std::string& key) {
    return getUniqueIdPrefix() + "_" + _prefixId.getNextId(key);
}

}  // namespace mongo::optimizer
