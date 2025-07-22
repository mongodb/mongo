/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/match_path.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_ast.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/abt/comparison_op.h"
#include "mongo/db/query/stage_builder/sbe/builder_state.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mongo::projection_ast {
class Projection;
}

namespace mongo {
class AccumulationStatement;
struct WindowFunctionStatement;
}  // namespace mongo

namespace mongo::stage_builder {

class PlanStageSlots;
struct Environment;
struct PlanStageStaticData;

/**
 * Generates an expression that returns shard key that behaves similarly to
 * ShardKeyPattern::extractShardKeyFromDoc. However, it will not check for arrays in shard key, as
 * it is used only for documents that are already persisted in a sharded collection
 */
SbExpr makeShardKeyForPersistedDocuments(StageBuilderState& state,
                                         const std::vector<sbe::MatchPath>& shardKeyPaths,
                                         const std::vector<bool>& shardKeyHashed,
                                         const PlanStageSlots& slots);

/**
 * Helper functions for converting from BSONObj/BSONArray to SBE Object/Array. Caller owns the SBE
 * Object/Array returned. These helper functions do not assume ownership of the BSONObj/BSONArray.
 */
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONObj& bo);
std::pair<sbe::value::TypeTags, sbe::value::Value> makeValue(const BSONArray& ba);

/**
 * Returns a BSON type mask of all data types coercible to date.
 */
uint32_t dateTypeMask();

struct BuildSortKeysPlan {
    enum Type {
        kTraverseFields,
        kCallGenSortKey,
        kCallGenCheapSortKey,
    };

    Type type = kCallGenSortKey;
    bool needsResultObj = true;
    std::vector<std::string> fieldsForSortKeys;
};

struct SortKeysExprs {
    SbExpr::Vector keyExprs;
    SbExpr parallelArraysCheckExpr;
    SbExpr fullKeyExpr;
};

BuildSortKeysPlan makeSortKeysPlan(const SortPattern& sortPattern,
                                   bool allowCallGenCheapSortKey = false);

SortKeysExprs buildSortKeys(StageBuilderState& state,
                            const BuildSortKeysPlan& plan,
                            const SortPattern& sortPattern,
                            const PlanStageSlots& outputs,
                            SbExpr sortSpecExpr = {});

struct UnfetchedIxscans {
    std::vector<const QuerySolutionNode*> ixscans;
    bool hasFetchesOrCollScans;
};

boost::optional<UnfetchedIxscans> getUnfetchedIxscans(const QuerySolutionNode* root);

/**
 * Retrieves the accumulation op name from 'accStmt' and returns it.
 */
StringData getAccumulationOpName(const AccumulationStatement& accStmt);

/**
 * Retrieves the window function op name from 'accStmt' and returns it.
 */
StringData getWindowFunctionOpName(const WindowFunctionStatement& wfStmt);

/**
 * Return true iff 'name', 'accStmt', or 'wfStmt' is one of $topN, $bottomN, $minN, $maxN,
 * $firstN, or $lastN.
 */
bool isAccumulatorN(StringData name);
bool isAccumulatorN(const AccumulationStatement& accStmt);
bool isAccumulatorN(const WindowFunctionStatement& wfStmt);

/**
 * Return true iff 'name', 'accStmt', or 'wfStmt' is $topN or $bottomN.
 */
bool isTopBottomN(StringData name);
bool isTopBottomN(const AccumulationStatement& accStmt);
bool isTopBottomN(const WindowFunctionStatement& wfStmt);

/**
 * Gets the internal pointer to the SortPattern (if there is one) inside 'accStmt' or 'wfStmt'.
 */
boost::optional<SortPattern> getSortPattern(const AccumulationStatement& accStmt);
boost::optional<SortPattern> getSortPattern(const WindowFunctionStatement& wfStmt);

/**
 * Creates a SortSpec object from a SortPattern.
 */
std::unique_ptr<sbe::SortSpec> makeSortSpecFromSortPattern(const SortPattern& sortPattern);
std::unique_ptr<sbe::SortSpec> makeSortSpecFromSortPattern(
    const boost::optional<SortPattern>& sortPattern);

std::tuple<SbStage, SbSlot, SbSlot, SbSlotVector> makeLoopJoinForFetch(
    SbStage inputStage,
    std::vector<std::string> fields,
    SbSlot seekRecordIdSlot,
    SbSlot snapshotIdSlot,
    SbSlot indexIdentSlot,
    SbSlot indexKeySlot,
    SbSlot indexKeyPatternSlot,
    boost::optional<SbSlot> prefetchedResultSlot,
    const CollectionPtr& collToFetch,
    StageBuilderState& state,
    PlanNodeId planNodeId,
    SbSlotVector slotsToForward);

/**
 * Given an index key pattern, and a subset of the fields of the index key pattern that are depended
 * on to compute the query, returns the corresponding 'IndexKeysInclusionSet' bit vector and field
 * name vector.
 *
 * For example, suppose that we have an index key pattern {d: 1, c: 1, b: 1, a: 1}, and the caller
 * depends on the set of 'requiredFields' {"b", "d"}. In this case, the pair of return values would
 * be:
 *  - 'IndexKeysInclusionSet' bit vector of 1010
 *  - Field name vector of <"d", "b">
 */
template <typename T>
std::pair<sbe::IndexKeysInclusionSet, std::vector<std::string>> makeIndexKeyInclusionSet(
    const BSONObj& indexKeyPattern, const T& requiredFields) {
    sbe::IndexKeysInclusionSet indexKeyBitset;
    std::vector<std::string> keyFieldNames;
    size_t i = 0;
    for (auto&& elt : indexKeyPattern) {
        if (requiredFields.count(elt.fieldName())) {
            indexKeyBitset.set(i);
            keyFieldNames.push_back(elt.fieldName());
        }

        ++i;
    }

    return {std::move(indexKeyBitset), std::move(keyFieldNames)};
}

inline bool pathIsPrefixOf(StringData lhs, StringData rhs) {
    return lhs.size() < rhs.size() ? rhs.starts_with(lhs) && rhs[lhs.size()] == '.' : lhs == rhs;
}

inline bool pathsAreConflicting(StringData lhs, StringData rhs) {
    return lhs.size() < rhs.size() ? pathIsPrefixOf(lhs, rhs) : pathIsPrefixOf(rhs, lhs);
}

/**
 * A tree of nodes arranged based on field path. PathTreeNode can be used to represent index key
 * patterns, projections, etc. A PathTreeNode can also optionally hold a value of type T.
 *
 * For example, the key pattern {a.b: 1, x: 1, a.c: 1} in tree form would look like:
 *
 *         <root>
 *         /   |
 *        a    x
 *       / \
 *      b   c
 */
template <typename T>
struct PathTreeNode {
    PathTreeNode() = default;
    explicit PathTreeNode(std::string name) : name(std::move(name)) {}

    // Aside from the root node, it is very common for a node to have no children or only 1 child.
    using ChildrenVector = absl::InlinedVector<std::unique_ptr<PathTreeNode<T>>, 1>;

    // It is the caller's responsibility to verify that there is not an existing field with the
    // same name as 'fieldComponent'.
    PathTreeNode<T>* emplace_back(std::string fieldComponent) {
        auto newNode = std::make_unique<PathTreeNode<T>>(std::move(fieldComponent));
        const auto newNodeRaw = newNode.get();
        children.emplace_back(std::move(newNode));

        if (childrenMap) {
            childrenMap->emplace(newNodeRaw->name, newNodeRaw);
        } else if (children.size() >= 3) {
            // If 'childrenMap' is null and there are 3 or more children, build 'childrenMap' now.
            buildChildrenMap();
        }

        return newNodeRaw;
    }

    bool isLeaf() const {
        return children.empty();
    }

    PathTreeNode<T>* findChild(StringData fieldComponent) {
        if (childrenMap) {
            auto it = childrenMap->find(fieldComponent);
            return it != childrenMap->end() ? it->second : nullptr;
        }
        for (auto&& child : children) {
            if (child->name == fieldComponent) {
                return child.get();
            }
        }
        return nullptr;
    }

    PathTreeNode<T>* findNode(const sbe::MatchPath& fieldRef, size_t currentIndex = 0) {
        if (currentIndex == fieldRef.numParts()) {
            return this;
        }

        auto currentPart = fieldRef.getPart(currentIndex);
        if (auto child = findChild(currentPart)) {
            return child->findNode(fieldRef, currentIndex + 1);
        } else {
            return nullptr;
        }
    }

    /**
     * Returns leaf node matching field path. If the field path provided resolves to a non-leaf
     * node, null will be returned. For example, if tree was built for key pattern {a.b: 1}, this
     * method will return nullptr for field path "a".
     */
    PathTreeNode<T>* findLeafNode(const sbe::MatchPath& fieldRef, size_t currentIndex = 0) {
        auto* node = findNode(fieldRef, currentIndex);
        return (node && node->isLeaf() ? node : nullptr);
    }

    void clearChildren() {
        children.clear();
        childrenMap.reset();
    }

    void buildChildrenMap() {
        if (!childrenMap) {
            childrenMap = std::make_unique<StringDataMap<PathTreeNode<T>*>>();
            for (auto&& child : children) {
                childrenMap->insert_or_assign(child->name, child.get());
            }
        }
    }

    std::string name;

    ChildrenVector children;

    // We only build a hash map when there are 3 or more children. The vast majority of nodes
    // will have 2 children or less, so we dynamically allocate 'childrenMap' to save space.
    std::unique_ptr<StringDataMap<PathTreeNode<T>*>> childrenMap;

    T value = {};
};

using SlotTreeNode = PathTreeNode<boost::optional<SbSlot>>;

std::unique_ptr<SlotTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                  const SbSlotVector& slots);

SbExpr buildNewObjExpr(StageBuilderState& state, const SlotTreeNode* slotTree);

SbExpr rehydrateIndexKey(StageBuilderState& state,
                         const BSONObj& indexKeyPattern,
                         const SbSlotVector& indexKeySlots);

template <typename T>
inline const char* getRawStringData(const T& str) {
    if constexpr (std::is_same_v<T, StringData>) {
        return str.data();
    } else {
        return str.data();
    }
}

enum class BuildPathTreeMode {
    AllowConflictingPaths,
    RemoveConflictingPaths,
    AssertNoConflictingPaths,
};

template <typename T, typename IterT, typename StringT>
inline std::unique_ptr<PathTreeNode<T>> buildPathTreeImpl(const std::vector<StringT>& paths,
                                                          boost::optional<IterT> valsBegin,
                                                          boost::optional<IterT> valsEnd,
                                                          BuildPathTreeMode mode) {
    auto tree = std::make_unique<PathTreeNode<T>>();
    auto valsIt = std::move(valsBegin);

    for (auto&& pathStr : paths) {
        auto path = sbe::MatchPath{pathStr};

        size_t numParts = path.numParts();
        size_t i = 0;

        auto* node = tree.get();
        StringData part;
        for (; i < numParts; ++i) {
            part = path.getPart(i);
            auto child = node->findChild(part);
            if (!child) {
                break;
            }
            node = child;
        }

        if (mode == BuildPathTreeMode::AssertNoConflictingPaths) {
            // If mode == AssertNoConflictingPaths, assert that no conflicting paths exist as
            // we build the path tree.
            tassert(7580701,
                    "Expected 'paths' to not contain any conflicting paths",
                    (node->isLeaf() && i == 0) || (!node->isLeaf() && i < numParts));
        } else if (mode == BuildPathTreeMode::RemoveConflictingPaths) {
            if (node->isLeaf() && i != 0) {
                // If 'mode == RemoveConflictingPaths' and we're about to process a path P and we've
                // already processed some other path that is a prefix of P, then ignore P.
                if (valsIt) {
                    ++(*valsIt);
                }

                continue;
            } else if (!node->isLeaf() && i == numParts) {
                // If 'mode == RemoveConflictingPaths' and we're about to process a path P that is a
                // prefix of another path that's already been processed, then delete the children of
                // 'node' to remove the longer conflicting path(s).
                node->clearChildren();
            }
        }

        if (i < numParts) {
            node = node->emplace_back(std::string(part));
            for (++i; i < numParts; ++i) {
                node = node->emplace_back(std::string(path.getPart(i)));
            }
        }

        if (valsIt) {
            tassert(7182003,
                    "Did not expect iterator 'valsIt' to reach the end yet",
                    !valsEnd || *valsIt != *valsEnd);

            node->value = **valsIt;

            ++(*valsIt);
        }
    }

    return tree;
}

/**
 * Builds a path tree from a set of paths and returns the root node of the tree.
 *
 * If 'mode == AllowConflictingPaths', this function will build a tree that contains all paths
 * specified in 'paths' (regardless of whether there are any paths that conflict).
 *
 * If 'mode == RemoveConflictingPaths', when there are two conflicting paths (ex. "a" and "a.b")
 * the conflict is resolved by removing the longer path ("a.b") and keeping the shorter path ("a").
 *
 * If 'mode == AssertNoConflictingPaths', this function will tassert() if it encounters any
 * conflicting paths.
 */
template <typename T, typename StringT>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<StringT>& paths,
                                               BuildPathTreeMode mode) {
    return buildPathTreeImpl<T, std::move_iterator<typename std::vector<T>::iterator>, StringT>(
        paths, boost::none, boost::none, mode);
}

/**
 * Builds a path tree from a set of paths, assigns a sequence of values to the sequence of nodes
 * corresponding to each path, and returns the root node of the tree.
 *
 * The 'values' sequence and the 'paths' vector are expected to have the same number of elements.
 * The kth value in the 'values' sequence will be assigned to the node corresponding to the kth path
 * in 'paths'.
 *
 * If 'mode == AllowConflictingPaths', this function will build a tree that contains all paths
 * specified in 'paths' (regardless of whether there are any paths that conflict).
 *
 * If 'mode == RemoveConflictingPaths', when there are two conflicting paths (ex. "a" and "a.b")
 * the conflict is resolved by removing the longer path ("a.b") and keeping the shorter path ("a").
 *
 * If 'mode == AssertNoConflictingPaths', this function will tassert() if it encounters any
 * conflicting paths.
 */
template <typename T, typename IterT, typename StringT>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<StringT>& paths,
                                               IterT valuesBegin,
                                               IterT valuesEnd,
                                               BuildPathTreeMode mode) {
    return buildPathTreeImpl<T, IterT, StringT>(
        paths, std::move(valuesBegin), std::move(valuesEnd), mode);
}

template <typename T, typename U>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<std::string>& paths,
                                               const std::vector<U>& values,
                                               BuildPathTreeMode mode) {
    tassert(7182004,
            "buildPathTree() expects 'paths' and 'values' to be the same size",
            paths.size() == values.size());

    return buildPathTree<T>(paths, values.begin(), values.end(), mode);
}

template <typename T, typename U>
std::unique_ptr<PathTreeNode<T>> buildPathTree(const std::vector<std::string>& paths,
                                               std::vector<U>&& values,
                                               BuildPathTreeMode mode) {
    tassert(7182005,
            "buildPathTree() expects 'paths' and 'values' to be the same size",
            paths.size() == values.size());

    return buildPathTree<T>(paths,
                            std::make_move_iterator(values.begin()),
                            std::make_move_iterator(values.end()),
                            mode);
}

/**
 * If a boolean can be constructed from type T, this function will construct a boolean from 'value'
 * and then return the negation. If a boolean cannot be constructed from type T, then this function
 * returns false.
 */
template <typename T>
bool convertsToFalse(const T& value) {
    if constexpr (std::is_constructible_v<bool, T>) {
        return !bool(value);
    } else {
        return false;
    }
}

template <typename T>
struct InvokeAndReturnBoolHelper {
    template <typename FuncT, typename... Args>
    static bool invoke(FuncT&& fn, bool defaultReturnValue, Args&&... args) {
        (std::forward<FuncT>(fn))(std::forward<Args>(args)...);
        return defaultReturnValue;
    }
};
template <>
struct InvokeAndReturnBoolHelper<bool> {
    template <typename FuncT, typename... Args>
    static bool invoke(FuncT&& fn, bool, Args&&... args) {
        return (std::forward<FuncT>(fn))(std::forward<Args>(args)...);
    }
};

/**
 * This function will invoke an invocable object ('fn') with the specified arguments ('args'). If
 * 'fn' returns bool, this function will return fn's return value. If 'fn' returns void or some type
 * other than bool, this function will return 'defaultReturnValue'.
 *
 * As a special case, this function alows fn's type (FuncT) to be nullptr_t. In such cases, this
 * function will do nothing and it will return 'defaultReturnValue'.
 *
 * If 'fn' is not invocable with the specified arguments and fn's type is not nullptr_t, this
 * function will raise a static assertion.
 *
 * Note that when a bool can be constructed from 'fn' (for example, if FuncT is a function pointer
 * type), this method will always invoke 'fn' regardless of whether "!bool(fn)" is true or false.
 * It is the caller's responsibility to do any necessary checks (ex. null checks) before calling
 * this function.
 */
template <typename FuncT, typename... Args>
inline bool invokeAndReturnBool(FuncT&& fn, bool defaultReturnValue, Args&&... args) {
    if constexpr (std::is_invocable_v<FuncT, Args...>) {
        return InvokeAndReturnBoolHelper<typename std::invoke_result<FuncT, Args...>::type>::invoke(
            std::forward<FuncT>(fn), defaultReturnValue, std::forward<Args>(args)...);
    } else {
        static_assert(std::is_null_pointer_v<std::remove_reference_t<FuncT>>);
        return defaultReturnValue;
    }
}

/**
 * This is a helper function used by visitPathTreeNodes() to invoke preVisit and postVisit callback
 * functions. This helper function will check if 'fn' supports invocation with the following args:
 *   (1) Node* node, const std::string& path, const DfsState& dfsState
 *   (2) Node* node, const DfsState& dfsState
 *   (3) Node* node, const std::string& path
 *   (4) Node* node
 *
 * After checking what 'fn' supports, this helper function will then use invokeAndReturnBool() to
 * invoke 'fn' accordingly and it will return invokeAndReturnBool()'s return value. If 'fn' supports
 * multiple signatures, whichever signature that appears first in the list above will be used.
 */
template <typename NodeT, typename FuncT>
inline bool invokeVisitPathTreeNodesCallback(
    FuncT&& fn,
    NodeT* node,
    const std::string& path,
    const std::vector<std::pair<NodeT*, size_t>>& dfsState) {
    using DfsState = std::vector<std::pair<NodeT*, size_t>>;

    if constexpr (std::is_invocable_v<FuncT, NodeT*, const std::string&, const DfsState&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, path, dfsState);
    } else if constexpr (std::is_invocable_v<FuncT, NodeT*, const DfsState&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, dfsState);
    } else if constexpr (std::is_invocable_v<FuncT, NodeT*, const std::string&>) {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node, path);
    } else {
        return invokeAndReturnBool(std::forward<FuncT>(fn), true, node);
    }
}

/**
 * This function performs a DFS traversal on a path tree (as given by 'treeRoot') and it invokes
 * the specified preVisit and postVisit callbacks at the appropriate times.
 *
 * The caller may pass nullptr for 'preVisit' if they do not wish to perform any pre-visit actions,
 * and likewise the caller may pass nullptr for 'postVisit' if they do not wish to perform any
 * post-visit actions.
 *
 * Assuming 'preVisit' is not null, the 'preVisit' callback must support one of the following
 * signatures:
 *   (1) Node* node, const std::string& path, const DfsState& dfsState
 *   (2) Node* node, const DfsState& dfsState
 *   (3) Node* node, const std::string& path
 *   (4) Node* node
 *
 * Likewise, assuming 'postVisit' is not null, the 'postVisit' callback must support one of the
 * signatures listed above. For details, see invokeVisitPathTreeNodesCallback().
 *
 * The 'preVisit' callback can return any type. If preVisit's return type is not 'bool', its return
 * value will be ignored at run time. If preVisit's return type _is_ 'bool', then its return value
 * at run time will be used to decide whether the current node should be "skipped". If preVisit()
 * returns false, then the current node will be "skipped", its descendents will not be visited (i.e.
 * instead of the DFS descending, it will backtrack), and the 'postVisit' will not be called for the
 * node.
 *
 * The 'postVisit' callback can return any type. postVisit's return value (if any) is ignored.
 *
 * If 'invokeCallbacksForRootNode' is false (which is the default), the preVisit and postVisit
 * callbacks won't be invoked for the root node of the tree. If 'invokeCallbacksForRootNode' is
 * true, the preVisit and postVisit callbacks will be invoked for the root node of the tree at
 * the appropriate times.
 *
 * The 'rootPath' parameter allows the caller to specify the absolute path of 'treeRoot', which
 * will be used as the base/prefix to determine the paths of all the other nodes in the tree. If
 * no 'rootPath' argument is provided, then 'rootPath' defaults to boost::none.
 */
template <typename T, typename PreVisitFn, typename PostVisitFn>
void visitPathTreeNodes(PathTreeNode<T>* treeRoot,
                        const PreVisitFn& preVisit,
                        const PostVisitFn& postVisit,
                        bool invokeCallbacksForRootNode = false,
                        boost::optional<std::string> rootPath = boost::none) {
    using Node = PathTreeNode<T>;
    using DfsState = std::vector<std::pair<Node*, size_t>>;
    constexpr bool isPathNeeded =
        std::is_invocable_v<PreVisitFn, Node*, const std::string&, const DfsState&> ||
        std::is_invocable_v<PreVisitFn, Node*, const std::string&> ||
        std::is_invocable_v<PostVisitFn, Node*, const std::string&, const DfsState&> ||
        std::is_invocable_v<PostVisitFn, Node*, const std::string&>;

    if (!treeRoot || (treeRoot->children.empty() && !invokeCallbacksForRootNode)) {
        return;
    }

    const bool hasPreVisit = !convertsToFalse(preVisit);
    const bool hasPostVisit = !convertsToFalse(postVisit);

    // Perform a depth-first traversal using 'dfs' to keep track of where we are.
    DfsState dfs;
    dfs.emplace_back(treeRoot, std::numeric_limits<size_t>::max());
    boost::optional<std::string> path = std::move(rootPath);
    const std::string emptyPath;

    auto getPath = [&]() -> const std::string& {
        return path ? *path : emptyPath;
    };
    auto dfsPop = [&] {
        dfs.pop_back();
        if (isPathNeeded && path) {
            if (auto pos = path->find_last_of('.'); pos != std::string::npos) {
                path->resize(pos);
            } else {
                path = boost::none;
            }
        }
    };

    if (hasPreVisit && invokeCallbacksForRootNode) {
        // Invoke the pre-visit callback on the root node if appropriate.
        if (!invokeVisitPathTreeNodesCallback(preVisit, treeRoot, getPath(), dfs)) {
            dfsPop();
        }
    }

    while (!dfs.empty()) {
        ++dfs.back().second;
        auto [node, idx] = dfs.back();
        const bool isRootNode = dfs.size() == 1;

        if (idx < node->children.size()) {
            auto child = node->children[idx].get();
            dfs.emplace_back(child, std::numeric_limits<size_t>::max());
            if (isPathNeeded) {
                if (path) {
                    path->append(1, '.');
                    *path += child->name;
                } else {
                    path = child->name;
                }
            }

            if (hasPreVisit) {
                // Invoke the pre-visit callback.
                if (!invokeVisitPathTreeNodesCallback(preVisit, child, getPath(), dfs)) {
                    dfsPop();
                }
            }
        } else {
            if (hasPostVisit && (invokeCallbacksForRootNode || !isRootNode)) {
                // Invoke the post-visit callback.
                invokeVisitPathTreeNodesCallback(postVisit, node, getPath(), dfs);
            }
            dfsPop();
        }
    }
}

/**
 * Simple tagged pointer to a projection AST node. This class provides some useful methods for
 * extracting information from the AST node.
 */
class ProjectNode {
public:
    using ASTNode = projection_ast::ASTNode;
    using BooleanConstantASTNode = projection_ast::BooleanConstantASTNode;
    using ExpressionASTNode = projection_ast::ExpressionASTNode;
    using ProjectionSliceASTNode = projection_ast::ProjectionSliceASTNode;

    enum class Type { kBool, kExpr, kSbExpr, kSlice };

    struct Bool {
        bool value;
    };
    struct Expr {
        Expression* expr;
    };
    using Slice = std::pair<int32_t, boost::optional<int32_t>>;

    using VariantType = std::variant<Bool, Expr, SbExpr, Slice>;

    struct Keep {};
    struct Drop {};

    static std::vector<Type> getNodeTypes(const std::vector<ProjectNode>& nodes) {
        std::vector<Type> nodeTypes;
        nodeTypes.reserve(nodes.size());
        for (const auto& node : nodes) {
            nodeTypes.emplace_back(node.type());
        }
        return nodeTypes;
    }

    ProjectNode() = default;

    ProjectNode(Keep) : _data(Bool{true}) {}
    ProjectNode(Drop) : _data(Bool{false}) {}
    ProjectNode(Expression* expr) : _data(Expr{expr}) {}
    ProjectNode(SbExpr sbExpr) : _data(std::move(sbExpr)) {}
    ProjectNode(Slice slice) : _data(slice) {}

    ProjectNode(const BooleanConstantASTNode* n) : _data(Bool{n->value()}) {}
    ProjectNode(const ExpressionASTNode* n) : _data(Expr{n->expressionRaw()}) {}
    ProjectNode(const ProjectionSliceASTNode* n) : _data(Slice{n->limit(), n->skip()}) {}

    ProjectNode clone() const {
        return visit(OverloadedVisitor{[](const Bool& b) {
                                           return b.value ? ProjectNode(Keep{})
                                                          : ProjectNode(Drop{});
                                       },
                                       [](const Expr& e) { return ProjectNode(e.expr); },
                                       [](const SbExpr& e) { return ProjectNode(e.clone()); },
                                       [](const Slice& s) {
                                           return ProjectNode(s);
                                       }},
                     _data);
    }

    Type type() const {
        return visit(OverloadedVisitor{[](const Bool&) { return Type::kBool; },
                                       [](const Expr&) { return Type::kExpr; },
                                       [](const SbExpr&) { return Type::kSbExpr; },
                                       [](const Slice&) {
                                           return Type::kSlice;
                                       }},
                     _data);
    }

    bool isBool() const {
        return type() == Type::kBool;
    }
    bool isExpr() const {
        return type() == Type::kExpr;
    }
    bool isSbExpr() const {
        return type() == Type::kSbExpr;
    }
    bool isSlice() const {
        return type() == Type::kSlice;
    }

    bool getBool() const {
        tassert(7580702, "getBool() expected type() to be kBool", isBool());
        return get<Bool>(_data).value;
    }
    Expression* getExpr() const {
        tassert(7580703, "getExpr() expected type() to be kExpr", isExpr());
        return get<Expr>(_data).expr;
    }
    SbExpr getSbExpr() const {
        tassert(7580715, "getSbExpr() expected type() to be kSbExpr", isSbExpr());
        return get<SbExpr>(_data).clone();
    }
    SbExpr extractSbExpr() {
        tassert(7580716, "getSbExpr() expected type() to be kSbExpr", isSbExpr());
        return std::move(get<SbExpr>(_data));
    }
    Slice getSlice() const {
        tassert(7580704, "getSlice() expected type() to be kSlice", isSlice());
        return get<Slice>(_data);
    }

    bool isKeep() const {
        return type() == Type::kBool && get<Bool>(_data).value == true;
    }
    bool isDrop() const {
        return type() == Type::kBool && get<Bool>(_data).value == false;
    }

private:
    VariantType _data{};
};

/**
 * This function converts from projection AST to a pair of vectors: a vector of field paths and a
 * vector of ProjectNodes.
 */
std::pair<std::vector<std::string>, std::vector<ProjectNode>> getProjectNodes(
    const projection_ast::Projection& projection);

std::vector<ProjectNode> cloneProjectNodes(const std::vector<ProjectNode>& nodes);

/**
 * This method retrieves the values of the specified field paths ('fields') from 'resultSlot'
 * and stores the values into slots.
 *
 * This method returns a pair containing: (1) the updated SBE plan stage tree and; (2) a vector of
 * the slots ('outSlots') containing the field path values.
 *
 * The order of slots in 'outSlots' will match the order of field paths in 'fields'.
 */
std::pair<SbStage, SbSlotVector> projectFieldsToSlots(SbStage stage,
                                                      const std::vector<std::string>& fields,
                                                      boost::optional<SbSlot> resultSlot,
                                                      PlanNodeId nodeId,
                                                      sbe::value::SlotIdGenerator* slotIdGenerator,
                                                      StageBuilderState& state,
                                                      const PlanStageSlots* slots = nullptr);

template <typename T>
inline StringData getTopLevelField(const T& path) {
    auto idx = path.find('.');
    return StringData(getRawStringData(path), idx != std::string::npos ? idx : path.size());
}

inline std::vector<std::string> getTopLevelFields(const std::vector<std::string>& setOfPaths) {
    StringDataSet topLevelFieldsSet;
    std::vector<std::string> topLevelFields;

    for (size_t i = 0; i < setOfPaths.size(); ++i) {
        auto& path = setOfPaths[i];
        auto field = getTopLevelField(path);

        auto [_, inserted] = topLevelFieldsSet.insert(field);
        if (inserted) {
            topLevelFields.emplace_back(std::string{field});
        }
    }

    return topLevelFields;
}

template <typename T>
inline std::vector<std::string> getTopLevelFields(const T& setOfPaths) {
    std::vector<std::string> topLevelFields;
    StringSet topLevelFieldsSet;

    for (const auto& path : setOfPaths) {
        auto field = getTopLevelField(path);
        if (!topLevelFieldsSet.count(field)) {
            topLevelFields.emplace_back(std::string(field));
            topLevelFieldsSet.emplace(std::string(field));
        }
    }

    return topLevelFields;
}

template <typename T, typename FuncT>
inline std::vector<T> filterVector(std::vector<T> vec, FuncT fn) {
    std::vector<T> result;
    std::copy_if(std::make_move_iterator(vec.begin()),
                 std::make_move_iterator(vec.end()),
                 std::back_inserter(result),
                 fn);
    return result;
}

template <typename T, typename FuncT>
inline std::pair<std::vector<T>, std::vector<T>> splitVector(std::vector<T> vec, FuncT fn) {
    std::pair<std::vector<T>, std::vector<T>> result;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (fn(vec[i])) {
            result.first.emplace_back(std::move(vec[i]));
        } else {
            result.second.emplace_back(std::move(vec[i]));
        }
    }
    return result;
}

template <typename T>
inline std::vector<T> appendVectorUnique(std::vector<T> lhs, std::vector<T> rhs) {
    if (!rhs.empty()) {
        auto valueSet = std::set<T>{lhs.begin(), lhs.end()};
        for (size_t i = 0; i < rhs.size(); ++i) {
            if (valueSet.emplace(rhs[i]).second) {
                lhs.emplace_back(std::move(rhs[i]));
            }
        }
    }
    return lhs;
}

inline std::pair<std::unique_ptr<key_string::Value>, std::unique_ptr<key_string::Value>>
makeKeyStringPair(const BSONObj& lowKey,
                  bool lowKeyInclusive,
                  const BSONObj& highKey,
                  bool highKeyInclusive,
                  key_string::Version version,
                  Ordering ordering,
                  bool forward) {
    // Note that 'makeKeyFromBSONKeyForSeek()' is intended to compute the "start" key for an
    // index scan. The logic for computing a "discriminator" for an "end" key is reversed, which
    // is why we use 'makeKeyStringFromBSONKey()' to manually specify the discriminator for the
    // end key.
    key_string::Builder lowBuilder(version);
    IndexEntryComparison::makeKeyStringFromBSONKeyForSeek(
        lowKey, ordering, forward, lowKeyInclusive, lowBuilder);
    key_string::Builder highBuilder(version);
    IndexEntryComparison::makeKeyStringFromBSONKey(highKey,
                                                   ordering,
                                                   forward != highKeyInclusive
                                                       ? key_string::Discriminator::kExclusiveBefore
                                                       : key_string::Discriminator::kExclusiveAfter,
                                                   highBuilder);
    return {std::make_unique<key_string::Value>(lowBuilder.getValueCopy()),
            std::make_unique<key_string::Value>(highBuilder.getValueCopy())};
}

}  // namespace mongo::stage_builder
