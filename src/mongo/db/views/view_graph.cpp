/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/views/view_graph.h"

#include "mongo/db/views/view.h"

namespace mongo {

// Leave room for view name and type in documents returned from listCollections, or an actual query
// on a sharded system.
const int ViewGraph::kMaxViewPipelineSizeBytes = 16 * 1000 * 1000;

const int ViewGraph::kMaxViewDepth = 20;
uint64_t ViewGraph::_idCounter = 0;

void ViewGraph::clear() {
    _graph.clear();
    _namespaceIds.clear();
}

Status ViewGraph::insertAndValidate(const NamespaceString& viewNss,
                                    const std::vector<NamespaceString>& refs,
                                    const int pipelineSize) {
    insertWithoutValidating(viewNss, refs, pipelineSize);

    // Perform validation on this newly inserted view. Note, if the graph was put in an invalid
    // state through unvalidated inserts (e.g. if the user manually edits system.views)
    // this may not necessarily be detected. We only check for errors introduced by this view.
    uint64_t nodeId = _getNodeId(viewNss);

    auto viewDepthLimitExceeded = [this, &viewNss]() -> Status {
        this->remove(viewNss);
        return Status(ErrorCodes::ViewDepthLimitExceeded,
                      str::stream() << "View depth limit exceeded; maximum depth is "
                                    << kMaxViewDepth);
    };

    auto viewPipelineMaxSizeExceeded = [this, &viewNss]() -> Status {
        this->remove(viewNss);
        return Status(ErrorCodes::ViewPipelineMaxSizeExceeded,
                      str::stream() << "View pipeline exceeds maximum size; maximum size is "
                                    << ViewGraph::kMaxViewPipelineSizeBytes);
    };

    // Check for cycles and get the height of the children.
    StatsMap statsMap;
    std::vector<uint64_t> cycleVertices;
    ErrorCodes::Error childRes =
        _getChildrenStatsAndCheckCycle(nodeId, nodeId, 0, &statsMap, &cycleVertices);
    if (childRes == ErrorCodes::ViewDepthLimitExceeded) {
        return viewDepthLimitExceeded();
    } else if (childRes == ErrorCodes::ViewPipelineMaxSizeExceeded) {
        return viewPipelineMaxSizeExceeded();
    } else if (childRes == ErrorCodes::GraphContainsCycle) {
        // Make the error message with the namespaces of the cycle and remove the node.
        str::stream ss;
        ss << "View cycle detected: ";
        for (auto cycleIter = cycleVertices.rbegin(); cycleIter != cycleVertices.rend();
             cycleIter++) {
            ss << _graph[*cycleIter].ns << " => ";
        }
        ss << viewNss.ns();
        remove(viewNss);
        return Status(ErrorCodes::GraphContainsCycle, ss);
    }

    // Subtract one since the child height includes the non-view leaf node(s).
    int childrenHeight = statsMap[nodeId].height - 1;

    int childrenSize = statsMap[nodeId].cumulativeSize;

    // Get the height of the parents to obtain the diameter through this node, as well as the size
    // of the pipeline to check if if the combined pipeline exceeds the max size.
    statsMap.clear();
    ErrorCodes::Error parentRes = _getParentsStats(nodeId, 0, &statsMap);
    if (parentRes == ErrorCodes::ViewDepthLimitExceeded) {
        return viewDepthLimitExceeded();
    } else if (parentRes == ErrorCodes::ViewPipelineMaxSizeExceeded) {
        return viewPipelineMaxSizeExceeded();
    }

    // Check the combined heights of the children and parents.
    int parentsHeight = statsMap[nodeId].height;
    // Subtract one since the parent and children heights include the current node.
    int diameter = parentsHeight + childrenHeight - 1;

    if (diameter > kMaxViewDepth) {
        return viewDepthLimitExceeded();
    }

    // Check the combined sizes of the children and parents.
    int parentsSize = statsMap[nodeId].cumulativeSize;
    // Subtract the current node's size since the parent and children sizes include the current
    // node.
    const Node& currentNode = _graph[nodeId];
    int pipelineTotalSize = parentsSize + childrenSize - currentNode.size;

    if (pipelineTotalSize > kMaxViewPipelineSizeBytes) {
        return viewPipelineMaxSizeExceeded();
    }

    return Status::OK();
}

void ViewGraph::insertWithoutValidating(const NamespaceString& viewNss,
                                        const std::vector<NamespaceString>& refs,
                                        const int pipelineSize) {
    uint64_t nodeId = _getNodeId(viewNss);
    // Note, the parent pointers of this node are set when the parents are inserted.
    // This sets the children pointers of the node for this view, as well as the parent
    // pointers for its children.
    Node* node = &(_graph[nodeId]);
    node->size = pipelineSize;
    invariant(node->children.empty());

    for (const NamespaceString& childNss : refs) {
        uint64_t childId = _getNodeId(childNss);
        node->children.insert(childId);
        _graph[childId].parents.insert(nodeId);
    }
}

void ViewGraph::remove(const NamespaceString& viewNss) {
    // If this node hasn't been referenced, return early.
    if (_namespaceIds.find(viewNss.ns()) == _namespaceIds.end()) {
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
            _namespaceIds.erase(childNode->ns);
            _graph.erase(childId);
        }
    }

    // Remove all child pointers since this view no longer references anything.
    node->children.clear();

    // Only remove node if there are no remaining references to this node.
    if (node->parents.size() == 0) {
        _namespaceIds.erase(node->ns);
        _graph.erase(nodeId);
    }
}

ErrorCodes::Error ViewGraph::_getParentsStats(uint64_t currentId,
                                              int currentDepth,
                                              StatsMap* statsMap) {
    const Node& currentNode = _graph[currentId];
    int maxHeightOfParents = 0;
    int maxSizeOfParents = 0;

    // Return early if we've already exceeded the maximum depth. This will also be triggered if
    // we're traversing a cycle introduced through unvalidated inserts.
    if (currentDepth > kMaxViewDepth) {
        return ErrorCodes::ViewDepthLimitExceeded;
    }

    for (uint64_t parentId : currentNode.parents) {
        if (!(*statsMap)[parentId].checked) {
            auto res = _getParentsStats(parentId, currentDepth + 1, statsMap);
            if (res != ErrorCodes::OK) {
                return res;
            }
        }
        maxHeightOfParents = std::max(maxHeightOfParents, (*statsMap)[parentId].height);
        maxSizeOfParents = std::max(maxSizeOfParents, (*statsMap)[parentId].cumulativeSize);
    }

    (*statsMap)[currentId].checked = true;
    (*statsMap)[currentId].height = maxHeightOfParents + 1;
    (*statsMap)[currentId].cumulativeSize += maxSizeOfParents + currentNode.size;

    if ((*statsMap)[currentId].cumulativeSize > kMaxViewPipelineSizeBytes) {
        return ErrorCodes::ViewPipelineMaxSizeExceeded;
    }

    return ErrorCodes::OK;
}

ErrorCodes::Error ViewGraph::_getChildrenStatsAndCheckCycle(uint64_t startingId,
                                                            uint64_t currentId,
                                                            int currentDepth,
                                                            StatsMap* statsMap,
                                                            std::vector<uint64_t>* cycleIds) {
    // Check children of current node.
    const Node& currentNode = _graph[currentId];
    if (currentDepth > 0 && currentId == startingId) {
        return ErrorCodes::GraphContainsCycle;
    }

    // Return early if we've already exceeded the maximum depth. This will also be triggered if
    // we're traversing a cycle introduced through unvalidated inserts.
    if (currentDepth > kMaxViewDepth) {
        return ErrorCodes::ViewDepthLimitExceeded;
    }

    int maxHeightOfChildren = 0;
    int maxSizeOfChildren = 0;
    for (uint64_t childId : currentNode.children) {
        if (!(*statsMap)[childId].checked) {
            auto res = _getChildrenStatsAndCheckCycle(
                startingId, childId, currentDepth + 1, statsMap, cycleIds);
            if (res == ErrorCodes::GraphContainsCycle) {
                cycleIds->push_back(currentId);
                return res;
            } else if (res != ErrorCodes::OK) {
                return res;
            }
        }
        maxHeightOfChildren = std::max(maxHeightOfChildren, (*statsMap)[childId].height);
        maxSizeOfChildren = std::max(maxSizeOfChildren, (*statsMap)[childId].cumulativeSize);
    }

    (*statsMap)[currentId].checked = true;
    (*statsMap)[currentId].height = maxHeightOfChildren + 1;
    (*statsMap)[currentId].cumulativeSize += maxSizeOfChildren + currentNode.size;
    return ErrorCodes::OK;
}

uint64_t ViewGraph::_getNodeId(const NamespaceString& nss) {
    if (_namespaceIds.find(nss.ns()) == _namespaceIds.end()) {
        uint64_t nodeId = _idCounter++;
        _namespaceIds[nss.ns()] = nodeId;
        // Initialize the corresponding graph node.
        _graph[nodeId].ns = nss.ns();
    }
    return _namespaceIds[nss.ns()];
}

}  // namespace mongo
