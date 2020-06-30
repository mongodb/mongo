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

#include "mongo/db/update/document_diff_calculator.h"

namespace mongo::doc_diff {
namespace {

// Note: This function is mutually with computeArrayDiff() and computeDocDiff().
template <class DiffBuilder, class T>
void calculateSubDiffHelper(const BSONElement& preVal,
                            const BSONElement& postVal,
                            T fieldIdentifier,
                            DiffBuilder* builder);

bool computeArrayDiff(const BSONObj& pre,
                      const BSONObj& post,
                      doc_diff::ArrayDiffBuilder* diffBuilder) {
    auto preItr = BSONObjIterator(pre);
    auto postItr = BSONObjIterator(post);
    const size_t postObjSize = static_cast<size_t>(post.objsize());
    size_t nFieldsInPostArray = 0;
    for (; preItr.more() && postItr.more(); ++preItr, ++postItr, ++nFieldsInPostArray) {
        // Bailout if the generated diff so far is larger than the 'post' object.
        if (postObjSize < diffBuilder->getObjSize()) {
            return false;
        }
        if (!(*preItr).binaryEqual(*postItr)) {
            // If both are arrays or objects, then recursively compute the diff of the respective
            // array or object.
            if ((*preItr).type() == (*postItr).type() &&
                ((*preItr).type() == BSONType::Object || (*preItr).type() == BSONType::Array)) {
                calculateSubDiffHelper(*preItr, *postItr, nFieldsInPostArray, diffBuilder);
            } else {
                diffBuilder->addUpdate(nFieldsInPostArray, *postItr);
            }
        }
    }

    // When we reach here, only one of postItr or preItr can have more fields. If postItr has more
    // fields, we need to add all the remaining fields.
    for (; postItr.more(); ++postItr, ++nFieldsInPostArray) {
        diffBuilder->addUpdate(nFieldsInPostArray, *postItr);
    }

    // If preItr has more fields, we can ignore the remaining fields, since we only need to do a
    // resize operation.
    if (preItr.more()) {
        diffBuilder->setResize(nFieldsInPostArray);
    }
    return postObjSize > diffBuilder->getObjSize();
}

bool computeDocDiff(const BSONObj& pre,
                    const BSONObj& post,
                    doc_diff::DocumentDiffBuilder* diffBuilder) {
    BSONObjIterator preItr(pre);
    BSONObjIterator postItr(post);
    const size_t postObjSize = static_cast<size_t>(post.objsize());
    std::set<StringData> deletes;
    while (preItr.more() && postItr.more()) {
        auto preVal = *preItr;
        auto postVal = *postItr;

        // Bailout if the generated diff so far is larger than the 'post' object.
        if (postObjSize < diffBuilder->getObjSize()) {
            return false;
        }
        if (preVal.fieldNameStringData() == postVal.fieldNameStringData()) {
            if (preVal.binaryEqual(postVal)) {
                // They're identical. Move on.
            } else if (preVal.type() == postVal.type() &&
                       (preVal.type() == BSONType::Object || preVal.type() == BSONType::Array)) {
                // Both are either arrays or objects, recursively compute the diff of the respective
                // array or object.
                calculateSubDiffHelper(preVal, postVal, preVal.fieldNameStringData(), diffBuilder);
            } else {
                // Any other case, just replace with the 'postVal'.
                diffBuilder->addUpdate((*preItr).fieldNameStringData(), postVal);
            }
            preItr.next();
            postItr.next();
        } else {
            // If the 'preVal' field name does not exist in the 'post' object then, just remove it.
            // If it present, we do nothing for now, since the field gets inserted later.
            deletes.insert(preVal.fieldNameStringData());
            preItr.next();
        }
    }

    // When we reach here, only one of postItr or preItr can have more fields. Record remaining
    // fields in preItr as removals.
    for (; preItr.more(); preItr.next()) {
        // Note that we don't need to record these into the 'deletes' set because there are no more
        // fields in the post image.
        invariant(!postItr.more());
        diffBuilder->addDelete((*preItr).fieldNameStringData());
    }

    // Record remaining fields in postItr as creates.
    for (; postItr.more(); postItr.next()) {
        auto fieldName = (*postItr).fieldNameStringData();
        diffBuilder->addInsert(fieldName, *postItr);
        deletes.erase(fieldName);
    }
    for (auto&& deleteField : deletes) {
        diffBuilder->addDelete(deleteField);
    }
    return postObjSize > diffBuilder->getObjSize();
}

template <class DiffBuilder, class T>
void calculateSubDiffHelper(const BSONElement& preVal,
                            const BSONElement& postVal,
                            T fieldIdentifier,
                            DiffBuilder* builder) {
    if (preVal.type() == BSONType::Object) {
        auto subDiffBuilderGuard = builder->startSubObjDiff(fieldIdentifier);
        const auto hasSubDiff = computeDocDiff(
            preVal.embeddedObject(), postVal.embeddedObject(), subDiffBuilderGuard.builder());
        if (!hasSubDiff) {
            subDiffBuilderGuard.abandon();
            builder->addUpdate(fieldIdentifier, postVal);
        }
    } else {
        auto subDiffBuilderGuard = builder->startSubArrDiff(fieldIdentifier);
        const auto hasSubDiff = computeArrayDiff(
            preVal.embeddedObject(), postVal.embeddedObject(), subDiffBuilderGuard.builder());
        if (!hasSubDiff) {
            subDiffBuilderGuard.abandon();
            builder->addUpdate(fieldIdentifier, postVal);
        }
    }
}
}  // namespace

boost::optional<doc_diff::Diff> computeDiff(const BSONObj& pre,
                                            const BSONObj& post,
                                            size_t padding) {
    doc_diff::DocumentDiffBuilder diffBuilder(padding);
    if (computeDocDiff(pre, post, &diffBuilder)) {
        auto diff = diffBuilder.serialize();
        if (diff.objsize() < post.objsize()) {
            return diff;
        }
    }
    return {};
}
}  // namespace mongo::doc_diff
