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

#include "mongo/db/update/v2_log_builder.h"

#include <stack>

#include "mongo/base/checked_cast.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/util/str.h"

namespace mongo::v2_log_builder {
Node* ArrayNode::addChild(StringData fieldName, std::unique_ptr<Node> node) {
    auto idx = str::parseUnsignedBase10Integer(fieldName);
    invariant(idx);

    auto itr = children.insert({*idx, std::move(node)});
    invariant(itr.second);
    return itr.first->second.get();
}

Node* DocumentNode::addChild(StringData fieldName, std::unique_ptr<Node> node) {
    auto* nodePtr = node.get();
    auto result = children.insert({fieldName.toString(), std::move(node)});
    invariant(result.second);
    StringData storedFieldName = result.first->first;
    switch (nodePtr->type()) {
        case (NodeType::kArray):
        case (NodeType::kDocumentSubDiff): {
            subDiffs.push_back({storedFieldName, checked_cast<InternalNode*>(nodePtr)});
            return nodePtr;
        }
        case (NodeType::kDocumentInsert):
        case (NodeType::kInsert): {
            inserts.push_back({storedFieldName, nodePtr});
            return nodePtr;
        }
        case (NodeType::kDelete): {
            deletes.push_back({storedFieldName, checked_cast<DeleteNode*>(nodePtr)});
            return nodePtr;
        }
        case (NodeType::kUpdate): {
            updates.push_back({storedFieldName, checked_cast<UpdateNode*>(nodePtr)});
            return nodePtr;
        }
    }
    MONGO_UNREACHABLE;
}

Status V2LogBuilder::logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) {
    auto newNode = std::make_unique<UpdateNode>(elt);
    addNodeAtPath(path,
                  &_root,
                  std::move(newNode),
                  boost::none  // Index of first created component is none since this was an update,
                               // not a create.
    );
    return Status::OK();
}

Status V2LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     mutablebson::Element elt) {
    auto newNode = std::make_unique<InsertNode>(elt);
    addNodeAtPath(path, &_root, std::move(newNode), idxOfFirstNewComponent);
    return Status::OK();
}

Status V2LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     BSONElement elt) {
    auto newNode = std::make_unique<InsertNode>(elt);
    addNodeAtPath(path, &_root, std::move(newNode), idxOfFirstNewComponent);
    return Status::OK();
}

Status V2LogBuilder::logDeletedField(const RuntimeUpdatePath& path) {
    addNodeAtPath(path, &_root, std::make_unique<DeleteNode>(), boost::none);
    return Status::OK();
}

Node* V2LogBuilder::createInternalNode(InternalNode* parent,
                                       const RuntimeUpdatePath& fullPath,
                                       size_t pathIdx,
                                       bool newPath) {
    auto fieldName = fullPath.fieldRef().getPart(pathIdx);

    // If the child is an array index, then this node is an ArrayNode.
    if (fullPath.size() > pathIdx + 1 &&
        fullPath.types()[pathIdx + 1] == RuntimeUpdatePath::ComponentType::kArrayIndex) {
        invariant(!newPath);
        return parent->addChild(fieldName, std::make_unique<ArrayNode>());
    } else if (newPath) {
        return parent->addChild(fieldName, std::make_unique<DocumentInsertionNode>());
    } else {
        return parent->addChild(fieldName, std::make_unique<DocumentSubDiffNode>());
    }
    MONGO_UNREACHABLE;
}

void V2LogBuilder::addNodeAtPath(const RuntimeUpdatePath& path,
                                 Node* root,
                                 std::unique_ptr<Node> nodeToAdd,
                                 boost::optional<size_t> idxOfFirstNewComponent) {
    addNodeAtPathHelper(path, 0, root, std::move(nodeToAdd), idxOfFirstNewComponent);
}

void V2LogBuilder::addNodeAtPathHelper(const RuntimeUpdatePath& path,
                                       size_t pathIdx,
                                       Node* root,
                                       std::unique_ptr<Node> nodeToAdd,
                                       boost::optional<size_t> idxOfFirstNewComponent) {
    invariant(root->type() == NodeType::kArray || root->type() == NodeType::kDocumentSubDiff ||
              root->type() == NodeType::kDocumentInsert);

    // If our path is a.b.c.d and the first new component is "b" then we are dealing with a
    // newly created path for components b, c and d.
    const bool isNewPath = idxOfFirstNewComponent && (pathIdx >= *idxOfFirstNewComponent);

    auto* node = checked_cast<InternalNode*>(root);
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

namespace {
void appendElementToBuilder(stdx::variant<mutablebson::Element, BSONElement> elem,
                            StringData fieldName,
                            BSONObjBuilder* builder) {
    stdx::visit(
        visit_helper::Overloaded{
            [&](const mutablebson::Element& element) {
                if (element.hasValue()) {
                    builder->appendAs(element.getValue(), fieldName);
                } else if (element.getType() == BSONType::Object) {
                    BSONObjBuilder subBuilder(builder->subobjStart(fieldName));
                    element.writeChildrenTo(&subBuilder);
                } else {
                    invariant(element.getType() == BSONType::Array);
                    BSONArrayBuilder subBuilder(builder->subarrayStart(fieldName));
                    element.writeArrayTo(&subBuilder);
                }
            },
            [&](BSONElement element) { builder->appendAs(element, fieldName); }},
        elem);
}

// Construction of the $v:2 diff needs to handle the same number of levels of recursion as the
// maximum permitted BSON depth. In order to avoid the possibility of stack overflow, which has
// been observed in configurations that use small stack sizes(such as 'dbg=on'), we use an explicit
// stack data structure stored on the heap instead.
//
// The Frame class represents one "frame" of this explicit stack.
class Frame {
public:
    virtual ~Frame() {}

    // When execute() is called, the Frame may either return a new Frame to be placed on top of the
    // stack, or return nullptr, indicating that the frame has finished and can be destroyed.
    //
    // If a Frame returns a new stack frame, it must be able to pick up where it left off when
    // execute() is called on it again.
    virtual std::unique_ptr<Frame> execute() = 0;
};
using UniqueFrame = std::unique_ptr<Frame>;

// Helper used for creating a new frame from a sub-diff node. Definition depends on some of the
// *Frame constructors.
UniqueFrame makeSubNodeFrameHelper(InternalNode* node, BSONObjBuilder builder);
// Given a 'node' stored in the 'inserts' section of an InternalNode, will either append that
// node's value to the given builder, or return a new stack frame which will build the object to be
// inserted. 'node' must be an InsertionNode or a DocumentInsertNode.
UniqueFrame handleInsertHelper(StringData fieldName, Node* node, BSONObjBuilder* bob);

// Stack frame used to maintain state while serializing DocumentInsertionNodes.
class DocumentInsertFrame final : public Frame {
public:
    DocumentInsertFrame(const DocumentInsertionNode& node, BSONObjBuilder bob)
        : _node(node), _bob(std::move(bob)) {}

    UniqueFrame execute() override {
        for (; _insertIdx < _node.children.size(); ++_insertIdx) {
            auto&& [fieldName, child] = _node.inserts[_insertIdx];

            if (auto newFrame = handleInsertHelper(fieldName, child, &_bob)) {
                ++_insertIdx;
                return newFrame;
            }
        }

        return nullptr;
    }

private:
    size_t _insertIdx = 0;
    const DocumentInsertionNode& _node;
    BSONObjBuilder _bob;
};

// Stack frame used to maintain state while serializing DocumentSubDiffNodes.
class DocumentSubDiffFrame final : public Frame {
public:
    DocumentSubDiffFrame(const DocumentSubDiffNode& node, BSONObjBuilder bob)
        : _node(node), _bob(std::move(bob)) {}

    UniqueFrame execute() override {
        if (!_wroteDeletesAndUpdates) {
            if (!_node.deletes.empty()) {
                BSONObjBuilder subBob(_bob.subobjStart(doc_diff::kDeleteSectionFieldName));
                for (auto&& [fieldName, node] : _node.deletes) {
                    // The deletes are logged in the form {fieldName: false} in $v:2 format.
                    subBob.append(fieldName, false);
                }
            }
            if (!_node.updates.empty()) {
                BSONObjBuilder subBob(_bob.subobjStart(doc_diff::kUpdateSectionFieldName));
                for (auto&& [fieldName, node] : _node.updates) {
                    appendElementToBuilder(node->elt, fieldName, &subBob);
                }
            }
            _wroteDeletesAndUpdates = true;
        }

        for (; _insertIdx < _node.inserts.size(); ++_insertIdx) {
            if (!_insertBob) {
                _insertBob.emplace(_bob.subobjStart(doc_diff::kInsertSectionFieldName));
            }

            auto&& [fieldName, child] = _node.inserts[_insertIdx];

            if (auto newFrame = handleInsertHelper(fieldName, child, _insertBob.get_ptr())) {
                ++_insertIdx;
                return newFrame;
            }
        }

        if (_insertBob) {
            // All inserts have been written so we destroy the insert builder now.
            _insertBob = boost::none;
        }

        if (_subDiffIdx != _node.subDiffs.size()) {
            auto&& [fieldName, child] = _node.subDiffs[_subDiffIdx];

            BSONObjBuilder childBuilder =
                _bob.subobjStart(std::string(1, doc_diff::kSubDiffSectionFieldPrefix) + fieldName);
            ++_subDiffIdx;
            return makeSubNodeFrameHelper(child, std::move(childBuilder));
        }

        return nullptr;
    }

    BSONObjBuilder& bob() {
        return _bob;
    }

private:
    const DocumentSubDiffNode& _node;
    BSONObjBuilder _bob;

    // Indicates whether or not we've written deletes and updates yet. Since deletes and updates
    // are always leaf nodes, they are always written in the first call to execute().
    bool _wroteDeletesAndUpdates = false;

    // Keeps track of which insertion or subDiff is being serialized.
    size_t _insertIdx = 0;
    size_t _subDiffIdx = 0;

    boost::optional<BSONObjBuilder> _insertBob;
};

// Stack frame used to maintain state while serializing ArrayNodes.
class ArrayFrame final : public Frame {
public:
    ArrayFrame(const ArrayNode& node, BSONObjBuilder bob)
        : _node(node), _bob(std::move(bob)), _childIt(node.children.begin()) {}

    UniqueFrame execute() override {
        invariant(!_node.children.empty());
        if (_childIt == _node.children.begin()) {
            _bob.append(doc_diff::kArrayHeader, true);
        }

        for (; _childIt != _node.children.end(); ++_childIt) {

            auto&& [idx, child] = *_childIt;
            auto idxAsStr = std::to_string(idx);

            switch (child->type()) {
                case (NodeType::kUpdate): {
                    const auto& valueNode = checked_cast<const UpdateNode&>(*child);
                    appendElementToBuilder(
                        valueNode.elt, doc_diff::kUpdateSectionFieldName + idxAsStr, &_bob);
                    break;
                }
                case (NodeType::kInsert): {
                    const auto& valueNode = checked_cast<const InsertNode&>(*child);
                    appendElementToBuilder(
                        valueNode.elt, doc_diff::kUpdateSectionFieldName + idxAsStr, &_bob);
                    break;
                }
                case (NodeType::kDocumentInsert): {
                    // This represents an array element that is being created with a sub object.
                    //
                    // For example {$set: {"a.0.c": 1}} when the input document is {a: []}. Here we
                    // need to create the array element at '0', then sub document 'c'.

                    ++_childIt;
                    return std::make_unique<DocumentInsertFrame>(
                        *checked_cast<DocumentInsertionNode*>(child.get()),
                        BSONObjBuilder(
                            _bob.subobjStart(doc_diff::kUpdateSectionFieldName + idxAsStr)));
                }
                case (NodeType::kDocumentSubDiff):
                case (NodeType::kArray): {
                    InternalNode* subNode = checked_cast<InternalNode*>(child.get());
                    BSONObjBuilder childBuilder = _bob.subobjStart(
                        std::string(1, doc_diff::kSubDiffSectionFieldPrefix) + idxAsStr);

                    ++_childIt;
                    return makeSubNodeFrameHelper(subNode, std::move(childBuilder));
                }
                case (NodeType::kDelete): {
                    MONGO_UNREACHABLE;
                }
            }
        }

        return nullptr;
    }

private:
    const ArrayNode& _node;
    BSONObjBuilder _bob;
    decltype(ArrayNode::children)::const_iterator _childIt;
};

BSONObj writeDiff(const DocumentSubDiffNode& root) {
    std::stack<UniqueFrame> stack;
    stack.push(std::make_unique<DocumentSubDiffFrame>(root, BSONObjBuilder{}));

    // Iterate until the stack size is one and there is no more work to be done.
    while (true) {
        auto nextFrame = stack.top()->execute();
        if (nextFrame) {
            stack.push(std::move(nextFrame));
        } else if (stack.size() == 1) {
            break;
        } else {
            stack.pop();
        }
    }

    invariant(stack.size() == 1);

    auto& topFrame = checked_cast<DocumentSubDiffFrame&>(*stack.top());
    return topFrame.bob().obj();
}

UniqueFrame makeSubNodeFrameHelper(InternalNode* node, BSONObjBuilder builder) {
    if (node->type() == NodeType::kArray) {
        return std::make_unique<ArrayFrame>(*checked_cast<ArrayNode*>(node), std::move(builder));
    } else {
        // We never expect to see a DocumentInsertionNode under the 'subDiffs' section of an
        // internal node.
        invariant(node->type() == NodeType::kDocumentSubDiff);
        return std::make_unique<DocumentSubDiffFrame>(*checked_cast<DocumentSubDiffNode*>(node),
                                                      std::move(builder));
    }
}

UniqueFrame handleInsertHelper(StringData fieldName, Node* node, BSONObjBuilder* bob) {
    if (node->type() == NodeType::kInsert) {
        appendElementToBuilder(checked_cast<InsertNode*>(node)->elt, fieldName, bob);
        return nullptr;
    }
    invariant(node->type() == NodeType::kDocumentInsert);
    return std::make_unique<DocumentInsertFrame>(*checked_cast<DocumentInsertionNode*>(node),
                                                 BSONObjBuilder(bob->subobjStart(fieldName)));
}
}  // namespace

BSONObj V2LogBuilder::serialize() const {
    auto diff = writeDiff(_root);
    return update_oplog_entry::makeDeltaOplogEntry(diff);
}
}  // namespace mongo::v2_log_builder
