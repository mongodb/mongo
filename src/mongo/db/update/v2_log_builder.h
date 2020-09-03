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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/log_builder_interface.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace v2_log_builder {
/**
 * These are structs for a "diff tree" that is constructed while the update is applied. There are
 * two types of internal nodes: Document nodes and Array nodes. All other node types are always
 * leaves.
 *
 * When the update is complete, the diff tree is converted into a $v: 2 oplog entry.
 */
enum class NodeType { kDocumentSubDiff, kDocumentInsert, kArray, kDelete, kUpdate, kInsert };

struct Node {
    virtual ~Node(){};
    virtual NodeType type() const = 0;
};


/**
 * This class represents insertion of a BSONElement or mutablebson Element. Note that
 * 'DocumentInsertionNode' also repesent an insert for the cases where an object is created
 * implicity.
 */
struct InsertNode : public Node {
    InsertNode(mutablebson::Element el) : elt(el) {}
    InsertNode(BSONElement el) : elt(el) {}

    NodeType type() const override {
        return NodeType::kInsert;
    }
    stdx::variant<mutablebson::Element, BSONElement> elt;
};

struct UpdateNode : public Node {
    UpdateNode(mutablebson::Element el) : elt(el) {}

    NodeType type() const override {
        return NodeType::kUpdate;
    }
    mutablebson::Element elt;
};

struct DeleteNode : public Node {
    NodeType type() const override {
        return NodeType::kDelete;
    }
};

// Struct representing non-leaf node.
struct InternalNode : public Node {
    virtual Node* addChild(StringData fieldName, std::unique_ptr<Node> node) = 0;
    virtual Node* getChild(StringData fieldName) const = 0;
};

struct DocumentNode : public InternalNode {
    Node* addChild(StringData fieldName, std::unique_ptr<Node> node) override;

    Node* getChild(StringData fieldName) const override {
        auto it = children.find(fieldName.toString());
        return (it != children.end()) ? it->second.get() : nullptr;
    }

    // We store the raw pointer to each of the child node so that we don't have to look up in
    // 'children' map every time. Note that the field names of these modifications will reference
    // the field name stored in 'children'.
    std::vector<std::pair<StringData, UpdateNode*>> updates;
    std::vector<std::pair<StringData, DeleteNode*>> deletes;
    std::vector<std::pair<StringData, Node*>> inserts;
    std::vector<std::pair<StringData, InternalNode*>> subDiffs;

    // We use std::unordered_map here for pointer stability on keys (field names) when a rehash
    // happens.
    stdx::unordered_map<std::string, std::unique_ptr<Node>, StringMapHasher, StringMapEq> children;
};

// Indicates that the document this node represents was created as part of the update.
//
// E.g. applying the update {$set: {"a.b.c": "foo"}} on document {} will create sub-documents
// at paths "a" and "a.b".
struct DocumentInsertionNode : public DocumentNode {
    NodeType type() const override {
        return NodeType::kDocumentInsert;
    }
};

// Indicates a Document internal node which is already in the pre-image document.
struct DocumentSubDiffNode : public DocumentNode {
    NodeType type() const override {
        return NodeType::kDocumentSubDiff;
    }
};

struct ArrayNode : public InternalNode {
    Node* addChild(StringData fieldName, std::unique_ptr<Node> node) override;

    virtual Node* getChild(StringData fieldName) const override {
        auto idx = str::parseUnsignedBase10Integer(fieldName);
        invariant(idx);
        auto it = children.find(*idx);
        return (it != children.end()) ? it->second.get() : nullptr;
    }

    NodeType type() const override {
        return NodeType::kArray;
    }

    // The ordering of this map is significant. We are expected to serialize array indexes in
    // numeric ascending order (as opposed to "stringified" order where "11" < "8").
    std::map<size_t, std::unique_ptr<Node>> children;
};

/**
 * A log builder which can produce $v:2 oplog entries.
 *
 * This log builder accumulates updates, creates and deletes, and stores them in a tree. When the
 * update is done and serialize() is called, the tree is converted into a $v:2 oplog entry. Note
 * that we don't need a pre-image for building the oplog.
 */
class V2LogBuilder : public LogBuilderInterface {
public:
    /**
     * Overload methods from the LogBuilder interface.
     */
    Status logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) override;
    Status logCreatedField(const RuntimeUpdatePath& path,
                           int idxOfFirstNewComponent,
                           mutablebson::Element elt) override;
    Status logCreatedField(const RuntimeUpdatePath& path,
                           int idxOfFirstNewComponent,
                           BSONElement elt) override;
    Status logDeletedField(const RuntimeUpdatePath& path) override;

    /**
     * Converts the in-memory tree to a $v:2 delta oplog entry.
     */
    BSONObj serialize() const override;

private:
    // Helpers for maintaining/updating the tree.
    Node* createInternalNode(InternalNode* parent,
                             const RuntimeUpdatePath& fullPath,
                             size_t pathIdx,
                             bool newPath);

    // Helpers for adding nodes at a certain path. Returns false if the path was invalid/did
    // not exist.
    void addNodeAtPathHelper(const RuntimeUpdatePath& path,
                             size_t pathIdx,
                             Node* root,
                             std::unique_ptr<Node> nodeToAdd,
                             boost::optional<size_t> idxOfFirstNewComponent);

    void addNodeAtPath(const RuntimeUpdatePath& path,
                       Node* root,
                       std::unique_ptr<Node> nodeToAdd,
                       boost::optional<size_t> idxOfFirstNewComponent);

    // Root of the tree.
    DocumentSubDiffNode _root;
};
}  // namespace v2_log_builder
}  // namespace mongo
