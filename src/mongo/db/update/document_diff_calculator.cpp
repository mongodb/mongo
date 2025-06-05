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

#include "mongo/db/update/document_diff_calculator.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/update_index_data.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include <absl/container/node_hash_map.h>
#include <boost/dynamic_bitset/dynamic_bitset.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
                (preVal.type() == BSONType::object || preVal.type() == BSONType::array)) {
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
                (preVal.type() == BSONType::object || preVal.type() == BSONType::array)) {
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
        tassert(7639001, "postItr needs to be empty", !postItr.more());

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
    auto subDiff = (preVal.type() == BSONType::object)
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
void appendFieldNested(std::variant<mutablebson::Element, BSONElement> elt,
                       StringData outerValue,
                       StringData innerValue,
                       BSONObjBuilder* bob) {
    visit(OverloadedVisitor{
              [&](const mutablebson::Element& element) {
                  auto fieldName = element.getFieldName();
                  if (element.getType() == BSONType::object) {
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
                  if (element.type() == BSONType::object) {
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
            case diff_tree::NodeType::kBinary: {
                MONGO_UNIMPLEMENTED;
            }
            case diff_tree::NodeType::kDelete: {
                bob->append(StringData{field}, doc_diff::kDeleteSectionFieldName);
                break;
            }
            case diff_tree::NodeType::kDocumentSubDiff: {
                BSONObjBuilder subBob(bob->subobjStart(StringData{field}));
                serializeInlineDiff(
                    checked_cast<const diff_tree::DocumentSubDiffNode*>(child.get()), &subBob);
                break;
            }
            case diff_tree::NodeType::kArray: {
                bob->append(StringData{field}, doc_diff::kUpdateSectionFieldName);
                break;
            }
            case diff_tree::NodeType::kDocumentInsert: {
                MONGO_UNREACHABLE;
            }
        }
    }
}

}  // namespace

IndexUpdateIdentifier::IndexUpdateIdentifier(size_t numIndexes) : _numIndexes(numIndexes) {}

void IndexUpdateIdentifier::addIndex(size_t indexCounter, const UpdateIndexData& updateIndexData) {
    tassert(7639000, "indexCounter should be less than _numIndexes", indexCounter < _numIndexes);

    auto addCurrentIndexToIndexSet = [&](IndexSet& indexSet) {
        indexSet.set(indexCounter);
    };

    // Add the current index to the index set of wildcard indexes, if the index is a wildcard index.
    if (updateIndexData.allPathsIndexed()) {
        addCurrentIndexToIndexSet(_allPathsIndexed);
        // No need to continue with inserting path-specific data if we get here.
        return;
    }

    // Add the current index to the index sets for all canonical paths covered by the index.
    for (const auto& path : updateIndexData.getCanonicalPaths()) {
        // Check if we already have an entry for the same path or not.
        // Note that this loop has O(n x m) runtime complexity where 'n' is the number of canonical
        // paths in the index entry, and 'm' is the number of existing canonical paths of other
        // indexes in the same collection. This should not be a problem in practice, as the maximum
        // number of indexes is limited to currently 64 per collection, and indexes normally tend to
        // cover only few fields. Also note that for wildcard indexes and wildcard text indexes that
        // could cover a lot of fields, we do not store any paths here.
        auto foundPathEntry = std::find_if(_canonicalPathsToIndexSets.begin(),
                                           _canonicalPathsToIndexSets.end(),
                                           [&](const auto& entry) { return entry.first == path; });
        if (foundPathEntry == _canonicalPathsToIndexSets.end()) {
            // No such entry yet. We create a new one and make 'it' point to it.
            foundPathEntry = _canonicalPathsToIndexSets.insert(_canonicalPathsToIndexSets.end(),
                                                               std::make_pair(path, IndexSet{}));
        }
        addCurrentIndexToIndexSet(foundPathEntry->second);
    }

    // Add the current index to an index set associated with each path component used by the current
    // index.
    for (const auto& pathComponent : updateIndexData.getPathComponents()) {
        addCurrentIndexToIndexSet(_pathComponentsToIndexSets[pathComponent]);
    }
}

IndexSet IndexUpdateIdentifier::determineAffectedIndexes(const Diff& diff) const {
    if (diff.isEmpty()) {
        // No diff -> nothing to do!
        return IndexSet{};
    }

    IndexSet indexesToUpdate = _allPathsIndexed;
    dassert(indexesToUpdate.size() >= _numIndexes);

    DocumentDiffReader reader(diff);
    FieldRef path;
    determineAffectedIndexes(&reader, path, indexesToUpdate);

    // Expect that the set of wildcard indexes is a subset of all indexes to be updated.
    dassert((_allPathsIndexed & indexesToUpdate) == _allPathsIndexed);

    return indexesToUpdate;
}

void IndexUpdateIdentifier::determineAffectedIndexes(const FieldRef& path,
                                                     IndexSet& indexesToUpdate) const {
    for (const auto& indexedPath : _canonicalPathsToIndexSets) {
        if ((indexesToUpdate & indexedPath.second) == indexedPath.second) {
            // Already handled all the indexes for which we have canonical paths.
            // This is an important speedup.
            continue;
        }
        if (FieldRef::pathOverlaps(path, indexedPath.first)) {
            dassert(indexesToUpdate.size() == indexedPath.second.size());
            indexesToUpdate |= indexedPath.second;
        }
    }

    if (!_pathComponentsToIndexSets.empty()) {
        for (size_t partIdx = 0; partIdx < path.numParts(); ++partIdx) {
            // Look up path component in hash table.
            StringData part = path.getPart(partIdx);

            // Converting to std::string_view here to avoid heap allocations for temporary
            // std::string lookup values.
            if (auto foundPathComponent =
                    _pathComponentsToIndexSets.find(toStdStringViewForInterop(part));
                foundPathComponent != _pathComponentsToIndexSets.end()) {
                // Path component found. Now add the index positions to the IndexSet.
                dassert(indexesToUpdate.size() == foundPathComponent->second.size());
                indexesToUpdate |= foundPathComponent->second;
            }
        }
    }
}

void IndexUpdateIdentifier::determineAffectedIndexes(DocumentDiffReader* reader,
                                                     FieldRef& fieldRef,
                                                     IndexSet& indexesToUpdate) const {
    boost::optional<StringData> delItem;
    while ((delItem = reader->nextDelete())) {
        FieldRef::FieldRefTempAppend tempAppend(fieldRef, *delItem);
        determineAffectedIndexes(fieldRef, indexesToUpdate);

        // Early exit if possible.
        if (indexesToUpdate.count() == _numIndexes) {
            return;
        }
    }

    boost::optional<BSONElement> updItem;
    while ((updItem = reader->nextUpdate())) {
        FieldRef::FieldRefTempAppend tempAppend(fieldRef, updItem->fieldNameStringData());
        determineAffectedIndexes(fieldRef, indexesToUpdate);

        // Early exit.
        if (indexesToUpdate.count() == _numIndexes) {
            return;
        }
    }

    boost::optional<BSONElement> insItem;
    while ((insItem = reader->nextInsert())) {
        FieldRef::FieldRefTempAppend tempAppend(fieldRef, insItem->fieldNameStringData());
        determineAffectedIndexes(fieldRef, indexesToUpdate);

        // Early exit if possible.
        if (indexesToUpdate.count() == _numIndexes) {
            return;
        }
    }

    for (auto subItem = reader->nextSubDiff(); subItem; subItem = reader->nextSubDiff()) {
        FieldRef::FieldRefTempAppend tempAppend(fieldRef, subItem->first);
        visit(OverloadedVisitor{[this, &fieldRef, &indexesToUpdate](DocumentDiffReader& item) {
                                    determineAffectedIndexes(&item, fieldRef, indexesToUpdate);
                                },
                                [this, &fieldRef, &indexesToUpdate](ArrayDiffReader& item) {
                                    determineAffectedIndexes(&item, fieldRef, indexesToUpdate);
                                }},
              subItem->second);

        // Early exit if possible.
        if (indexesToUpdate.count() == _numIndexes) {
            return;
        }
    }
}

void IndexUpdateIdentifier::determineAffectedIndexes(ArrayDiffReader* reader,
                                                     FieldRef& fieldRef,
                                                     IndexSet& indexesToUpdate) const {
    if (reader->newSize()) {
        determineAffectedIndexes(fieldRef, indexesToUpdate);

        // Early exit if possible.
        if (indexesToUpdate.count() == _numIndexes) {
            return;
        }
    }
    for (auto item = reader->next(); item; item = reader->next()) {
        auto idxAsStr = std::to_string(item->first);
        FieldRef::FieldRefTempAppend tempAppend(fieldRef, idxAsStr);
        visit(OverloadedVisitor{[this, &fieldRef, &indexesToUpdate](BSONElement& update) {
                                    determineAffectedIndexes(fieldRef, indexesToUpdate);
                                },
                                [this, &fieldRef, &indexesToUpdate](DocumentDiffReader& item) {
                                    determineAffectedIndexes(&item, fieldRef, indexesToUpdate);
                                },
                                [this, &fieldRef, &indexesToUpdate](ArrayDiffReader& item) {
                                    determineAffectedIndexes(&item, fieldRef, indexesToUpdate);
                                }},
              item->second);

        // Early exit if possible.
        if (indexesToUpdate.count() == _numIndexes) {
            return;
        }
    }
}

boost::optional<Diff> computeOplogDiff(const BSONObj& pre, const BSONObj& post, size_t padding) {
    if (auto diffNode = computeDocDiff(pre, post, false /* ignoreSizeLimit */, padding)) {
        auto diff = diffNode->serialize();
        if (diff.objsize() < post.objsize()) {
            return diff;
        }
    }
    return {};
}


Diff computeOplogDiff_forTest(const BSONObj& pre, const BSONObj& post) {
    // Compute the full document differences between 'pre' and 'post'.
    auto diffNode = computeDocDiff(pre, post, true /* ignoreSizeLimit */, 0 /* padding */);
    tassert(7639006, "diffNode should always have a value", diffNode);
    return diffNode->serialize();
}

boost::optional<BSONObj> computeInlineDiff(const BSONObj& pre, const BSONObj& post) {
    // Compute the full document differences between 'pre' and 'post'.
    auto diffNode = computeDocDiff(pre, post, true /* ignoreSizeLimit */, 0);
    BSONObjBuilder bob;
    serializeInlineDiff(diffNode.get(), &bob);
    if (bob.bb().len() >= BSONObjMaxUserSize) {
        return boost::none;
    }
    return bob.obj();
}

}  // namespace mongo::doc_diff
