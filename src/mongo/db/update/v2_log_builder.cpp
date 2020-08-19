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
    auto newNode = std::make_unique<InsertElement>(elt);
    addNodeAtPath(path, &_root, std::move(newNode), idxOfFirstNewComponent);
    return Status::OK();
}

Status V2LogBuilder::logCreatedField(const RuntimeUpdatePath& path,
                                     int idxOfFirstNewComponent,
                                     BSONElement elt) {
    auto newNode = std::make_unique<InsertElement>(elt);
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
void serializeNewlyCreatedDocument(DocumentNode* const node, BSONObjBuilder* out) {
    for (auto&& [fieldName, child] : node->inserts) {
        if (child->type() == NodeType::kInsert) {
            appendElementToBuilder(checked_cast<InsertElement*>(child)->elt, fieldName, out);
            continue;
        }

        BSONObjBuilder childBuilder(out->subobjStart(fieldName));
        serializeNewlyCreatedDocument(checked_cast<DocumentNode* const>(child), &childBuilder);
    }
}

// Mutually recursive with writeArrayDiff().
void writeSubNodeHelper(InternalNode* node, BSONObjBuilder* builder);

void writeArrayDiff(const ArrayNode& node, BSONObjBuilder* builder) {
    builder->append(doc_diff::kArrayHeader, true);
    for (auto&& [idx, child] : node.children) {
        auto idxAsStr = std::to_string(idx);
        switch (child->type()) {
            case (NodeType::kUpdate): {
                const auto& valueNode = checked_cast<const UpdateNode&>(*child);
                appendElementToBuilder(
                    valueNode.elt, doc_diff::kUpdateSectionFieldName + idxAsStr, builder);
                break;
            }
            case (NodeType::kInsert): {
                const auto& valueNode = checked_cast<const InsertElement&>(*child);
                appendElementToBuilder(
                    valueNode.elt, doc_diff::kUpdateSectionFieldName + idxAsStr, builder);
                break;
            }
            case (NodeType::kDocumentInsert): {
                // This represents that the array element is being created which has a sub-object.
                //
                // For example {$set: {"a.0.c": 1}} when the input document is {a: []}. Here we need
                // to create the array element at '0', then sub document 'c'.
                BSONObjBuilder childBuilder =
                    builder->subobjStart(doc_diff::kUpdateSectionFieldName + idxAsStr);
                serializeNewlyCreatedDocument(checked_cast<DocumentNode*>(child.get()),
                                              &childBuilder);
                break;
            }
            case (NodeType::kDocumentSubDiff):
            case (NodeType::kArray): {
                InternalNode* subNode = checked_cast<InternalNode*>(child.get());
                BSONObjBuilder childBuilder = builder->subobjStart(
                    std::string(1, doc_diff::kSubDiffSectionFieldPrefix) + idxAsStr);
                writeSubNodeHelper(subNode, &childBuilder);
                break;
            }
            case (NodeType::kDelete): {
                MONGO_UNREACHABLE;
            }
        }
    }
}

void writeDocumentDiff(const DocumentNode& node, BSONObjBuilder* builder) {
    if (!node.deletes.empty()) {
        BSONObjBuilder subBob(builder->subobjStart(doc_diff::kDeleteSectionFieldName));
        for (auto&& [fieldName, node] : node.deletes) {
            // The deletes are logged in the form {fieldName: false} in $v:2 format.
            subBob.append(fieldName, false);
        }
    }
    if (!node.updates.empty()) {
        BSONObjBuilder subBob(builder->subobjStart(doc_diff::kUpdateSectionFieldName));
        for (auto&& [fieldName, node] : node.updates) {
            appendElementToBuilder(node->elt, fieldName, &subBob);
        }
    }

    if (!node.inserts.empty()) {
        BSONObjBuilder insertBob(builder->subobjStart(doc_diff::kInsertSectionFieldName));
        for (auto&& [fieldName, child] : node.inserts) {
            if (child->type() == NodeType::kInsert) {
                appendElementToBuilder(
                    checked_cast<InsertElement*>(child)->elt, fieldName, &insertBob);
                continue;
            }
            // This represents a new document. While the modifier-style update system
            // was capable of writing paths which would implicitly create new
            // documents, there is no equivalent in $v: 2 updates.
            //
            // For example {$set: {"a.b.c": 1}} would create document 'a' and 'a.b' if
            // necessary.
            //
            // Since $v:2 entries don't have this capability, we instead build the new
            // object and log it as an insert. For example, applying the above $set on
            // document {a: {}} will be logged as an insert of the value {b: {c: 1}} on
            // document 'a'.
            invariant(child->type() == NodeType::kDocumentInsert);
            BSONObjBuilder subBob = insertBob.subobjStart(fieldName);
            serializeNewlyCreatedDocument(checked_cast<DocumentNode*>(child), &subBob);
        }
    }

    for (auto&& [fieldName, child] : node.subDiffs) {
        BSONObjBuilder childBuilder =
            builder->subobjStart(std::string(1, doc_diff::kSubDiffSectionFieldPrefix) + fieldName);
        writeSubNodeHelper(child, &childBuilder);
    }
}

void writeSubNodeHelper(InternalNode* node, BSONObjBuilder* builder) {
    if (node->type() == NodeType::kArray) {
        writeArrayDiff(*checked_cast<ArrayNode*>(node), builder);
    } else {
        invariant(node->type() == NodeType::kDocumentSubDiff ||
                  node->type() == NodeType::kDocumentInsert);
        writeDocumentDiff(*checked_cast<DocumentNode*>(node), builder);
    }
}
}  // namespace


BSONObj V2LogBuilder::serialize() const {
    BSONObjBuilder topBuilder;
    writeDocumentDiff(_root, &topBuilder);
    return update_oplog_entry::makeDeltaOplogEntry(topBuilder.obj());
}
}  // namespace mongo::v2_log_builder