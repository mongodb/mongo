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
                            Node* diffNode);

std::unique_ptr<diff_tree::ArrayNode> computeArrayDiff(const BSONObj& pre, const BSONObj& post) {
    auto diffNode = std::make_unique<diff_tree::ArrayNode>();
    auto preItr = BSONObjIterator(pre);
    auto postItr = BSONObjIterator(post);
    const size_t postObjSize = static_cast<size_t>(post.objsize());
    size_t nFieldsInPostArray = 0;
    while (preItr.more() && postItr.more()) {
        // Bailout if the generated diff so far is larger than the 'post' object.
        if (postObjSize < diffNode->getObjSize()) {
            return nullptr;
        }

        auto preVal = *preItr;
        auto postVal = *postItr;

        if (!preVal.binaryEqual(postVal)) {
            // If both are arrays or objects, then recursively compute the diff of the respective
            // array or object.
            if (preVal.type() == postVal.type() &&
                (preVal.type() == BSONType::Object || preVal.type() == BSONType::Array)) {
                calculateSubDiffHelper(preVal, postVal, nFieldsInPostArray, diffNode.get());
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
    return (postObjSize > diffNode->getObjSize()) ? std::move(diffNode) : nullptr;
}

std::unique_ptr<diff_tree::DocumentSubDiffNode> computeDocDiff(const BSONObj& pre,
                                                               const BSONObj& post,
                                                               size_t padding = 0) {
    auto diffNode = std::make_unique<diff_tree::DocumentSubDiffNode>(padding);
    BSONObjIterator preItr(pre);
    BSONObjIterator postItr(post);
    const size_t postObjSize = static_cast<size_t>(post.objsize());
    std::set<StringData> deletes;
    while (preItr.more() && postItr.more()) {
        // Bailout if the generated diff so far is larger than the 'post' object.
        if (postObjSize < diffNode->getObjSize()) {
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
                    preVal, postVal, preVal.fieldNameStringData(), diffNode.get());
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
    return (postObjSize > diffNode->getObjSize()) ? std::move(diffNode) : nullptr;
}

template <class Node, class T>
void calculateSubDiffHelper(const BSONElement& preVal,
                            const BSONElement& postVal,
                            T fieldIdentifier,
                            Node* diffNode) {
    auto subDiff = (preVal.type() == BSONType::Object)
        ? std::unique_ptr<diff_tree::InternalNode>(
              computeDocDiff(preVal.embeddedObject(), postVal.embeddedObject()))
        : std::unique_ptr<diff_tree::InternalNode>(
              computeArrayDiff(preVal.embeddedObject(), postVal.embeddedObject()));
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

template <class DiffNode>
bool anyIndexesMightBeAffected(const DiffNode* node,
                               const UpdateIndexData* indexData,
                               FieldRef* path) {
    for (auto&& [field, child] : node->getChildren()) {
        // The 'field' here can either be an integer or a string.
        StringWrapper wrapper(field);
        FieldRef::FieldRefTempAppend tempAppend(*path, wrapper.getStr());
        switch (child->type()) {
            case diff_tree::NodeType::kDelete:
            case diff_tree::NodeType::kUpdate:
            case diff_tree::NodeType::kInsert: {
                if (indexData && indexData->mightBeIndexed(*path)) {
                    return true;
                }
                break;
            }
            case diff_tree::NodeType::kDocumentSubDiff: {
                if (anyIndexesMightBeAffected<diff_tree::DocumentSubDiffNode>(
                        checked_cast<const diff_tree::DocumentSubDiffNode*>(child.get()),
                        indexData,
                        path)) {
                    return true;
                }
                break;
            }
            case diff_tree::NodeType::kArray: {
                auto* arrayNode = checked_cast<const diff_tree::ArrayNode*>(child.get());
                if ((arrayNode->getResize() && indexData && indexData->mightBeIndexed(*path)) ||
                    anyIndexesMightBeAffected<diff_tree::ArrayNode>(arrayNode, indexData, path)) {
                    return true;
                }
                break;
            }
            case diff_tree::NodeType::kDocumentInsert: {
                MONGO_UNREACHABLE;
            }
        }
    }
    return false;
}
}  // namespace

boost::optional<DiffResult> computeDiff(const BSONObj& pre,
                                        const BSONObj& post,
                                        size_t padding,
                                        const UpdateIndexData* indexData) {
    if (auto diffNode = computeDocDiff(pre, post, padding)) {
        auto diff = diffNode->serialize();
        if (diff.objsize() < post.objsize()) {
            FieldRef path;
            return DiffResult{diff,
                              anyIndexesMightBeAffected<diff_tree::DocumentSubDiffNode>(
                                  diffNode.get(), indexData, &path)};
        }
    }
    return {};
}
}  // namespace mongo::doc_diff
