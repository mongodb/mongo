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

#include "mongo/platform/basic.h"

#include "mongo/db/update/v2_log_builder.h"

#include <stack>

#include "mongo/base/checked_cast.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"

namespace mongo::v2_log_builder {
Status V2LogBuilder::logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) {
    auto newNode = std::make_unique<diff_tree::UpdateNode>(elt);
    addNodeAtPath(path,
                  &_root,
                  std::move(newNode),
                  boost::none  // Index of first created component is none since this was an
                               // update, not a create.
    );
    return Status::OK();
}

Status V2LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     mutablebson::Element elt) {
    auto newNode = std::make_unique<diff_tree::InsertNode>(elt);
    addNodeAtPath(path, &_root, std::move(newNode), idxOfFirstNewComponent);
    return Status::OK();
}

Status V2LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     BSONElement elt) {
    auto newNode = std::make_unique<diff_tree::InsertNode>(elt);
    addNodeAtPath(path, &_root, std::move(newNode), idxOfFirstNewComponent);
    return Status::OK();
}

Status V2LogBuilder::logDeletedField(const RuntimeUpdatePath& path) {
    addNodeAtPath(path, &_root, std::make_unique<diff_tree::DeleteNode>(), boost::none);
    return Status::OK();
}

diff_tree::Node* V2LogBuilder::createInternalNode(diff_tree::InternalNode* parent,
                                                  const RuntimeUpdatePath& fullPath,
                                                  size_t pathIdx,
                                                  bool newPath) {
    auto fieldName = fullPath.fieldRef().getPart(pathIdx);

    // If the child is an array index, then this node is an ArrayNode.
    if (fullPath.size() > pathIdx + 1 &&
        fullPath.types()[pathIdx + 1] == RuntimeUpdatePath::ComponentType::kArrayIndex) {
        invariant(!newPath);
        return parent->addChild(fieldName, std::make_unique<diff_tree::ArrayNode>());
    } else if (newPath) {
        return parent->addChild(fieldName, std::make_unique<diff_tree::DocumentInsertionNode>());
    } else {
        return parent->addChild(fieldName, std::make_unique<diff_tree::DocumentSubDiffNode>());
    }
    MONGO_UNREACHABLE;
}

void V2LogBuilder::addNodeAtPath(const RuntimeUpdatePath& path,
                                 diff_tree::Node* root,
                                 std::unique_ptr<diff_tree::Node> nodeToAdd,
                                 boost::optional<size_t> idxOfFirstNewComponent) {
    addNodeAtPathHelper(path, 0, root, std::move(nodeToAdd), idxOfFirstNewComponent);
}

void V2LogBuilder::addNodeAtPathHelper(const RuntimeUpdatePath& path,
                                       size_t pathIdx,
                                       diff_tree::Node* root,
                                       std::unique_ptr<diff_tree::Node> nodeToAdd,
                                       boost::optional<size_t> idxOfFirstNewComponent) {
    invariant(root->type() == diff_tree::NodeType::kArray ||
              root->type() == diff_tree::NodeType::kDocumentSubDiff ||
              root->type() == diff_tree::NodeType::kDocumentInsert);

    // If our path is a.b.c.d and the first new component is "b" then we are dealing with a
    // newly created path for components b, c and d.
    const bool isNewPath = idxOfFirstNewComponent && (pathIdx >= *idxOfFirstNewComponent);

    auto* node = checked_cast<diff_tree::InternalNode*>(root);
    const auto part = path.fieldRef().getPart(pathIdx);
    if (pathIdx == static_cast<size_t>(path.fieldRef().numParts() - 1)) {
        node->addChild(part, std::move(nodeToAdd));
        return;
    }

    if (auto* child = node->getChild(part)) {
        addNodeAtPathHelper(path, pathIdx + 1, child, std::move(nodeToAdd), idxOfFirstNewComponent);
    } else {
        auto newNode = createInternalNode(node, path, pathIdx, isNewPath);
        addNodeAtPathHelper(
            path, pathIdx + 1, newNode, std::move(nodeToAdd), idxOfFirstNewComponent);
    }
}

BSONObj V2LogBuilder::serialize() const {
    auto diff = _root.serialize();
    return update_oplog_entry::makeDeltaOplogEntry(diff);
}
}  // namespace mongo::v2_log_builder
