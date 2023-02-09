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

#include "mongo/db/query/optimizer/utils/path_utils.h"


namespace mongo::optimizer {

std::vector<ABT::reference_type> collectComposed(const ABT& n) {
    if (auto comp = n.cast<PathComposeM>(); comp) {
        auto lhs = collectComposed(comp->getPath1());
        auto rhs = collectComposed(comp->getPath2());
        lhs.insert(lhs.end(), rhs.begin(), rhs.end());
        return lhs;
    }
    return {n.ref()};
}

// Helper function to count the size of a nested conjunction.
size_t countComposed(const ABT& n) {
    if (auto comp = n.cast<PathComposeM>()) {
        return countComposed(comp->getPath1()) + countComposed(comp->getPath2());
    }
    return 1;
}

std::vector<ABT::reference_type> collectComposedBounded(const ABT& n, size_t maxDepth) {
    if (countComposed(n) > maxDepth) {
        return {n.ref()};
    }
    return collectComposed(n);
}

static ABT appendFieldPath(const FieldPathType& fieldPath, ABT input) {
    for (size_t index = fieldPath.size(); index-- > 0;) {
        input = make<PathGet>(fieldPath.at(index), std::move(input));
    }
    return input;
}

boost::optional<ABT> decomposeToFilterNodes(const ABT& input,
                                            const ABT& path,
                                            const ABT& pathInput,
                                            const size_t minDepth,
                                            const size_t maxDepth) {
    ABT::reference_type subPathRef = path.ref();
    FieldPathType fieldPath;
    while (const auto newPath = subPathRef.cast<PathGet>()) {
        fieldPath.push_back(newPath->name());
        subPathRef = newPath->getPath().ref();
    }

    ABT subPath = subPathRef;
    if (auto composition = collectComposedBounded(subPath, maxDepth);
        composition.size() >= minDepth) {
        // Remove the path composition and insert two filter nodes.
        ABT result = input;
        for (const auto& element : composition) {
            result =
                make<FilterNode>(make<EvalFilter>(appendFieldPath(fieldPath, element), pathInput),
                                 std::move(result));
        }
        return result;
    }

    return boost::none;
}

bool isSimplePath(const ABT& node) {
    if (auto getPtr = node.cast<PathGet>();
        getPtr != nullptr && getPtr->getPath().is<PathIdentity>()) {
        return true;
    }
    return false;
}

/**
 * Removes Traverse nodes from a single path, using MultikeynessTrie which tells us
 * which child paths are never applied to an array.
 */
class MultikeynessSimplifier {
public:
    bool operator()(ABT&, PathIdentity&, const MultikeynessTrie&, bool /*skippedParentTraverse*/) {
        // No simplifications apply here.
        return false;
    }

    bool operator()(ABT& path,
                    PathGet& get,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        if (auto it = trie.children.find(get.name()); it != trie.children.end()) {
            return get.getPath().visit(*this, it->second, false /*skippedParentTraverse*/);
        } else {
            return false;
        }
    }

    bool operator()(ABT& path,
                    PathTraverse& traverse,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        tassert(6859603,
                "Unexpected maxDepth for Traverse in MultikeynessSimplifier",
                traverse.getMaxDepth() == PathTraverse::kSingleLevel);

        if (!trie.isMultiKey) {
            // This path is never applied to an array: we can remove any number of Traverse nodes,
            // of any maxDepth.
            path = std::exchange(traverse.getPath(), make<Blackhole>());
            // The parent can't have been a Traverse that we skipped, because we would have
            // removed it, because !trie.isMultiKey.
            invariant(!skippedParentTraverse);
            path.visit(*this, trie, false /*skippedParentTraverse*/);
            return true;
        } else if (traverse.getMaxDepth() == PathTraverse::kSingleLevel && !skippedParentTraverse) {
            // This path is possibly multikey, so we can't remove any Traverse nodes.
            // But each edge in the trie represents a 'Traverse [1] Get [a]', so we can
            // skip a single Traverse [1] node.
            return traverse.getPath().visit(*this, trie, true /*skippedParentTraverse*/);
        } else {
            // We have no information about multikeyness of the child path.
            return false;
        }
    }

    bool operator()(ABT& path,
                    PathLambda& pathLambda,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        // Look for PathLambda Lambda [tmp] UnaryOp [Not] EvalFilter <path> Variable [tmp],
        // and simplify <path>.  This works because 'tmp' is the same variable name in both places,
        // so <path> is applied to the same input as the PathLambda. (And the 'trie' tells us
        // which parts of that input are not arrays.)

        // In the future we may want to generalize this to skip over other expressions besides Not,
        // as long as the Lambda and EvalFilter are connected by a variable.

        if (auto* lambda = pathLambda.getLambda().cast<LambdaAbstraction>()) {
            if (auto* unary = lambda->getBody().cast<UnaryOp>();
                unary && unary->op() == Operations::Not) {
                if (auto* evalFilter = unary->getChild().cast<EvalFilter>()) {
                    if (auto* variable = evalFilter->getInput().cast<Variable>();
                        variable && variable->name() == lambda->varName()) {
                        return evalFilter->getPath().visit(
                            *this, trie, false /*skippedParentTraverse*/);
                    }
                }
            }
        }
        return false;
    }

    bool operator()(ABT& path,
                    PathComposeM& compose,
                    const MultikeynessTrie& trie,
                    bool skippedParentTraverse) {
        const bool simplified1 = compose.getPath1().visit(*this, trie, skippedParentTraverse);
        const bool simplified2 = compose.getPath2().visit(*this, trie, skippedParentTraverse);
        return simplified1 || simplified2;
    }

    template <typename T, typename... Ts>
    bool operator()(ABT& n, T& /*node*/, Ts&&...) {
        // Don't optimize a node we don't recognize.
        return false;

        // Some other cases to consider:
        // - Remove PathArr for non-multikey paths.
        // - Descend into disjunction.
        // - Descend into PathLambda and simplify expressions, especially Not and EvalFilter.
    }

    static bool simplify(ABT& path, const MultikeynessTrie& trie) {
        MultikeynessSimplifier instance;
        return path.visit(instance, trie, false /*skippedParentTraverse*/);
    }
};

bool simplifyTraverseNonArray(ABT& path, const MultikeynessTrie& multikeynessTrie) {
    return MultikeynessSimplifier::simplify(path, multikeynessTrie);
}

class IndexPathFusor {
public:
    /**
     * 'n' - The complete index path being compared to, can be modified if needed.
     * 'node' - Same as 'n' but cast to a specific type by the caller in order to invoke the
     *   correct operator.
     * 'other' - The query path, of which the index may satisfy a prefix.
     */
    IndexFusionResult operator()(const ABT& n, const PathGet& node, const ABT& other) {
        if (auto otherGet = other.cast<PathGet>();
            otherGet != nullptr && otherGet->name() == node.name()) {
            if (auto otherChildTraverse = otherGet->getPath().cast<PathTraverse>();
                otherChildTraverse != nullptr && !node.getPath().is<PathTraverse>()) {
                // If a query path has a Traverse, but the index path doesn't, the query can
                // still be evaluated by this index. Skip the Traverse node, and continue matching.
                // This works because we know the Traverse will never be applied to an array,
                // so 'Traverse [anything] p == p'.
                auto result = node.getPath().visit(*this, otherChildTraverse->getPath());
                result._numTraversesSkipped++;
                return result;
            } else {
                return node.getPath().visit(*this, otherGet->getPath());
            }
        }
        return {};
    }

    IndexFusionResult operator()(const ABT& n, const PathTraverse& node, const ABT& other) {
        if (auto otherTraverse = other.cast<PathTraverse>();
            otherTraverse != nullptr && otherTraverse->getMaxDepth() == node.getMaxDepth()) {
            auto result = node.getPath().visit(*this, otherTraverse->getPath());
            result._numTraversesFused++;
            return result;
        }
        return {};
    }

    IndexFusionResult operator()(const ABT& n, const PathIdentity& node, const ABT& other) {
        return {other.ref()};
    }

    template <typename T, typename... Ts>
    IndexFusionResult operator()(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        uasserted(6624152, "Unexpected node type");
    }
};

IndexFusionResult fuseIndexPath(const ABT& node, const ABT& candidatePrefix) {
    IndexPathFusor instance;
    return candidatePrefix.visit(instance, node);
}

/**
 * Check if an index path contains a Traverse element.
 */
class PathTraverseChecker {
public:
    PathTraverseChecker() {}

    bool transport(const ABT& /*n*/, const PathTraverse& /*node*/, bool /*childResult*/) {
        return true;
    }

    bool transport(const ABT& /*n*/, const PathGet& /*node*/, bool childResult) {
        return childResult;
    }

    bool transport(const ABT& /*n*/, const PathIdentity& /*node*/) {
        return false;
    }

    template <typename T, typename... Ts>
    bool transport(const ABT& /*n*/, const T& /*node*/, Ts&&...) {
        uasserted(6624153, "Index paths only consist of Get, Traverse, and Id nodes.");
        return false;
    }

    bool check(const ABT& path) {
        return algebra::transport<true>(path, *this);
    }
};

bool checkPathContainsTraverse(const ABT& path) {
    return PathTraverseChecker{}.check(path);
}

/**
 * Checks if a path ends in a Traverse + PathId.
 */
class PathEndsInTraverseId {
public:
    bool transport(const optimizer::PathTraverse& node, bool childResult) {
        return node.getPath().is<PathIdentity>() || childResult;
    }

    bool transport(const optimizer::PathGet& /*node*/, bool childResult) {
        return childResult;
    }

    bool transport(const optimizer::PathIdentity& /*node*/) {
        return false;
    }

    template <typename T, typename... Ts>
    bool transport(const T& node, Ts&&... /* args */) {
        uasserted(6749500, "Unexpected node in transport to check if path is $elemMatch.");
    }
};

bool pathEndsInTraverse(const optimizer::ABT& path) {
    PathEndsInTraverseId t;
    return optimizer::algebra::transport<false>(path, t);
}

}  // namespace mongo::optimizer
