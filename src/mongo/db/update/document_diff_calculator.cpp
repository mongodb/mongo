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

#include "mongo/base/checked_cast.h"
#include "mongo/db/update/document_diff_calculator.h"

namespace mongo::doc_diff {
namespace {

// Note: This function is mutually with computeArrayDiff() and computeDocDiff().
template <class Node, class T>
void calculateSubDiffHelper(const BSONElement& preVal,
                            const BSONElement& postVal,
                            T fieldIdentifier,
                            Node* diffNode,
                            bool ignoreSizeLimit);

/**
 * Computes an array diff between the given 'pre' and 'post' images. If 'ignoreSizeLimit' is false,
 * returns a nullptr if the size of the computed diff is larger than the 'post' image.
 */
std::unique_ptr<diff_tree::ArrayNode> computeArrayDiff(const BSONObj& pre,
                                                       const BSONObj& post,
                                                       bool ignoreSizeLimit) {
    auto diffNode = std::make_unique<diff_tree::ArrayNode>();
    auto preItr = BSONObjIterator(pre);
    auto postItr = BSONObjIterator(post);
    const size_t postObjSize = static_cast<size_t>(post.objsize());
    size_t nFieldsInPostArray = 0;
    while (preItr.more() && postItr.more()) {
        // Bailout if the generated diff so far is larger than the 'post' object.
        if (!ignoreSizeLimit && (postObjSize < diffNode->getObjSize())) {
            return nullptr;
        }

        auto preVal = *preItr;
        auto postVal = *postItr;

        if (!preVal.binaryEqual(postVal)) {
            // If both are arrays or objects, then recursively compute the diff of the respective
            // array or object.
            if (preVal.type() == postVal.type() &&
                (preVal.type() == BSONType::Object || preVal.type() == BSONType::Array)) {
                calculateSubDiffHelper(
                    preVal, postVal, nFieldsInPostArray, diffNode.get(), ignoreSizeLimit);
            } else {
                diffNode->addUpdate(nFieldsInPostArray, postVal);
            }
        }

        preItr.advance(preVal);
        postItr.advance(postVal);
        ++nFieldsInPostArray;
    }

    // When we reach here, only one of postItr or preItr can have more fields. If postItr has more
    // fields, we need to add all the remaining fields.
    for (; postItr.more(); ++nFieldsInPostArray) {
        auto postVal = *postItr;
        diffNode->addUpdate(nFieldsInPostArray, postVal);
        postItr.advance(postVal);
    }

    // If preItr has more fields, we can ignore the remaining fields, since we only need to do a
    // resize operation.
    if (preItr.more()) {
        diffNode->setResize(nFieldsInPostArray);
    }
    return (ignoreSizeLimit || (postObjSize > diffNode->getObjSize())) ? std::move(diffNode)
                                                                       : nullptr;
}

/**
 * Computes an object diff between the given 'pre' and 'post' images. If 'ignoreSizeLimit' is false,
 * returns a nullptr if the size of the computed diff is larger than the 'post' image. The
 * 'padding' represents the additional size that needs be added to the size of the diff, while
 * comparing whether the diff is viable.
 */
std::unique_ptr<diff_tree::DocumentSubDiffNode> computeDocDiff(const BSONObj& pre,
                                                               const BSONObj& post,
                                                               bool ignoreSizeLimit,
                                                               size_t padding = 0) {
    auto diffNode = std::make_unique<diff_tree::DocumentSubDiffNode>(padding);
    BSONObjIterator preItr(pre);
    BSONObjIterator postItr(post);
    const size_t postObjSize = static_cast<size_t>(post.objsize());
    std::set<StringData> deletes;
    while (preItr.more() && postItr.more()) {
        // Bailout if the generated diff so far is larger than the 'post' object.
        if (!ignoreSizeLimit && (postObjSize < diffNode->getObjSize())) {
            return nullptr;
        }

        auto preVal = *preItr;
        // Fast path for case where they're equal.
        if (postItr.currentElementBinaryEqual(preVal)) {
            // Here the current element of preItr and postItr are equal. So it's safe to advance
            // 'postItr' using 'preVal'. This way we can save the cost of constructing 'postVal'.
            preItr.advance(preVal);
            postItr.advance(preVal);
            continue;
        }

        auto postVal = *postItr;
        if (preVal.fieldNameStringData() == postVal.fieldNameStringData()) {
            if (preVal.type() == postVal.type() &&
                (preVal.type() == BSONType::Object || preVal.type() == BSONType::Array)) {
                // Both are either arrays or objects, recursively compute the diff of the respective
                // array or object.
                calculateSubDiffHelper(
                    preVal, postVal, preVal.fieldNameStringData(), diffNode.get(), ignoreSizeLimit);
            } else {
                // Any other case, just replace with the 'postVal'.
                diffNode->addUpdate((*preItr).fieldNameStringData(), postVal);
            }
            preItr.advance(preVal);
            postItr.advance(postVal);
        } else {
            // If the 'preVal' field name does not exist in the 'post' object then, just remove it.
            // If it present, we do nothing for now, since the field gets inserted later.
            deletes.insert(preVal.fieldNameStringData());
            preItr.advance(preVal);
        }
    }

    // When we reach here, only one of postItr or preItr can have more fields. Record remaining
    // fields in preItr as removals.
    while (preItr.more()) {
        // Note that we don't need to record these into the 'deletes' set because there are no more
        // fields in the post image.
        invariant(!postItr.more());
        auto next = (*preItr);
        diffNode->addDelete(next.fieldNameStringData());
        preItr.advance(next);
    }

    // Record remaining fields in postItr as creates.
    while (postItr.more()) {
        auto next = (*postItr);

        diffNode->addInsert(next.fieldNameStringData(), next);
        deletes.erase(next.fieldNameStringData());
        postItr.advance(next);
    }
    for (auto&& deleteField : deletes) {
        diffNode->addDelete(deleteField);
    }
    return (ignoreSizeLimit || (postObjSize > diffNode->getObjSize())) ? std::move(diffNode)
                                                                       : nullptr;
}

template <class Node, class T>
void calculateSubDiffHelper(const BSONElement& preVal,
                            const BSONElement& postVal,
                            T fieldIdentifier,
                            Node* diffNode,
                            bool ignoreSizeLimit) {
    auto subDiff = (preVal.type() == BSONType::Object)
        ? std::unique_ptr<diff_tree::InternalNode>(
              computeDocDiff(preVal.embeddedObject(), postVal.embeddedObject(), ignoreSizeLimit))
        : std::unique_ptr<diff_tree::InternalNode>(
              computeArrayDiff(preVal.embeddedObject(), postVal.embeddedObject(), ignoreSizeLimit));
    if (!subDiff) {
        // We could not compute sub-diff because the potential sub-diff is bigger than the 'postVal'
        // itself. So we just log the modification as an update.
        diffNode->addUpdate(fieldIdentifier, postVal);
    } else {
        diffNode->addChild(fieldIdentifier, std::move(subDiff));
    }
}

class StringWrapper {
public:
    StringWrapper(size_t s) : storage(std::to_string(s)), str(storage) {}
    StringWrapper(StringData s) : str(s) {}

    StringData getStr() {
        return str;
    }

private:
    std::string storage;
    StringData str;
};

/**
 * Appends the given element to the given BSONObjBuilder. If the element is an object, sets the
 * value of the most inner field(s) to 'innerValue'. Otherwise, sets the value of the field to
 * 'outerValue'.
 */
void appendFieldNested(stdx::variant<mutablebson::Element, BSONElement> elt,
                       StringData outerValue,
                       StringData innerValue,
                       BSONObjBuilder* bob) {
    stdx::visit(OverloadedVisitor{
                    [&](const mutablebson::Element& element) {
                        auto fieldName = element.getFieldName();
                        if (element.getType() == BSONType::Object) {
                            auto elementObj = element.getValueObject();
                            if (!elementObj.isEmpty()) {
                                BSONObjBuilder subBob(bob->subobjStart(fieldName));
                                for (const auto& subElement : elementObj) {
                                    appendFieldNested(subElement, innerValue, innerValue, &subBob);
                                }
                                return;
                            }
                        }
                        bob->append(fieldName, outerValue);
                    },
                    [&](BSONElement element) {
                        auto fieldName = element.fieldNameStringData();
                        if (element.type() == BSONType::Object) {
                            auto elementObj = element.Obj();
                            if (!elementObj.isEmpty()) {
                                BSONObjBuilder subBob(bob->subobjStart(fieldName));
                                for (const auto& subElement : elementObj) {
                                    appendFieldNested(subElement, innerValue, innerValue, &subBob);
                                }
                                return;
                            }
                        }
                        bob->append(fieldName, outerValue);
                    }},
                elt);
}

void serializeInlineDiff(diff_tree::DocumentSubDiffNode const* node, BSONObjBuilder* bob) {
    for (auto&& [field, child] : node->getChildren()) {
        StringWrapper wrapper(field);
        auto fieldName = wrapper.getStr().toString();
        switch (child->type()) {
            case diff_tree::NodeType::kInsert: {
                appendFieldNested(checked_cast<const diff_tree::InsertNode&>(*child).elt,
                                  doc_diff::kInsertSectionFieldName, /* outerValue */
                                  doc_diff::kInsertSectionFieldName, /* innerValue */
                                  bob);
                break;
            }
            case diff_tree::NodeType::kUpdate: {
                appendFieldNested(checked_cast<const diff_tree::UpdateNode&>(*child).elt,
                                  doc_diff::kUpdateSectionFieldName, /* outerValue */
                                  doc_diff::kInsertSectionFieldName, /* innerValue */
                                  bob);
                break;
            }
            case diff_tree::NodeType::kDelete: {
                bob->append(fieldName, doc_diff::kDeleteSectionFieldName);
                break;
            }
            case diff_tree::NodeType::kDocumentSubDiff: {
                BSONObjBuilder subBob(bob->subobjStart(fieldName));
                serializeInlineDiff(
                    checked_cast<const diff_tree::DocumentSubDiffNode*>(child.get()), &subBob);
                break;
            }
            case diff_tree::NodeType::kArray: {
                bob->append(fieldName, doc_diff::kUpdateSectionFieldName);
                break;
            }
            case diff_tree::NodeType::kDocumentInsert: {
                MONGO_UNREACHABLE;
            }
        }
    }
}

void anyIndexesMightBeAffected(ArrayDiffReader* reader,
                               const std::vector<const UpdateIndexData*>& indexData,
                               FieldRef* fieldRef,
                               BitVector* result);

void anyIndexesMightBeAffected(DocumentDiffReader* reader,
                               const std::vector<const UpdateIndexData*>& indexData,
                               FieldRef* fieldRef,
                               BitVector* result) {
    boost::optional<StringData> delItem;
    while ((delItem = reader->nextDelete())) {
        FieldRef::FieldRefTempAppend tempAppend(*fieldRef, *delItem);
        for (size_t i = 0; i < indexData.size(); i++) {
            if (!(*result)[i]) {
                (*result)[i] = indexData[i]->mightBeIndexed(*fieldRef);
            }
        }
        // early exit
        if (result->all()) {
            return;
        }
    }

    boost::optional<BSONElement> updItem;
    while ((updItem = reader->nextUpdate())) {
        FieldRef::FieldRefTempAppend tempAppend(*fieldRef, updItem->fieldNameStringData());
        for (size_t i = 0; i < indexData.size(); i++) {
            if (!(*result)[i]) {
                (*result)[i] = indexData[i]->mightBeIndexed(*fieldRef);
            }
        }
        // early exit
        if (result->all()) {
            return;
        }
    }

    boost::optional<BSONElement> insItem;
    while ((insItem = reader->nextInsert())) {
        FieldRef::FieldRefTempAppend tempAppend(*fieldRef, insItem->fieldNameStringData());
        for (size_t i = 0; i < indexData.size(); i++) {
            if (!(*result)[i]) {
                (*result)[i] = indexData[i]->mightBeIndexed(*fieldRef);
            }
        }
        // early exit
        if (result->all()) {
            return;
        }
    }

    for (auto subItem = reader->nextSubDiff(); subItem; subItem = reader->nextSubDiff()) {
        FieldRef::FieldRefTempAppend tempAppend(*fieldRef, subItem->first);
        stdx::visit(
            OverloadedVisitor{[&indexData, &fieldRef, &result](DocumentDiffReader& item) {
                                  anyIndexesMightBeAffected(&item, indexData, fieldRef, result);
                              },
                              [&indexData, &fieldRef, &result](ArrayDiffReader& item) {
                                  anyIndexesMightBeAffected(&item, indexData, fieldRef, result);
                              }},
            subItem->second);
        // early exit
        if (result->all()) {
            return;
        }
    }
}

void anyIndexesMightBeAffected(ArrayDiffReader* reader,
                               const std::vector<const UpdateIndexData*>& indexData,
                               FieldRef* fieldRef,
                               BitVector* result) {
    if (reader->newSize()) {
        for (size_t i = 0; i < indexData.size(); i++) {
            if (!(*result)[i]) {
                (*result)[i] = indexData[i]->mightBeIndexed(*fieldRef);
            }
        }
        // early exit
        if (result->all()) {
            return;
        }
    }
    for (auto item = reader->next(); item; item = reader->next()) {
        auto idxAsStr = std::to_string(item->first);
        FieldRef::FieldRefTempAppend tempAppend(*fieldRef, idxAsStr);
        stdx::visit(
            OverloadedVisitor{[&indexData, &fieldRef, &result](BSONElement& update) {
                                  for (size_t i = 0; i < indexData.size(); i++) {
                                      if (!(*result)[i]) {
                                          (*result)[i] = indexData[i]->mightBeIndexed(*fieldRef);
                                      }
                                  }
                              },
                              [&indexData, &fieldRef, &result](DocumentDiffReader& item) {
                                  anyIndexesMightBeAffected(&item, indexData, fieldRef, result);
                              },
                              [&indexData, &fieldRef, &result](ArrayDiffReader& item) {
                                  anyIndexesMightBeAffected(&item, indexData, fieldRef, result);
                              }},
            item->second);
        // early exit
        if (result->all()) {
            return;
        }
    }
}
}  // namespace

boost::optional<Diff> computeOplogDiff(const BSONObj& pre, const BSONObj& post, size_t padding) {
    if (auto diffNode = computeDocDiff(pre, post, false /* ignoreSizeLimit */, padding)) {
        auto diff = diffNode->serialize();
        if (diff.objsize() < post.objsize()) {
            return diff;
        }
    }
    return {};
}

boost::optional<BSONObj> computeInlineDiff(const BSONObj& pre, const BSONObj& post) {
    auto diffNode = computeDocDiff(pre, post, true /* ignoreSizeLimit */, 0);
    BSONObjBuilder bob;
    serializeInlineDiff(diffNode.get(), &bob);
    if (bob.bb().len() >= BSONObjMaxUserSize) {
        return boost::none;
    }
    return bob.obj();
}


BitVector anyIndexesMightBeAffected(const Diff& diff,
                                    const std::vector<const UpdateIndexData*>& indexData) {
    invariant(!indexData.empty());
    BitVector result(indexData.size());
    if (diff.isEmpty()) {
        return result;
    }

    DocumentDiffReader reader(diff);
    FieldRef path;
    anyIndexesMightBeAffected(&reader, indexData, &path, &result);
    return result;
}
}  // namespace mongo::doc_diff
