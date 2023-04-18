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

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"


namespace mongo::optimizer {

/**
 * If the input expression is a constant or a variable, or it is an EvalFilter/Path which has an
 * identity path and input which itself is constant or variable, then return a pointer to the deepst
 * simple expression.
 */
template <class T>
ABT::reference_type getTrivialExprPtr(const ABT& n) {
    if (n.is<Constant>() || n.is<Variable>()) {
        return n.ref();
    }
    if (const auto* ptr = n.cast<T>();
        ptr != nullptr && ptr->getPath().template is<PathIdentity>()) {
        return getTrivialExprPtr<T>(ptr->getInput());
    }
    return {};
}

/**
 * Returns a vector all paths nested under conjunctions (PathComposeM) in the given path.
 * For example, PathComposeM(PathComposeM(Foo, Bar), Baz) returns [Foo, Bar, Baz].
 * If the given path is not a conjunction, returns a vector with the given path.
 */
std::vector<ABT::reference_type> collectComposed(const ABT& n);

/**
 * Like collectComposed() but bounded by a maximum number of composed paths.
 * If the given path has more PathComposeM;s than specified by maxDepth, then return a vector
 * with the given path. Otherwise, returns the result of collectComposed().
 *
 * This is useful for preventing the optimizer from unintentionally creating a very deep tree which
 * causes stack-overflow on a recursive traversal.
 */
std::vector<ABT::reference_type> collectComposedBounded(const ABT& n, size_t maxDepth);

/**
 * De-compose a path and an input to an EvalFilter into sequence of Filter nodes. If we have a path
 * with a prefix of PathGet's followed by a series of nested PathComposeM, then split into two or
 * more filter nodes at the composition and retain the prefix for each. The result is a tree of
 * chained filter nodes. We return an empty result if we have less than "minDepth" sub-tress which
 * are composed. If "minDepth" = 1, then we are guaranteed to return a result, which will consist of
 * a single Filter node.
 *
 * If the number of compositions exceeds "maxDepth" then we return the a single FilterNode
 * consisting of an EvalFilter over the original path and input.
 */
constexpr size_t kMaxPathConjunctionDecomposition = 20;
boost::optional<ABT> decomposeToFilterNodes(const ABT& input,
                                            const ABT& path,
                                            const ABT& pathInput,
                                            size_t minDepth,
                                            size_t maxDepth = kMaxPathConjunctionDecomposition);

/**
 * Returns true if the path represented by 'node' is of the form PathGet "field" PathId
 */
bool isSimplePath(const ABT& node);

template <class Element = PathComposeM>
inline void maybeComposePath(ABT& composition, ABT child) {
    if (child.is<PathIdentity>()) {
        return;
    }
    if (composition.is<PathIdentity>()) {
        composition = std::move(child);
        return;
    }

    composition = make<Element>(std::move(composition), std::move(child));
}

/**
 * Creates a balanced tree of composition elements over the input vector which it modifies in place.
 * In the end at most one element remains in the vector.
 */
template <class Element = PathComposeM>
inline void maybeComposePaths(ABTVector& paths) {
    while (paths.size() > 1) {
        const size_t half = paths.size() / 2;
        for (size_t i = 0; i < half; i++) {
            maybeComposePath<Element>(paths.at(i), std::move(paths.back()));
            paths.pop_back();
        }
    }
}

/**
 * Appends a path to another path. Performs the append at PathIdentity elements.
 */
class PathAppender {
public:
    PathAppender(ABT suffix) : _suffix(std::move(suffix)) {}

    void transport(ABT& n, const PathIdentity& node) {
        n = _suffix;
    }

    template <typename T, typename... Ts>
    void transport(ABT& /*n*/, const T& /*node*/, Ts&&...) {
        // noop
    }

    /**
     * Concatenate 'prefix' and 'suffix' by modifying 'prefix' in place.
     */
    static void appendInPlace(ABT& prefix, ABT suffix) {
        PathAppender instance{std::move(suffix)};
        algebra::transport<true>(prefix, instance);
    }

    /**
     * Return the concatenation of 'prefix' and 'suffix'.
     */
    [[nodiscard]] static ABT append(ABT prefix, ABT suffix) {
        appendInPlace(prefix, std::move(suffix));
        return prefix;
    }

private:
    ABT _suffix;
};

/**
 * Given a path and a MultikeynessTrie describing the path's input,
 * removes any Traverse nodes that we know will never encounter an array.
 *
 * Returns true if any changes were made to the ABT.
 */
bool simplifyTraverseNonArray(ABT& path, const MultikeynessTrie& multikeynessTrie);

/**
 * Fuses an index path and a query path to determine a residual path to apply over the index
 * results. Checks if one index path is a prefix of another. Considers only Get, Traverse, and Id.
 * Return the suffix that doesn't match.
 */
struct IndexFusionResult {
    boost::optional<ABT::reference_type> _suffix;
    size_t _numTraversesSkipped = 0;
    size_t _numTraversesFused = 0;
};
IndexFusionResult fuseIndexPath(const ABT& node, const ABT& candidatePrefix);

/**
 * Check if a path contains a Traverse element.
 */
bool checkPathContainsTraverse(const ABT& path);

/**
 * This helper checks to see if we have a PathTraverse + PathId at the end of the path.
 */
bool pathEndsInTraverse(const optimizer::ABT& path);

}  // namespace mongo::optimizer
