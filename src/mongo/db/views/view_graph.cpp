/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/views/view_graph.h"

#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/views/view.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

// Leave room for view name and type in documents returned from listCollections, or an actual query
// on a sharded system.
const int ViewGraph::kMaxViewPipelineSizeBytes = 16 * 1000 * 1000;

const int ViewGraph::kMaxViewDepth = 20;

void ViewGraph::clear() {
    _graph.clear();
    _namespaceIds.clear();
}

size_t ViewGraph::size() const {
    return _graph.size();
}

Status ViewGraph::insertAndValidate(const ViewDefinition& view,
                                    const std::vector<NamespaceString>& refs,
                                    int pipelineSize) {
    insertWithoutValidating(view, refs, pipelineSize);

    // Perform validation on this newly inserted view. Note, if the graph was put in an invalid
    // state through unvalidated inserts (e.g. if the user manually edits system.views)
    // this may not necessarily be detected. We only check for errors introduced by this view.
    const auto& viewNss = view.name();
    uint64_t nodeId = _getNodeId(viewNss);

    // If the graph fails validation for any reason, the insert is automatically rolled back on
    // exiting this method.
    ScopeGuard guard([&] { remove(viewNss); });

    // Check for cycles and get the height of the children.
    StatsMap statsMap;
    std::vector<uint64_t> cycleVertices;
    cycleVertices.reserve(kMaxViewDepth);
    auto childRes = _validateChildren(nodeId, nodeId, 0, &statsMap, &cycleVertices);
    if (!childRes.isOK()) {
        return childRes;
    }

    // Subtract one since the child height includes the non-view leaf node(s).
    int childrenHeight = statsMap[nodeId].height - 1;

    int childrenSize = statsMap[nodeId].cumulativeSize;

    // Get the height of the parents to obtain the diameter through this node, as well as the size
    // of the pipeline to check if if the combined pipeline exceeds the max size.
    statsMap.clear();
    auto parentRes = _validateParents(nodeId, 0, &statsMap);
    if (!parentRes.isOK()) {
        return parentRes;
    }

    // Check the combined heights of the children and parents.
    int parentsHeight = statsMap[nodeId].height;
    // Subtract one since the parent and children heights include the current node.
    int diameter = parentsHeight + childrenHeight - 1;

    if (diameter > kMaxViewDepth) {
        return {ErrorCodes::ViewDepthLimitExceeded,
                str::stream() << "View depth limit exceeded; maximum depth is " << kMaxViewDepth};
    }

    // Check the combined sizes of the children and parents.
    int parentsSize = statsMap[nodeId].cumulativeSize;
    // Subtract the current node's size since the parent and children sizes include the current
    // node.
    const Node& currentNode = _graph[nodeId];
    int pipelineTotalSize = parentsSize + childrenSize - currentNode.size;

    if (pipelineTotalSize > kMaxViewPipelineSizeBytes) {
        return {ErrorCodes::ViewPipelineMaxSizeExceeded,
                str::stream() << "Operation would result in a resolved view pipeline that exceeds "
                                 "the maximum size of "
                              << kMaxViewPipelineSizeBytes << " bytes"};
    }

    guard.dismiss();
    return Status::OK();
}

void ViewGraph::insertWithoutValidating(const ViewDefinition& view,
                                        const std::vector<NamespaceString>& refs,
                                        int pipelineSize) {
    uint64_t nodeId = _getNodeId(view.name());
    // Note, the parent pointers of this node are set when the parents are inserted.
    // This sets the children pointers of the node for this view, as well as the parent
    // pointers for its children.
    Node* node = &(_graph[nodeId]);
    invariant(node->children.empty());

    node->size = pipelineSize;
    node->collator = CollatorInterface::cloneCollator(view.defaultCollator());

    for (const NamespaceString& childNss : refs) {
        uint64_t childId = _getNodeId(childNss);
        node->children.insert(childId);
        _graph[childId].parents.insert(nodeId);
    }
}

void ViewGraph::remove(const NamespaceString& viewNss) {
    // If this node hasn't been referenced, return early.
    if (_namespaceIds.find(viewNss) == _namespaceIds.end()) {
        return;
    }

    uint64_t nodeId = _getNodeId(viewNss);
    Node* node = &(_graph[nodeId]);

    // Remove self-reference pointers if they exist.
    node->children.erase(nodeId);
    node->parents.erase(nodeId);

    // Remove child->parent pointers.
    for (uint64_t childId : node->children) {
        Node* childNode = &(_graph[childId]);
        childNode->parents.erase(nodeId);
        // If the child has no remaining references or children, remove it.
        if (childNode->parents.size() == 0 && childNode->children.size() == 0) {
            _namespaceIds.erase(childNode->nss);
            _graph.erase(childId);
        }
    }

    // This node no longer represents a view, so its children must be cleared and its collator
    // unset.
    node->children.clear();
    node->collator = nullptr;

    // Only remove node if there are no remaining references to this node.
    if (node->parents.size() == 0) {
        _namespaceIds.erase(node->nss);
        _graph.erase(nodeId);
    }
}

Status ViewGraph::_validateParents(uint64_t currentId, int currentDepth, StatsMap* statsMap) {
    const Node& currentNode = _graph[currentId];
    int maxHeightOfParents = 0;
    int maxSizeOfParents = 0;

    // Return early if we've already exceeded the maximum depth. This will also be triggered if
    // we're traversing a cycle introduced through unvalidated inserts.
    if (currentDepth > kMaxViewDepth) {
        return {ErrorCodes::ViewDepthLimitExceeded,
                str::stream() << "View depth limit exceeded; maximum depth is "
                              << ViewGraph::kMaxViewDepth};
    }

    for (uint64_t parentId : currentNode.parents) {
        const auto& parentNode = _graph[parentId];
        if (parentNode.isView() &&
            !CollatorInterface::collatorsMatch(currentNode.collator.get(),
                                               parentNode.collator.get())) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "View " << currentNode.nss.ns()
                                  << " has a collation that does not match the collation of view "
                                  << parentNode.nss.ns()};
        }

        if (!(*statsMap)[parentId].checked) {
            auto res = _validateParents(parentId, currentDepth + 1, statsMap);
            if (!res.isOK()) {
                return res;
            }
        }
        maxHeightOfParents = std::max(maxHeightOfParents, (*statsMap)[parentId].height);
        maxSizeOfParents = std::max(maxSizeOfParents, (*statsMap)[parentId].cumulativeSize);
    }

    (*statsMap)[currentId].checked = true;
    (*statsMap)[currentId].height = maxHeightOfParents + 1;
    (*statsMap)[currentId].cumulativeSize += maxSizeOfParents + currentNode.size;

    const auto size = (*statsMap)[currentId].cumulativeSize;
    if (size > kMaxViewPipelineSizeBytes) {
        return {ErrorCodes::ViewPipelineMaxSizeExceeded,
                str::stream() << "View pipeline is too large and exceeds the maximum size of "
                              << ViewGraph::kMaxViewPipelineSizeBytes << " bytes"};
    }

    return Status::OK();
}

Status ViewGraph::_validateChildren(uint64_t startingId,
                                    uint64_t currentId,
                                    int currentDepth,
                                    StatsMap* statsMap,
                                    std::vector<uint64_t>* traversalIds) {
    const Node& currentNode = _graph[currentId];
    traversalIds->push_back(currentId);

    // If we've encountered the id of the starting node, we've found a cycle in the graph.
    if (currentDepth > 0 && currentId == startingId) {
        auto iterator = traversalIds->rbegin();
        auto errmsg = StringBuilder();

        errmsg << "View cycle detected: ";
        errmsg << _graph[*iterator].nss.ns();
        for (; iterator != traversalIds->rend(); ++iterator) {
            errmsg << " => " << _graph[*iterator].nss.ns();
        }
        return {ErrorCodes::GraphContainsCycle, errmsg.str()};
    }

    // Return early if we've already exceeded the maximum depth. This will also be triggered if
    // we're traversing a cycle introduced through unvalidated inserts.
    if (currentDepth > kMaxViewDepth) {
        return {ErrorCodes::ViewDepthLimitExceeded,
                str::stream() << "View depth limit exceeded; maximum depth is "
                              << ViewGraph::kMaxViewDepth};
    }

    int maxHeightOfChildren = 0;
    int maxSizeOfChildren = 0;
    for (uint64_t childId : currentNode.children) {
        if ((*statsMap)[childId].checked) {
            continue;
        }

        const auto& childNode = _graph[childId];
        if (childNode.isView() &&
            !CollatorInterface::collatorsMatch(currentNode.collator.get(),
                                               childNode.collator.get())) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "View " << currentNode.nss.ns()
                                  << " has a collation that does not match the collation of view "
                                  << childNode.nss.ns()};
        }

        auto res = _validateChildren(startingId, childId, currentDepth + 1, statsMap, traversalIds);
        if (!res.isOK()) {
            return res;
        }

        maxHeightOfChildren = std::max(maxHeightOfChildren, (*statsMap)[childId].height);
        maxSizeOfChildren = std::max(maxSizeOfChildren, (*statsMap)[childId].cumulativeSize);
    }

    traversalIds->pop_back();
    (*statsMap)[currentId].checked = true;
    (*statsMap)[currentId].height = maxHeightOfChildren + 1;
    (*statsMap)[currentId].cumulativeSize += maxSizeOfChildren + currentNode.size;
    return Status::OK();
}

uint64_t ViewGraph::_getNodeId(const NamespaceString& nss) {
    if (_namespaceIds.find(nss) == _namespaceIds.end()) {
        uint64_t nodeId = _idCounter++;
        _namespaceIds[nss] = nodeId;
        // Initialize the corresponding graph node.
        _graph[nodeId].nss = nss;
    }
    return _namespaceIds[nss];
}

}  // namespace mongo
