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

#include <vector>

#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"


namespace mongo::optimizer {
/**
 * This class lowers projection paths (aka EvalPath) to simple expressions.
 */
class EvalPathLowering {
public:
    EvalPathLowering(PrefixId& prefixId) : _prefixId(prefixId) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    void transport(ABT&, const T&, Ts&&...) {
        static_assert(!std::is_base_of_v<PathSyntaxSort, T>,
                      "Path elements must define their transport");
    }

    void transport(ABT& n, const PathConstant&, ABT& c);
    void transport(ABT& n, const PathIdentity&);
    void transport(ABT& n, const PathLambda&, ABT& lam);
    void transport(ABT& n, const PathDefault&, ABT& c);
    void transport(ABT& n, const PathCompare&, ABT& c);
    void transport(ABT& n, const PathDrop&);
    void transport(ABT& n, const PathKeep&);
    void transport(ABT& n, const PathObj&);
    void transport(ABT& n, const PathArr&);

    void transport(ABT& n, const PathTraverse&, ABT& inner);

    void transport(ABT& n, const PathGet&, ABT& inner);
    void transport(ABT& n, const PathField&, ABT& inner);

    void transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2);
    void transport(ABT& n, const PathComposeA&, ABT& p1, ABT& p2);

    void transport(ABT& n, const EvalPath&, ABT& path, ABT& input);

    // The tree is passed in as NON-const reference as we will be updating it.
    // Returns true if the tree changed.
    bool optimize(ABT& n);

private:
    // We don't own these.
    PrefixId& _prefixId;

    bool _changed{false};
};

/**
 * This class lowers match/filter paths (aka EvalFilter) to simple expressions.
 */
class EvalFilterLowering {
public:
    EvalFilterLowering(PrefixId& prefixId) : _prefixId(prefixId) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    void transport(ABT&, const T&, Ts&&...) {
        static_assert(!std::is_base_of_v<PathSyntaxSort, T>,
                      "Path elements must define their transport");
    }

    void transport(ABT& n, const PathConstant&, ABT& c);
    void transport(ABT& n, const PathIdentity&);
    void transport(ABT& n, const PathLambda&, ABT& lam);
    void transport(ABT& n, const PathDefault&, ABT& c);
    void transport(ABT& n, const PathCompare&, ABT& c);
    void transport(ABT& n, const PathDrop&);
    void transport(ABT& n, const PathKeep&);
    void transport(ABT& n, const PathObj&);
    void transport(ABT& n, const PathArr&);

    void prepare(ABT& n, const PathTraverse& t);
    void transport(ABT& n, const PathTraverse&, ABT& inner);

    void transport(ABT& n, const PathGet&, ABT& inner);
    void transport(ABT& n, const PathField&, ABT& inner);

    void transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2);
    void transport(ABT& n, const PathComposeA&, ABT& p1, ABT& p2);

    void transport(ABT& n, const EvalFilter&, ABT& path, ABT& input);

    // The tree is passed in as NON-const reference as we will be updating it.
    // Returns true if the tree changed.
    bool optimize(ABT& n);

private:
    // We don't own these.
    PrefixId& _prefixId;

    std::vector<ABT::reference_type> _traverseStack;

    bool _changed{false};
};

class PathLowering {
public:
    PathLowering(PrefixId& prefixId)
        : _prefixId(prefixId), _project(_prefixId), _filter(_prefixId) {}

    // The default noop transport.
    template <typename T, typename... Ts>
    void transport(ABT&, const T&, Ts&&...) {}

    void transport(ABT& n, const EvalPath&, ABT&, ABT&);
    void transport(ABT& n, const EvalFilter&, ABT&, ABT&);

    // Returns true if the tree changed.
    bool optimize(ABT& n);

private:
    // We don't own these.
    PrefixId& _prefixId;

    EvalPathLowering _project;
    EvalFilterLowering _filter;

    bool _changed{false};
};
}  // namespace mongo::optimizer
