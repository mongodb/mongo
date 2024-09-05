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

#include <boost/none.hpp>
#include <map>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/query/optimizer/comparison_op.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/utils/strong_alias.h"
#include "mongo/util/assert_util.h"


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
    if (maxDepth == 1 && minDepth == maxDepth) {
        // When maxDepth = 1, a conjunctive path is not decomposed into a sequence of FilterNodes.
        // In this case the path is attached directly to a single FilterNode.
        return make<FilterNode>(make<EvalFilter>(path, pathInput), input);
    }

    ABT::reference_type subPathRef = path.ref();
    FieldPathType fieldPath;
    while (const auto newPath = subPathRef.cast<PathGet>()) {
        fieldPath.push_back(newPath->name());
        subPathRef = newPath->getPath().ref();
    }

    ABT subPath{subPathRef};
    if (auto composition = collectComposedBounded(subPath, maxDepth);
        composition.size() >= minDepth) {
        // Remove the path composition and insert separate filter nodes.
        ABT result = input;
        for (const auto& element : composition) {
            result = make<FilterNode>(
                make<EvalFilter>(appendFieldPath(fieldPath, element.copy()), pathInput),
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

bool pathEndsInTraverse(const optimizer::ABT::reference_type path) {
    PathEndsInTraverseId t;
    return optimizer::algebra::transport<false>(path, t);
}

/**
 * Checks if all the Traverse elements of an index path have single depth.
 */
class PathTraverseSingleDepth {
public:
    bool transport(const PathTraverse& node, bool childResult) {
        return childResult && node.getMaxDepth() == PathTraverse::kSingleLevel;
    }
    bool transport(const PathGet& /*node*/, bool childResult) {
        return childResult;
    }
    bool transport(const PathIdentity& /*node*/) {
        return true;
    }
    template <typename T, typename... Ts>
    bool transport(const T& /*node*/, Ts&&...) {
        uasserted(6935101, "Index paths only consist of Get, Traverse, and Id nodes.");
        return false;
    }
    bool check(const ABT& path) {
        return algebra::transport<false>(path, *this);
    }
};

bool checkPathTraverseSingleDepth(const ABT& path) {
    return PathTraverseSingleDepth{}.check(path);
}

}  // namespace mongo::optimizer
