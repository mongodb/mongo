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

#include "mongo/db/update/document_diff_serialization.h"

#include <fmt/format.h>
#include <stack>

#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include <boost/container/flat_map.hpp>
#include <boost/container/static_vector.hpp>

namespace mongo {
namespace diff_tree {

void InternalNode::ApproxBSONSizeTracker::addEntry(size_t fieldSize, const Node* node) {
    _size += fieldSize + 2; /* Type byte + null terminator for field name */

    switch (node->type()) {
        case (NodeType::kArray):
        case (NodeType::kDocumentSubDiff):
        case (NodeType::kDocumentInsert): {
            _size += checked_cast<const InternalNode*>(node)->getObjSize();
            break;
        }
        case (NodeType::kUpdate): {
            if (const auto* elem =
                    stdx::get_if<BSONElement>(&checked_cast<const UpdateNode*>(node)->elt)) {
                _size += elem->valuesize();
            }
            break;
        }
        case (NodeType::kInsert): {
            if (const auto* elem =
                    stdx::get_if<BSONElement>(&checked_cast<const InsertNode*>(node)->elt)) {
                _size += elem->valuesize();
            }
            break;
        }
        case (NodeType::kDelete): {
            _size += 1 /* boolean value */;
            break;
        }
    }
}

Node* DocumentSubDiffNode::addChild(StringData fieldName, std::unique_ptr<Node> node) {
    auto* nodePtr = node.get();

    // Add size of field name and the child element.
    sizeTracker.addEntry(fieldName.size(), nodePtr);

    auto result = children.insert({fieldName.toString(), std::move(node)});
    invariant(result.second);
    StringData storedFieldName = result.first->first;
    switch (nodePtr->type()) {
        case (NodeType::kArray):
        case (NodeType::kDocumentSubDiff): {
            sizeTracker.increment(1 /* kSubDiffSectionFieldPrefix */);
            auto internalNode = checked_cast<InternalNode*>(nodePtr);
            subDiffs.push_back({storedFieldName, internalNode});
            return nodePtr;
        }
        case (NodeType::kDocumentInsert):
        case (NodeType::kInsert): {
            if (inserts.empty()) {
                sizeTracker.addSizeForWrapping();
            }
            inserts.push_back({storedFieldName, nodePtr});
            return nodePtr;
        }
        case (NodeType::kDelete): {
            if (deletes.empty()) {
                sizeTracker.addSizeForWrapping();
            }
            deletes.push_back({storedFieldName, checked_cast<DeleteNode*>(nodePtr)});
            return nodePtr;
        }
        case (NodeType::kUpdate): {
            if (updates.empty()) {
                sizeTracker.addSizeForWrapping();
            }
            updates.push_back({storedFieldName, checked_cast<UpdateNode*>(nodePtr)});
            return nodePtr;
        }
    }
    MONGO_UNREACHABLE;
}

namespace {
void appendElementToBuilder(stdx::variant<mutablebson::Element, BSONElement> elem,
                            StringData fieldName,
                            BSONObjBuilder* builder) {
    stdx::visit(
        OverloadedVisitor{[&](const mutablebson::Element& element) {
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
        const auto& inserts = _node.getInserts();
        for (; _insertIdx < inserts.size(); ++_insertIdx) {
            auto&& [fieldName, child] = inserts[_insertIdx];

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
            if (!_node.getDeletes().empty()) {
                BSONObjBuilder subBob(_bob.subobjStart(doc_diff::kDeleteSectionFieldName));
                for (auto&& [fieldName, node] : _node.getDeletes()) {
                    // The deletes are logged in the form {fieldName: false} in $v:2 format.
                    subBob.append(fieldName, false);
                }
            }
            if (!_node.getUpdates().empty()) {
                BSONObjBuilder subBob(_bob.subobjStart(doc_diff::kUpdateSectionFieldName));
                for (auto&& [fieldName, node] : _node.getUpdates()) {
                    appendElementToBuilder(node->elt, fieldName, &subBob);
                }
            }
            _wroteDeletesAndUpdates = true;
        }

        const auto& inserts = _node.getInserts();
        for (; _insertIdx < inserts.size(); ++_insertIdx) {
            if (!_insertBob) {
                _insertBob.emplace(_bob.subobjStart(doc_diff::kInsertSectionFieldName));
            }

            auto&& [fieldName, child] = inserts[_insertIdx];

            if (auto newFrame = handleInsertHelper(fieldName, child, _insertBob.get_ptr())) {
                ++_insertIdx;
                return newFrame;
            }
        }

        if (_insertBob) {
            // All inserts have been written so we destroy the insert builder now.
            _insertBob = boost::none;
        }

        if (_subDiffIdx != _node.getSubDiffs().size()) {
            auto&& [fieldName, child] = _node.getSubDiffs()[_subDiffIdx];

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
        : _node(node), _bob(std::move(bob)), _childIt(node.getChildren().begin()) {}

    UniqueFrame execute() override {
        if (_childIt == _node.getChildren().begin()) {
            // If this is the first execution of this frame, append array header and resize field if
            // present.
            _bob.append(doc_diff::kArrayHeader, true);
            if (auto size = _node.getResize()) {
                _bob.append(doc_diff::kResizeSectionFieldName, static_cast<int>(*size));
            }
        }

        // +1 for leading char. +1 to round up from digits10.
        static constexpr size_t fieldNameSize = std::numeric_limits<std::uint64_t>::digits10 + 2;
        char fieldNameStorage[fieldNameSize];

        auto formatFieldName = [&](char pre, size_t idx) {
            const char* fieldNameStorageEnd =
                fmt::format_to(fieldNameStorage, FMT_STRING("{}{}"), pre, idx);
            return StringData(static_cast<const char*>(fieldNameStorage), fieldNameStorageEnd);
        };

        // Make sure that 'doc_diff::kUpdateSectionFieldName' is a single character.
        static_assert(doc_diff::kUpdateSectionFieldName.size() == 1,
                      "doc_diff::kUpdateSectionFieldName should be a single character.");

        for (; _childIt != _node.getChildren().end(); ++_childIt) {
            auto&& [idx, child] = *_childIt;

            switch (child->type()) {
                case (NodeType::kUpdate): {
                    const auto& valueNode = checked_cast<const UpdateNode&>(*child);
                    appendElementToBuilder(
                        valueNode.elt,
                        formatFieldName(doc_diff::kUpdateSectionFieldName[0], idx),
                        &_bob);
                    break;
                }
                case (NodeType::kInsert): {
                    const auto& valueNode = checked_cast<const InsertNode&>(*child);
                    appendElementToBuilder(
                        valueNode.elt,
                        formatFieldName(doc_diff::kUpdateSectionFieldName[0], idx),
                        &_bob);
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
                        BSONObjBuilder(_bob.subobjStart(
                            formatFieldName(doc_diff::kUpdateSectionFieldName[0], idx))));
                }
                case (NodeType::kDocumentSubDiff):
                case (NodeType::kArray): {
                    InternalNode* subNode = checked_cast<InternalNode*>(child.get());
                    BSONObjBuilder childBuilder = _bob.subobjStart(
                        formatFieldName(doc_diff::kSubDiffSectionFieldPrefix, idx));

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
    std::map<size_t, std::unique_ptr<Node>>::const_iterator _childIt;
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

BSONObj DocumentSubDiffNode::serialize() const {
    return writeDiff(*this);
}
}  // namespace diff_tree

namespace doc_diff {
namespace {

void assertDiffNonEmpty(const BSONObjIterator& it) {
    uassert(4770500, "Expected diff to be non-empty", it.more());
}

// Helper for taking a BSONObj and determining whether it's an array diff or an object diff.
doc_diff::DiffType identifyType(const BSONObj& diff) {
    BSONObjIterator it(diff);
    assertDiffNonEmpty(it);

    if ((*it).fieldNameStringData() == kArrayHeader) {
        return DiffType::kArray;
    }
    return DiffType::kDocument;
}

stdx::variant<DocumentDiffReader, ArrayDiffReader> getReader(const Diff& diff) {
    const auto type = identifyType(diff);
    if (type == DiffType::kArray) {
        return ArrayDiffReader(diff);
    }
    return DocumentDiffReader(diff);
}

void checkSection(BSONElement element, char sectionName, BSONType expectedType) {
    uassert(4770507,
            str::stream() << "Expected " << sectionName << " section to be type " << expectedType,
            element.type() == expectedType);
}

// Converts a (decimal) string to number. Will throw if the string is not a valid unsigned int.
size_t extractArrayIndex(StringData fieldName) {
    auto idx = str::parseUnsignedBase10Integer(fieldName);
    uassert(4770512, str::stream() << "Expected integer but got " << fieldName, idx);
    return *idx;
}
}  // namespace

ArrayDiffReader::ArrayDiffReader(const Diff& diff) : _diff(diff), _it(_diff) {
    assertDiffNonEmpty(_it);
    auto field = *_it;
    uassert(4770504,
            str::stream() << "Expected first field to be array header " << kArrayHeader
                          << " but found " << (*_it).fieldNameStringData(),
            field.fieldNameStringData() == kArrayHeader);
    uassert(4770519,
            str::stream() << "Expected array header to be bool but got " << (*_it),
            field.type() == BSONType::Bool);
    uassert(4770520,
            str::stream() << "Expected array header to be value true but got " << (*_it),
            field.Bool());
    ++_it;
    field = *_it;
    if (_it.more() && field.fieldNameStringData() == kResizeSectionFieldName) {
        checkSection(field, kResizeSectionFieldName[0], BSONType::NumberInt);
        _newSize.emplace(field.numberInt());
        ++_it;
    }
}

boost::optional<std::pair<size_t, ArrayDiffReader::ArrayModification>> ArrayDiffReader::next() {
    if (!_it.more()) {
        return {};
    }

    auto next = _it.next();
    auto fieldName = next.fieldNameStringData();

    uassert(4770521,
            str::stream() << "expected field name to be at least two characters long, but found: "
                          << fieldName,
            fieldName.size() > 1);
    const size_t idx = extractArrayIndex(fieldName.substr(1, fieldName.size()));

    if (fieldName[0] == kUpdateSectionFieldName[0]) {
        // It's an update.
        return {{idx, next}};
    } else if (fieldName[0] == kSubDiffSectionFieldPrefix) {
        // It's a sub diff...But which type?
        uassert(4770501,
                str::stream() << "expected sub diff at index " << idx << " but got " << next,
                next.type() == BSONType::Object);

        auto modification = stdx::visit(
            OverloadedVisitor{[](const auto& reader) -> ArrayModification { return {reader}; }},
            getReader(next.embeddedObject()));
        return {{idx, modification}};
    } else {
        uasserted(4770502,
                  str::stream() << "Expected either 'u' (update) or 's' (sub diff) at index " << idx
                                << " but got " << next);
    }
}

DocumentDiffReader::DocumentDiffReader(const Diff& diff) : _diff(diff) {
    BSONObjIterator it(diff);
    assertDiffNonEmpty(it);

    // Find each section of the diff and initialize an iterator.
    struct Section {
        boost::optional<BSONObjIterator>* outIterator;
        int order;
    };

    static_assert(kDeleteSectionFieldName.size() == 1);
    static_assert(kInsertSectionFieldName.size() == 1);
    static_assert(kUpdateSectionFieldName.size() == 1);

    // Create a map only using stack memory for this temporary helper map. Make sure to update the
    // size for the 'static_vector' if changes are made to the number of elements we hold.
    const boost::container::flat_map<char,
                                     Section,
                                     std::less<char>,
                                     boost::container::static_vector<std::pair<char, Section>, 4>>
        sections{{kDeleteSectionFieldName[0], Section{&_deletes, 1}},
                 {kUpdateSectionFieldName[0], Section{&_updates, 2}},
                 {kInsertSectionFieldName[0], Section{&_inserts, 3}},
                 {kSubDiffSectionFieldPrefix, Section{&_subDiffs, 4}}};

    char prev = 0;
    bool hasSubDiffSections = false;
    for (; it.more(); ++it) {
        const auto field = *it;
        uassert(4770505,
                str::stream() << "Expected sections field names in diff to be non-empty ",
                field.fieldNameStringData().size());
        const auto sectionName = field.fieldNameStringData()[0];
        auto section = sections.find(sectionName);
        if ((section != sections.end()) && (section->second.order > prev)) {
            checkSection(field, sectionName, BSONType::Object);

            // Once we encounter a sub-diff section, we break and save the iterator for later use.
            if (sectionName == kSubDiffSectionFieldPrefix) {
                section->second.outIterator->emplace(it);
                hasSubDiffSections = true;
                break;
            } else {
                section->second.outIterator->emplace(field.embeddedObject());
            }
        } else {
            uasserted(4770503,
                      str::stream()
                          << "Unexpected section: " << sectionName << " in document diff");
        }
        prev = section->second.order;
    }

    uassert(4770513,
            str::stream() << "Did not expect more sections in diff but found one: "
                          << (*it).fieldNameStringData(),
            hasSubDiffSections || !it.more());
}

boost::optional<StringData> DocumentDiffReader::nextDelete() {
    if (!_deletes || !_deletes->more()) {
        return {};
    }

    return _deletes->next().fieldNameStringData();
}

boost::optional<BSONElement> DocumentDiffReader::nextUpdate() {
    if (!_updates || !_updates->more()) {
        return {};
    }
    return _updates->next();
}

boost::optional<BSONElement> DocumentDiffReader::nextInsert() {
    if (!_inserts || !_inserts->more()) {
        return {};
    }
    return _inserts->next();
}

boost::optional<std::pair<StringData, stdx::variant<DocumentDiffReader, ArrayDiffReader>>>
DocumentDiffReader::nextSubDiff() {
    if (!_subDiffs || !_subDiffs->more()) {
        return {};
    }

    auto next = _subDiffs->next();
    const auto fieldName = next.fieldNameStringData();
    uassert(4770514,
            str::stream() << "Did not expect more sections in diff but found one: "
                          << next.fieldNameStringData(),
            fieldName.size() > 0 && fieldName[0] == kSubDiffSectionFieldPrefix);

    uassert(470510,
            str::stream() << "Subdiffs should be objects, got " << next,
            next.type() == BSONType::Object);

    return {{fieldName.substr(1, fieldName.size()), getReader(next.embeddedObject())}};
}
}  // namespace doc_diff
}  // namespace mongo
