// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/metadata/path_arrayness.h"

#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#include <stack>
#include <string_view>

using namespace mongo::multikey_paths;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

MONGO_FAIL_POINT_DEFINE(pathArraynessYieldInvalidation);

namespace mongo {

auto& pathArraynessQueriesFailedDueToInvalidation =
    *MetricBuilder<Counter64>{"query.pathArrayness.queriesFailedDueToInvalidation"};

const PathArrayness& PathArrayness::emptyPathArrayness() {
    static const PathArrayness kEmptyPathArrayness;
    return kEmptyPathArrayness;
}

bool PathArrayness::isIndexEligibleToAddToPathArrayness(const IndexDescriptor& descriptor) {
    if (descriptor.isPartial() || descriptor.hidden()) {
        return false;
    }
    if (descriptor.getIndexType() != INDEX_BTREE) {
        return false;
    }
    // Indexes with numeric path components (e.g. {"a.0.x": 1}) cannot be used to reason about
    // arrayness. A numeric component may access an array element positionally (e.g. a[0]), in which
    // case the array at the parent component is not recorded as multikey even though it is an
    // array. Trusting the multikey metadata for such indexes would incorrectly classify the parent
    // path as non-array. Conservatively exclude these indexes.
    for (const auto& key : descriptor.keyPattern()) {
        if (FieldRef(key.fieldNameStringData()).hasNumericPathComponents()) {
            return false;
        }
    }
    return true;
}

void PathArrayness::addPath(const FieldPath& path,
                            const MultikeyComponents& multikeyPath,
                            bool isFullRebuild) {
    _root.insertPath(path, multikeyPath, 0, isFullRebuild);
}

void PathArrayness::addPathsFromIndexKeyPattern(const BSONObj& indexKeyPattern,
                                                const MultikeyPaths& multikeyPaths,
                                                bool isFullRebuild) {
    if (indexKeyPattern.isEmpty()) {
        // No paths to add if indexKeyPattern is empty.
        return;
    } else {
        tassert(11480900,
                "multikeyPaths must be non-empty when indexKeyPattern is non-empty",
                !multikeyPaths.empty());
    }

    size_t indexCounter = 0;
    for (const auto& key : indexKeyPattern) {
        FieldPath path(key.fieldNameStringData());
        addPath(path, multikeyPaths[indexCounter], isFullRebuild);
        ++indexCounter;
    }
}

bool PathArrayness::canPathBeArray(const FieldPath& path, const ExpressionContext* expCtx) const {
    // If the PathArrayness query knob is disabled, conservatively return true.
    if (!expCtx->getQueryKnobConfiguration().getEnablePathArrayness()) {
        return true;
    }

    bool arrayness = _root.canPathBeArray(path);
    LOGV2_DEBUG(11467800,
                5,
                "Checking path arrayness",
                "path"_attr = path,
                "canPathBeArray"_attr = arrayness);
    return arrayness;
}

bool PathArrayness::canPathBeArray(const FieldRef& path, const ExpressionContext* expCtx) const {
    // If the PathArrayness query knob is disabled, conservatively return true.
    if (!expCtx->getQueryKnobConfiguration().getEnablePathArrayness()) {
        return true;
    }

    std::string_view pathString = path.dottedField(0);
    StatusWith<FieldPath> maybeFieldPath = fieldPathWithValidationStatus(std::string(pathString));

    // If FieldPath validation fails, conservatively assume this path is an array.
    if (!maybeFieldPath.isOK()) {
        return true;
    }

    return canPathBeArray(maybeFieldPath.getValue(), expCtx);
}

bool PathArrayness::TrieNode::canPathBeArray(const FieldPath& path) const {
    const TrieNode* current = this;
    // Track the number of times we have seen an array prefix.
    for (size_t depth = 0; depth < path.getPathLength(); ++depth) {
        const auto pathSegment = std::string(path.getFieldName(depth));
        const auto& next = current->_children.find(pathSegment);
        if (next == current->_children.end()) {
            // Missing path, conservatively assume all components from this point on are arrays.
            return true;
        }
        current = &next->second;
        if (current->canBeArray()) {
            return true;
        }
    }
    return current->canBeArray();
}

stdx::unordered_map<std::string, bool> PathArrayness::exportToMap_forTest() {
    stdx::unordered_map<std::string, bool> result;

    std::stack<std::pair<PathArrayness::TrieNode, std::string>> myStack;

    myStack.push({this->_root, ""});

    while (!myStack.empty()) {
        const auto [currNode, currPathComponent] = myStack.top();
        myStack.pop();

        // Do not insert the root (which is the only record with an empty fieldname)
        if (!currPathComponent.empty()) {
            result[currPathComponent] = currNode.canBeArray();
        }

        for (auto&& [childNode, childPathComponent] : currNode.getChildren()) {
            std::string pathPrefix = (currPathComponent.empty() ? "" : currPathComponent + ".");
            pathPrefix += childNode;
            myStack.push({childPathComponent, pathPrefix});
        }
    }

    return result;
}

void PathArrayness::TrieNode::visualizeTrie_forTest(std::string fieldName, int depth) const {
    for (int i = 0; i < depth; ++i) {
        std::cout << "  ";
    }

    std::cout << fieldName << "(" << _canBeArray << ")" << std::endl;

    // Recursively print children
    for (auto it = _children.begin(); it != _children.end(); ++it) {
        it->second.visualizeTrie_forTest(it->first, depth + 1);
    }
}

void PathArrayness::TrieNode::insertPath(const FieldPath& path,
                                         const MultikeyComponents& multikeyPath,
                                         size_t depth,
                                         bool isFullRebuild) {
    if (depth >= path.getPathLength()) {
        return;
    }

    const auto& fieldNameToInsert = std::string(path.getFieldName(depth));
    const auto& maybeChild = _children.find(fieldNameToInsert);

    if (maybeChild == _children.end()) {
        // This path component does not already exist so create a new TrieNode and insert it.
        _children.insert({fieldNameToInsert, TrieNode(multikeyPath.count(depth))});
    } else {
        // This path component already exists in trie so resolve conflicts in arrayness information.
        if (isFullRebuild) {
            // If we're fully rebuilding the trie from the full set of indexes, we can assume that
            // if the same path shows up as multikey and non-multikey then the non-multikey index is
            // more up to date.
            maybeChild->second._canBeArray &= (multikeyPath.count(depth) > 0);
        } else {
            // Otherwise (if we're inserting a path due to an index catalog update from a document
            // write operation), we take the conservative approach, i.e. prefer multikey in case of
            // conflict.
            maybeChild->second._canBeArray |= (multikeyPath.count(depth) > 0);
        }
    }

    // Recursively invoke the remaining path.
    _children.at(fieldNameToInsert).insertPath(path, multikeyPath, ++depth, isFullRebuild);
}

boost::optional<FieldPath> PathArrayness::getFirstInvalidatedPath(
    const MonotonicallyIncreasingFieldPathSet& nonArrayPaths, const PathArrayness& current) {
    if (MONGO_unlikely(pathArraynessYieldInvalidation.shouldFail())) {
        return FieldPath("pathArraynessYieldInvalidationShouldFail");
    }
    for (const auto& path : nonArrayPaths) {
        if (current._root.canPathBeArray(path)) {
            return path;
        }
    }
    return boost::none;
}

void PathArraynessChecker::uassertIfInvalidatedAndSyncEpoch(const PathArrayness& current,
                                                            const NamespaceString& ns) {
    auto currentEpoch = current.epoch();
    if (prevEpoch.has_value() && *prevEpoch == currentEpoch) {
        return;
    }
    prevEpoch = currentEpoch;
    if (auto invalidated = PathArrayness::getFirstInvalidatedPath(nonArrayPaths, current)) {
        pathArraynessQueriesFailedDueToInvalidation.increment();
        uasserted(
            ErrorCodes::QueryPlanKilled,
            str::stream() << "query plan killed :: non-array path became multikey during yield: "
                             "namespace="
                          << ns.toStringForErrorMsg() << ", path=" << invalidated->fullPath());
    }
}

}  // namespace mongo
