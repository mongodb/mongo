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

#include "mongo/db/query/optimizer/reference_tracker.h"

namespace mongo::optimizer {

/**
 * Performs fusion rewrites over paths. Those attempt to simplify complex paths.
 */
class PathFusion {
    enum class Type { unknown, nothing, object, array, boolean, any };
    enum class Kind { project, filter };

    struct CollectedInfo {
        bool isNotNothing() const {
            return _type != Type::unknown && _type != Type::nothing;
        }

        Type _type{Type::unknown};

        // Is the result of the path independent of its input (e.g. can be if the paths terminates
        // with PathConst, but not necessarily with PathIdentity).
        bool _isConst{false};
    };

public:
    PathFusion(VariableEnvironment& env) : _env(env) {}

    template <typename T, typename... Ts>
    void transport(ABT&, const T& op, Ts&&...) {
        if constexpr (std::is_base_of_v<PathSyntaxSort, T>) {
            _info[&op] = CollectedInfo{};
        }
    }

    void transport(ABT& n, const PathConstant&, ABT& c);
    void transport(ABT& n, const PathCompare&, ABT& c);
    void transport(ABT& n, const PathGet&, ABT& path);
    void transport(ABT& n, const PathField&, ABT& path);
    void transport(ABT& n, const PathTraverse&, ABT& inner);
    void transport(ABT& n, const PathComposeM&, ABT& p1, ABT& p2);

    void prepare(ABT& n, const EvalPath& eval) {
        _kindCtx.push_back(Kind::project);
    }
    void transport(ABT& n, const EvalPath& eval, ABT& path, ABT& input);
    void prepare(ABT& n, const EvalFilter& eval) {
        _kindCtx.push_back(Kind::filter);
    }
    void transport(ABT& n, const EvalFilter& eval, ABT& path, ABT& input);

    bool optimize(ABT& root);

private:
    ABT::reference_type follow(ABT::reference_type n);
    ABT::reference_type follow(const ABT& n) {
        return follow(n.ref());
    }
    bool fuse(ABT& lhs, const ABT& rhs);

    /**
     * Attempt to eliminate a chain of Fields with constant children.
     */
    void tryFuseComposition(ABT& n, ABT& input);

    VariableEnvironment& _env;
    opt::unordered_map<const PathSyntaxSort*, CollectedInfo> _info;
    opt::unordered_set<const PathSyntaxSort*> _redundant;

    // A stack of context (either project or filter path)
    std::vector<Kind> _kindCtx;
    bool _changed{false};
};
}  // namespace mongo::optimizer
