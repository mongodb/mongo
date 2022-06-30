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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/stdx/variant.h"
#include "mongo/util/itoa.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/string_map.h"

// This file contains classes for serializing document diffs to a format that can be stored in the
// oplog. Any code/machinery which manipulates document diffs should do so through these classes.

namespace mongo {
namespace doc_diff {
using Diff = BSONObj;

/**
 * Enum representing the type of a diff.
 */
enum DiffType : uint8_t { kDocument, kArray };

// Below are string constants used in the diff format.
constexpr StringData kArrayHeader = "a"_sd;
constexpr StringData kDeleteSectionFieldName = "d"_sd;
constexpr StringData kInsertSectionFieldName = "i"_sd;
constexpr StringData kUpdateSectionFieldName = "u"_sd;
constexpr char kSubDiffSectionFieldPrefix = 's';
// 'l' for length.
constexpr StringData kResizeSectionFieldName = "l"_sd;

// Below are constants used for computation of Diff size. Note that the computed size is supposed to
// be an approximate value.
constexpr size_t kSizeOfEmptyDocumentDiffBuilder = 5;
// Size of empty object(5) + kArrayHeader(1) + null terminator + type byte + bool size.
constexpr size_t kSizeOfEmptyArrayDiffBuilder = 9;

/*
 * The below classes are for reading diffs. When given an invalid/malformed diff they will uassert.
 */
class DocumentDiffReader;

class ArrayDiffReader {
public:
    using ArrayModification = stdx::variant<BSONElement, DocumentDiffReader, ArrayDiffReader>;

    explicit ArrayDiffReader(const Diff& diff);

    /**
     * Methods which return the next modification (if any) and advance the iterator.  Returns
     * boost::none if there are no remaining modifications.  Otherwise, returns a pair. The first
     * member of the pair is the index of the modification. The second part is the modification
     * itself.
     *
     * If the second part of the pair contains a BSONElement, it means there is an update at that
     * index.
     *
     * If the second part contains a DocumentDiffReader or ArrayDiffReader, it means there is a
     * subdiff at that index.
     */
    boost::optional<std::pair<size_t, ArrayModification>> next();

    /**
     * Returns the size of the post image array.
     */
    boost::optional<size_t> newSize() {
        return _newSize;
    }

private:
    Diff _diff;
    BSONObjIterator _it;

    boost::optional<size_t> _newSize;
};

class DocumentDiffReader {
public:
    DocumentDiffReader(const Diff& diff);

    /**
     * The below methods get the next type of modification (if any) and advance the iterator.
     */
    boost::optional<StringData> nextDelete();
    boost::optional<BSONElement> nextUpdate();
    boost::optional<BSONElement> nextInsert();
    boost::optional<std::pair<StringData, stdx::variant<DocumentDiffReader, ArrayDiffReader>>>
    nextSubDiff();

private:
    Diff _diff;

    boost::optional<BSONObjIterator> _deletes;
    boost::optional<BSONObjIterator> _inserts;
    boost::optional<BSONObjIterator> _updates;
    boost::optional<BSONObjIterator> _subDiffs;
};
}  // namespace doc_diff

namespace diff_tree {
/**
 * These are structs for a "diff tree" that is constructed while the update is applied. There are
 * two types of internal nodes: Document nodes and Array nodes. All other node types are always
 * leaves.
 *
 * When the update is complete, the diff tree is converted into a $v: 2 oplog entry.
 */
enum class NodeType { kDocumentSubDiff, kDocumentInsert, kArray, kDelete, kUpdate, kInsert };

/**
 * Base class to represents a node in the diff tree.
 */
struct Node {
    virtual ~Node(){};
    virtual NodeType type() const = 0;
};

/**
 * This class represents insertion of a BSONElement or mutablebson Element. Note that
 * 'DocumentInsertionNode' also represent an insert for the cases where an object is created
 * implicitly.
 */
struct InsertNode : public Node {
    InsertNode(mutablebson::Element el) : elt(el) {}
    InsertNode(BSONElement el) : elt(el) {}

    NodeType type() const override {
        return NodeType::kInsert;
    }
    stdx::variant<mutablebson::Element, BSONElement> elt;
};

/**
 * Structure to represent a field update node.
 */
struct UpdateNode : public Node {
    UpdateNode(mutablebson::Element el) : elt(el) {}
    UpdateNode(BSONElement el) : elt(el) {}

    NodeType type() const override {
        return NodeType::kUpdate;
    }
    stdx::variant<mutablebson::Element, BSONElement> elt;
};

/**
 * Structure to represent a field delete node.
 */
struct DeleteNode : public Node {
    NodeType type() const override {
        return NodeType::kDelete;
    }
};

/**
 * Struct representing non-leaf node.
 */
struct InternalNode : public Node {
public:
    /**
     * Internal helper class for BSON size tracking of the diff to be generated.
     */
    struct ApproxBSONSizeTracker {
        ApproxBSONSizeTracker(size_t initialSize) : _size(initialSize) {}

        void addEntry(size_t fieldSize, const Node* node);

        void addSizeForWrapping() {
            // Type byte(1) + FieldName(1) + Null terminator(1) + empty BSON object size (5)
            _size += 8;
        }

        void increment(size_t size) {
            _size += size;
        }

        size_t getSize() const {
            return _size;
        }

    private:
        size_t _size;
    };
    InternalNode(size_t size = 0) : sizeTracker(size){};

    // Returns an unowned pointer to the newly added child.
    virtual Node* addChild(StringData fieldName, std::unique_ptr<Node> node) = 0;
    virtual Node* getChild(StringData fieldName) const = 0;
    size_t getObjSize() const {
        return sizeTracker.getSize();
    }

protected:
    ApproxBSONSizeTracker sizeTracker;
};

/**
 *  Indicates a Document internal node which is already in the pre-image document.
 */
class DocumentSubDiffNode : public InternalNode {
public:
    template <typename E>
    using ModificationEntries = std::vector<std::pair<StringData, E>>;

    DocumentSubDiffNode(size_t size = 0)
        : InternalNode(size + doc_diff::kSizeOfEmptyDocumentDiffBuilder){};

    Node* addChild(StringData fieldName, std::unique_ptr<Node> node) override;

    Node* getChild(StringData fieldName) const override {
        auto it = children.find(fieldName);
        return (it != children.end()) ? it->second.get() : nullptr;
    }

    void addUpdate(StringData fieldName, BSONElement value) {
        addChild(fieldName, std::make_unique<UpdateNode>(value));
    }
    void addInsert(StringData fieldName, BSONElement value) {
        addChild(fieldName, std::make_unique<InsertNode>(value));
    }
    void addDelete(StringData fieldName) {
        addChild(fieldName, std::make_unique<DeleteNode>());
    }
    NodeType type() const override {
        return NodeType::kDocumentSubDiff;
    }

    BSONObj serialize() const;

    const ModificationEntries<UpdateNode*>& getUpdates() const {
        return updates;
    }
    const ModificationEntries<DeleteNode*>& getDeletes() const {
        return deletes;
    }
    const ModificationEntries<Node*>& getInserts() const {
        return inserts;
    }
    const ModificationEntries<InternalNode*>& getSubDiffs() const {
        return subDiffs;
    }

    const absl::node_hash_map<std::string, std::unique_ptr<Node>, StringMapHasher, StringMapEq>&
    getChildren() const {
        return children;
    }

private:
    // We store the raw pointer to each of the child node so that we don't have to look up in
    // 'children' map every time. Note that the field names of these modifications will reference
    // the field name stored in 'children'. The node objects also point to the value of 'children'
    // map, where they are owned.
    ModificationEntries<UpdateNode*> updates;
    ModificationEntries<DeleteNode*> deletes;
    ModificationEntries<Node*> inserts;
    ModificationEntries<InternalNode*> subDiffs;

    // We use absl::node_hash_map here for pointer stability on keys (field names) when a rehash
    // happens.
    absl::node_hash_map<std::string, std::unique_ptr<Node>, StringMapHasher, StringMapEq> children;
};

/**
 *  Indicates that the document this node represents was created as part of the update.
 *
 * E.g. applying the update {$set: {"a.b.c": "foo"}} on document {} will create sub-documents
 * at paths "a" and "a.b".
 */
class DocumentInsertionNode : public InternalNode {
public:
    DocumentInsertionNode() : InternalNode(0){};

    Node* addChild(StringData fieldName, std::unique_ptr<Node> node) override {
        invariant(node->type() == NodeType::kInsert || node->type() == NodeType::kDocumentInsert);

        auto* nodePtr = node.get();
        auto result = children.insert({fieldName.toString(), std::move(node)});
        invariant(result.second);
        inserts.push_back({result.first->first, nodePtr});
        return nodePtr;
    }

    Node* getChild(StringData fieldName) const override {
        auto it = children.find(fieldName);
        return (it != children.end()) ? it->second.get() : nullptr;
    }

    NodeType type() const override {
        return NodeType::kDocumentInsert;
    }

    const std::vector<std::pair<StringData, Node*>>& getInserts() const {
        return inserts;
    }

private:
    // We store the raw pointer to each of the child node so that we don't have to look up in
    // 'children' map every time. Note that the field names of these inserts will reference
    // the field name stored in 'children'. The node objects also point to the value of 'children'
    // map, where they are owned.
    std::vector<std::pair<StringData, Node*>> inserts;

    // We use absl::node_hash_map here for pointer stability on keys (field names) when a rehash
    // happens.
    absl::node_hash_map<std::string, std::unique_ptr<Node>, StringMapHasher, StringMapEq> children;
};

/**
 * Class representing an array node.
 */
class ArrayNode : public InternalNode {
public:
    ArrayNode() : InternalNode(doc_diff::kSizeOfEmptyArrayDiffBuilder){};

    Node* addChild(size_t idx, std::unique_ptr<Node> node) {
        sizeTracker.addEntry(1 /* modification type */ +
                                 StringData(ItoA(idx)).size() /* Count the number of digits */,
                             node.get());
        auto itr = children.insert({idx, std::move(node)});
        invariant(itr.second);
        return itr.first->second.get();
    }

    Node* addChild(StringData fieldName, std::unique_ptr<Node> node) override {
        auto idx = str::parseUnsignedBase10Integer(fieldName);
        invariant(idx);
        return addChild(*idx, std::move(node));
    }

    void addUpdate(size_t idx, BSONElement value) {
        addChild(idx, std::make_unique<UpdateNode>(value));
    }

    virtual Node* getChild(StringData fieldName) const override {
        auto idx = str::parseUnsignedBase10Integer(fieldName);
        invariant(idx);
        auto it = children.find(*idx);
        return (it != children.end()) ? it->second.get() : nullptr;
    }

    void setResize(size_t size) {
        resize = size;
        sizeTracker.increment(1 /* kResizeSectionFieldName */ +
                              sizeof(uint32_t) /* size of value */ + 2);
    }

    NodeType type() const override {
        return NodeType::kArray;
    }

    const std::map<size_t, std::unique_ptr<Node>>& getChildren() const {
        return children;
    }
    const boost::optional<size_t>& getResize() const {
        return resize;
    }

private:
    // The ordering of this map is significant. We are expected to serialize array indexes in
    // numeric ascending order (as opposed to "stringified" order where "11" < "8").
    std::map<size_t, std::unique_ptr<Node>> children;
    boost::optional<size_t> resize;
};

}  // namespace diff_tree
}  // namespace mongo
