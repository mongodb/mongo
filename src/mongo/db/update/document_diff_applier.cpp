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

#include "mongo/db/field_ref.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update_index_data.h"

#include "mongo/stdx/variant.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/string_map.h"

namespace mongo::doc_diff {
namespace {

struct Update {
    BSONElement newElt;
};
struct Insert {
    BSONElement newElt;
};
struct Delete {};
struct SubDiff {
    DiffType type() const {
        return stdx::holds_alternative<DocumentDiffReader>(reader) ? DiffType::kDocument
                                                                   : DiffType::kArray;
    }

    stdx::variant<DocumentDiffReader, ArrayDiffReader> reader;
};

// This struct stores the tables we build from an object diff before applying it.
struct DocumentDiffTables {
    // Types of modifications that can be done to a field.
    using FieldModification = stdx::variant<Delete, Update, Insert, SubDiff>;

    /**
     * Inserts to the table and throws if the key exists already, which would mean that the
     * diff is invalid.
     */
    void safeInsert(StringData fieldName, FieldModification mod) {
        auto [it, inserted] = fieldMap.insert({fieldName, std::move(mod)});
        uassert(4728000, str::stream() << "duplicate field name in diff: " << fieldName, inserted);
    }

    // Map from field name to modification for that field. Not populated for insert only diffs.
    StringDataMap<FieldModification> fieldMap;

    // Order in which new fields should be added to the pre image.
    std::vector<BSONElement> fieldsToInsert;
    // Diff only inserts fields, no deletes or updates
    bool insertOnly = false;
};

DocumentDiffTables buildObjDiffTables(DocumentDiffReader* reader,
                                      bool mustCheckExistenceForInsertOperations) {
    DocumentDiffTables out;
    out.insertOnly = true;

    boost::optional<StringData> optFieldName;
    while ((optFieldName = reader->nextDelete())) {
        out.safeInsert(*optFieldName, Delete{});
        out.insertOnly = false;
    }

    boost::optional<BSONElement> nextUpdate;
    while ((nextUpdate = reader->nextUpdate())) {
        out.safeInsert(nextUpdate->fieldNameStringData(), Update{*nextUpdate});
        out.fieldsToInsert.push_back(*nextUpdate);
        out.insertOnly = false;
    }

    for (auto next = reader->nextSubDiff(); next; next = reader->nextSubDiff()) {
        out.safeInsert(next->first, SubDiff{next->second});
        out.insertOnly = false;
    }

    boost::optional<BSONElement> nextInsert;
    while ((nextInsert = reader->nextInsert())) {
        if (mustCheckExistenceForInsertOperations || !out.insertOnly) {
            out.safeInsert(nextInsert->fieldNameStringData(), Insert{*nextInsert});
        }

        out.fieldsToInsert.push_back(*nextInsert);
    }
    return out;
}

int32_t computeDamageOnArray(const BSONObj& preImageRoot,
                             const BSONObj& arrayPreImage,
                             ArrayDiffReader* reader,
                             mutablebson::DamageVector* damages,
                             BufBuilder* bufBuilder,
                             size_t offsetRoot,
                             bool mustCheckExistenceForInsertOperations);

void appendTypeByte(BufBuilder* bufBuilder, char typeByte) {
    bufBuilder->appendChar(typeByte);
}

size_t targetOffsetInPostImage(const char* cur,
                               const char* start,
                               size_t globalOffset,
                               size_t localOffset = 0) {
    return cur - start + globalOffset + localOffset;
}

// Merges the new damage with the previous one if possible, otherwise adds a new one.
void appendDamage(mutablebson::DamageVector* damages,
                  size_t sourceOffset,
                  size_t sourceSize,
                  size_t targetOffset,
                  size_t targetSize) {
    if (!damages->empty()) {
        auto& [sourceOffsetPrev, sourceSizePrev, targetOffsetPrev, targetSizePrev] =
            damages->back();
        if (targetOffsetPrev + sourceSizePrev == targetOffset) {
            if (!sourceSizePrev) {
                // If the previous damage is a delete, updates with the new sourceOffset.
                sourceOffsetPrev = sourceOffset;
            }
            sourceSizePrev += sourceSize;
            targetSizePrev += targetSize;
            return;
        }
    }
    damages->emplace_back(sourceOffset, sourceSize, targetOffset, targetSize);
}

void appendEOOByte(mutablebson::DamageVector* damages,
                   BufBuilder* bufBuilder,
                   size_t targetOffset) {
    auto lastDamage = damages->back();
    if (lastDamage.targetOffset + lastDamage.sourceSize == targetOffset) {
        // Appends EOO byte to help with potential merging.
        appendDamage(damages, bufBuilder->len(), 1, targetOffset, 1);
        appendTypeByte(bufBuilder, BSONType::EOO);
    }
}

void addElementPrefix(const BSONElement& elt,
                      mutablebson::DamageVector* damages,
                      BufBuilder* bufBuilder,
                      size_t targetOffset) {
    auto prev = damages->back();
    if (prev.targetOffset + prev.sourceSize == targetOffset) {
        // Appends the fieldname of the sub-document for potential merging.
        auto prefixSize = elt.embeddedObject().objdata() - elt.rawdata();
        appendDamage(damages, bufBuilder->len(), prefixSize, targetOffset, prefixSize);
        bufBuilder->appendBuf(elt.rawdata(), prefixSize);
    }
}

// Computes the damage vector and constructs the damage source from doc diffs.
// The 'preImageRoot' is kept to calculate the offset of current (sub)document 'preImageSub' in the
// root document. The diff for the current (sub)document is stored in the 'reader'. The 'damages'
// and 'bufBuilder' are passed down to the recursive calls to build the damage vector and damage
// source. The 'offsetRoot' is an extra offset in the root document to account for bytes changed by
// updates made already.
int32_t computeDamageOnObject(const BSONObj& preImageRoot,
                              const BSONObj& preImageSub,
                              DocumentDiffReader* reader,
                              mutablebson::DamageVector* damages,
                              BufBuilder* bufBuilder,
                              size_t offsetRoot,
                              bool mustCheckExistenceForInsertOperations) {
    const DocumentDiffTables tables =
        buildObjDiffTables(reader, mustCheckExistenceForInsertOperations);
    int32_t diffSize = 0;
    // Reserves four bytes for the total size. Stores the offset instead of the actual pointer in
    // case the buffer grows and invalidates the pointer. Will update the value at the end.
    size_t sizeBytesPos = bufBuilder->len();
    bufBuilder->skip(4);
    // The start of 'preImageSub' with the offset from the updates made already.
    auto sizeBytesOffset =
        targetOffsetInPostImage(preImageSub.objdata(), preImageRoot.objdata(), offsetRoot);
    appendDamage(damages, sizeBytesPos, 4, sizeBytesOffset, 4);

    if (!mustCheckExistenceForInsertOperations && tables.insertOnly) {
        for (auto&& elt : tables.fieldsToInsert) {
            // The end of 'preImageSub' with the offset from the updates made already.
            auto targetOffset = targetOffsetInPostImage(
                preImageSub.end()->rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
            // Inserts the field to the end.
            appendDamage(damages, bufBuilder->len(), elt.size(), targetOffset, 0);
            diffSize += elt.size();
            bufBuilder->appendBuf(elt.rawdata(), elt.size());
        }

        // Updates the bytes of total size.
        DataView(bufBuilder->buf() + sizeBytesPos)
            .write(tagLittleEndian(preImageSub.objsize() + diffSize));

        // The end of 'preImageSub' with the offset from the updates made already.
        auto targetOffset = targetOffsetInPostImage(
            preImageSub.end()->rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
        // Appends EOO byte to help with potential merging.
        appendDamage(damages, bufBuilder->len(), 1, targetOffset, 1);
        appendTypeByte(bufBuilder, BSONType::EOO);
        return diffSize;
    }

    // Keeps track of what fields we already appended, so that we can insert the rest at the end.
    StringDataSet fieldsToSkipInserting;

    for (auto&& elt : preImageSub) {
        auto it = tables.fieldMap.find(elt.fieldNameStringData());
        // The start of 'elt' with the offset from the updates made already.
        auto targetOffset =
            targetOffsetInPostImage(elt.rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
        if (it == tables.fieldMap.end()) {
            // Field is not modified.
            continue;
        }

        stdx::visit(
            OverloadedVisitor{
                [&](Delete) {
                    appendDamage(damages, 0, 0, targetOffset, elt.size());
                    diffSize -= elt.size();
                },

                [&](const Update& update) {
                    auto newElt = update.newElt;
                    // Updates with the new BSONElement.
                    appendDamage(
                        damages, bufBuilder->len(), newElt.size(), targetOffset, elt.size());
                    diffSize += newElt.size() - elt.size();
                    bufBuilder->appendBuf(newElt.rawdata(), newElt.size());
                    fieldsToSkipInserting.insert(elt.fieldNameStringData());
                },

                [&](const Insert&) {
                    // Deletes the pre-image version of the field. We'll add it at the end.
                    appendDamage(damages, 0, 0, targetOffset, elt.size());
                    diffSize -= elt.size();
                },

                [&](const SubDiff& subDiff) {
                    const auto type = subDiff.type();
                    if (elt.type() == BSONType::Object && type == DiffType::kDocument) {
                        addElementPrefix(elt, damages, bufBuilder, targetOffset);
                        auto reader = stdx::get<DocumentDiffReader>(subDiff.reader);
                        diffSize += computeDamageOnObject(preImageRoot,
                                                          elt.embeddedObject(),
                                                          &reader,
                                                          damages,
                                                          bufBuilder,
                                                          offsetRoot + diffSize,
                                                          mustCheckExistenceForInsertOperations);
                    } else if (elt.type() == BSONType::Array && type == DiffType::kArray) {
                        addElementPrefix(elt, damages, bufBuilder, targetOffset);
                        auto reader = stdx::get<ArrayDiffReader>(subDiff.reader);
                        diffSize += computeDamageOnArray(preImageRoot,
                                                         elt.embeddedObject(),
                                                         &reader,
                                                         damages,
                                                         bufBuilder,
                                                         offsetRoot + diffSize,
                                                         mustCheckExistenceForInsertOperations);
                    }
                }},
            it->second);
    }

    for (auto&& elt : tables.fieldsToInsert) {
        if (!fieldsToSkipInserting.count(elt.fieldNameStringData())) {
            // The end of 'preImageSub' with the offset from the updates made already.
            auto targetOffset = targetOffsetInPostImage(
                preImageSub.end()->rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
            // Inserts the field to the end.
            appendDamage(damages, bufBuilder->len(), elt.size(), targetOffset, 0);
            diffSize += elt.size();
            bufBuilder->appendBuf(elt.rawdata(), elt.size());
        }
    }

    // Updates the bytes of total size.
    DataView(bufBuilder->buf() + sizeBytesPos)
        .write(tagLittleEndian(preImageSub.objsize() + diffSize));

    // The end of 'preImageSub' with the offset from the updates made already.
    auto targetOffset = targetOffsetInPostImage(
        preImageSub.end()->rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
    appendEOOByte(damages, bufBuilder, targetOffset);
    return diffSize;
}

int32_t computeDamageForArrayIndex(const BSONObj& preImageRoot,
                                   const BSONObj& arrayPreImage,
                                   boost::optional<BSONElement> preImageValue,
                                   const ArrayDiffReader::ArrayModification& modification,
                                   mutablebson::DamageVector* damages,
                                   BufBuilder* bufBuilder,
                                   size_t offsetRoot,
                                   bool mustCheckExistenceForInsertOperations) {
    int32_t diffSize = 0;
    stdx::visit(
        OverloadedVisitor{
            [&](const BSONElement& update) {
                invariant(!update.eoo());
                auto preValuePos = arrayPreImage.end()->rawdata();
                auto targetSize = 0;
                if (preImageValue) {
                    preValuePos = preImageValue->rawdata();
                    targetSize = preImageValue->size();
                }
                // The start of 'preImageValue' if existed otherwise the end of the array, with the
                // offset from the updates made already.
                auto targetOffset =
                    targetOffsetInPostImage(preValuePos, preImageRoot.objdata(), offsetRoot);
                // Updates with the new BSONElement except for the 'u' byte.
                auto sourceSize = update.size() - 1;
                appendDamage(damages, bufBuilder->len(), sourceSize, targetOffset, targetSize);
                diffSize += sourceSize - targetSize;
                auto source = update.rawdata();
                appendTypeByte(bufBuilder, *source++);
                // Skips the byte of 'u' and appends the data after the type byte.
                bufBuilder->appendBuf(source + 1, sourceSize - 1);
            },
            [&](auto reader) {
                if (preImageValue) {
                    auto targetOffset = targetOffsetInPostImage(
                        preImageValue->rawdata(), preImageRoot.objdata(), offsetRoot);
                    if constexpr (std::is_same_v<decltype(reader), ArrayDiffReader>) {
                        if (preImageValue->type() == BSONType::Array) {
                            addElementPrefix(*preImageValue, damages, bufBuilder, targetOffset);
                            diffSize += computeDamageOnArray(preImageRoot,
                                                             preImageValue->embeddedObject(),
                                                             &reader,
                                                             damages,
                                                             bufBuilder,
                                                             offsetRoot,
                                                             mustCheckExistenceForInsertOperations);
                            return;
                        }
                    } else if constexpr (std::is_same_v<decltype(reader), DocumentDiffReader>) {
                        if (preImageValue->type() == BSONType::Object) {
                            addElementPrefix(*preImageValue, damages, bufBuilder, targetOffset);
                            diffSize +=
                                computeDamageOnObject(preImageRoot,
                                                      preImageValue->embeddedObject(),
                                                      &reader,
                                                      damages,
                                                      bufBuilder,
                                                      offsetRoot,
                                                      mustCheckExistenceForInsertOperations);
                            return;
                        }
                    }
                }
            },
        },
        modification);
    return diffSize;
}

// Mutually recursive with computeDamageOnObject().
int32_t computeDamageOnArray(const BSONObj& preImageRoot,
                             const BSONObj& arrayPreImage,
                             ArrayDiffReader* reader,
                             mutablebson::DamageVector* damages,
                             BufBuilder* bufBuilder,
                             size_t offsetRoot,
                             bool mustCheckExistenceForInsertOperations) {
    int32_t diffSize = 0;
    const auto resizeVal = reader->newSize();
    // Each modification is an optional pair where the first component is the array index and
    // the second is the modification type.
    auto nextMod = reader->next();
    BSONObjIterator preImageIt(arrayPreImage);
    // Reserves four bytes for the total size. Stores the offset instead of the actual pointer in
    // case the buffer grows and invalidates the pointer. Will update the value at the end.
    size_t sizeBytesPos = bufBuilder->len();
    bufBuilder->skip(4);
    // The start of 'arrayPreImage' with the offset from the updates made already.
    auto sizeBytesOffset =
        targetOffsetInPostImage(arrayPreImage.objdata(), preImageRoot.objdata(), offsetRoot);
    appendDamage(damages, sizeBytesPos, 4, sizeBytesOffset, 4);

    size_t idx = 0;
    for (; preImageIt.more() && (!resizeVal || idx < *resizeVal); ++idx, ++preImageIt) {
        if (nextMod && idx == nextMod->first) {
            diffSize += computeDamageForArrayIndex(preImageRoot,
                                                   arrayPreImage,
                                                   *preImageIt,
                                                   nextMod->second,
                                                   damages,
                                                   bufBuilder,
                                                   offsetRoot + diffSize,
                                                   mustCheckExistenceForInsertOperations);
            nextMod = reader->next();
        }
    }

    // Removes the remaining fields in 'arrayPreImage' if resizing to a shorter array.
    if (preImageIt.more()) {
        // The element 'preImageIt' points to with the offset from the updates made already.
        auto targetOffset = targetOffsetInPostImage(
            (*preImageIt).rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
        // The size of bytes from current element to the end of the array.
        auto targetSize = arrayPreImage.end()->rawdata() - (*preImageIt).rawdata();
        // Deletes the rest of the array in the 'arrayPreImage'.
        appendDamage(damages, 0, 0, targetOffset, targetSize);
        diffSize -= targetSize;
    }

    // Deals with remaining fields in the diff if the pre image was too short.
    for (; (resizeVal && idx < *resizeVal) || nextMod; ++idx) {
        if (nextMod && idx == nextMod->first) {
            diffSize += computeDamageForArrayIndex(preImageRoot,
                                                   arrayPreImage,
                                                   boost::none,
                                                   nextMod->second,
                                                   damages,
                                                   bufBuilder,
                                                   offsetRoot + diffSize,
                                                   mustCheckExistenceForInsertOperations);
            nextMod = reader->next();
        } else {
            // This field is not mentioned in the diff so we pad the post image with null.
            auto idxAsStr = std::to_string(idx);
            // The end of 'arrayPreImage' with the offset from the updates made already.
            auto targetOffset = targetOffsetInPostImage(
                arrayPreImage.end()->rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
            // Inserts the BSON type byte, index string, and terminating null byte to the end.
            auto sourceSize = idxAsStr.size() + 2;
            appendDamage(damages, bufBuilder->len(), sourceSize, targetOffset, 0);
            diffSize += sourceSize;
            appendTypeByte(bufBuilder, BSONType::jstNULL);
            bufBuilder->appendStr(idxAsStr);
        }
    }

    invariant(!resizeVal || *resizeVal == idx);

    // Updates the bytes of total size.
    DataView(bufBuilder->buf() + sizeBytesPos)
        .write(tagLittleEndian(arrayPreImage.objsize() + diffSize));

    // The end of 'arrayPreImage' with the offset from the updates made already.
    auto targetOffset = targetOffsetInPostImage(
        arrayPreImage.end()->rawdata(), preImageRoot.objdata(), offsetRoot, diffSize);
    appendEOOByte(damages, bufBuilder, targetOffset);
    return diffSize;
}

class DiffApplier {
public:
    DiffApplier(const UpdateIndexData* indexData, bool mustCheckExistenceForInsertOperations)
        : _indexData(indexData),
          _mustCheckExistenceForInsertOperations{mustCheckExistenceForInsertOperations} {}

    void applyDiffToObject(const BSONObj& preImage,
                           FieldRef* path,
                           DocumentDiffReader* reader,
                           BSONObjBuilder* builder) {
        // First build some tables so we can quickly apply the diff. We shouldn't need to examine
        // the diff again once this is done.
        const DocumentDiffTables tables =
            buildObjDiffTables(reader, _mustCheckExistenceForInsertOperations);

        if (!_mustCheckExistenceForInsertOperations && tables.insertOnly) {
            builder->appendElements(preImage);
            for (auto&& elt : tables.fieldsToInsert) {
                builder->append(elt);
                FieldRef::FieldRefTempAppend tempAppend(*path, elt.fieldNameStringData());
                updateIndexesAffected(path);
            }
            return;
        }

        // Keep track of what fields we already appended, so that we can insert the rest at the end.
        StringDataSet fieldsToSkipInserting;

        for (auto&& elt : preImage) {
            auto it = tables.fieldMap.find(elt.fieldNameStringData());
            if (it == tables.fieldMap.end()) {
                // Field is not modified, so we append it as is.
                invariant(!elt.eoo());
                builder->append(elt);
                continue;
            }
            FieldRef::FieldRefTempAppend tempAppend(*path, elt.fieldNameStringData());

            stdx::visit(
                OverloadedVisitor{
                    [this, &path](Delete) {
                        // Do not append anything.
                        updateIndexesAffected(path);
                    },

                    [this, &path, &builder, &elt, &fieldsToSkipInserting](const Update& update) {
                        builder->append(update.newElt);
                        updateIndexesAffected(path);
                        fieldsToSkipInserting.insert(elt.fieldNameStringData());
                    },

                    [](const Insert&) {
                        // Skip the pre-image version of the field. We'll add it at the end.
                    },

                    [this, &builder, &elt, &path](const SubDiff& subDiff) {
                        const auto type = subDiff.type();
                        if (elt.type() == BSONType::Object && type == DiffType::kDocument) {
                            BSONObjBuilder subBob(builder->subobjStart(elt.fieldNameStringData()));
                            auto reader = stdx::get<DocumentDiffReader>(subDiff.reader);
                            applyDiffToObject(elt.embeddedObject(), path, &reader, &subBob);
                        } else if (elt.type() == BSONType::Array && type == DiffType::kArray) {
                            BSONArrayBuilder subBob(
                                builder->subarrayStart(elt.fieldNameStringData()));
                            auto reader = stdx::get<ArrayDiffReader>(subDiff.reader);
                            applyDiffToArray(elt.embeddedObject(), path, &reader, &subBob);
                        } else {
                            // There's a type mismatch. The diff was expecting one type but the pre
                            // image contains a value of a different type. This means we are
                            // re-applying a diff.

                            // There must be some future operation which changed the type of this
                            // field from object/array to something else. So we set this field to
                            // null and expect the future value to overwrite the value here.

                            builder->appendNull(elt.fieldNameStringData());
                            updateIndexesAffected(path);
                        }

                        // Note: There's no need to update 'fieldsToSkipInserting' here, because a
                        // field cannot appear in both the sub-diff and insert section.
                    },
                },
                it->second);
        }

        // Whether we have already determined whether indexes are affected for the base path; that
        // is, the path without any of the fields to insert below. This is useful for when multiple
        // of the fields to insert are not canonical index field components.
        bool alreadyDidUpdateIndexAffectedForBasePath = false;

        // Insert remaining fields to the end.
        for (auto&& elt : tables.fieldsToInsert) {
            if (!fieldsToSkipInserting.count(elt.fieldNameStringData())) {
                builder->append(elt);

                bool isComponentPartOfCanonicalizedIndexPath =
                    UpdateIndexData::isComponentPartOfCanonicalizedIndexPath(
                        elt.fieldNameStringData());
                // If the path is empty, then the field names are being appended at the top level.
                // This means that they cannot represent indices of an array, so the 'canonical'
                // path check does not apply.
                if (isComponentPartOfCanonicalizedIndexPath ||
                    !alreadyDidUpdateIndexAffectedForBasePath || path->empty()) {
                    FieldRef::FieldRefTempAppend tempAppend(*path, elt.fieldNameStringData());
                    updateIndexesAffected(path);

                    // If we checked whether the update affects indexes for a path where the tail
                    // element is not considered part of the 'canonicalized' path (as defined by
                    // UpdateIndexData) then we've effectively checked whether updating the base
                    // path affects indexes. This means we can skip future checks for paths that end
                    // with a component that's not considered part of the canonicalized path.
                    alreadyDidUpdateIndexAffectedForBasePath =
                        alreadyDidUpdateIndexAffectedForBasePath ||
                        !isComponentPartOfCanonicalizedIndexPath;
                }
            }
        }
    }

    bool indexesAffected() const {
        return _indexesAffected;
    }

private:
    /**
     * Given an (optional) member of the pre image array and a modification, apply the modification
     * and add it to the post image array in 'builder'.
     */
    void appendNewValueForArrayIndex(boost::optional<BSONElement> preImageValue,
                                     FieldRef* path,
                                     const ArrayDiffReader::ArrayModification& modification,
                                     BSONArrayBuilder* builder) {
        stdx::visit(
            OverloadedVisitor{
                [this, &path, builder](const BSONElement& update) {
                    invariant(!update.eoo());
                    builder->append(update);
                    updateIndexesAffected(path);
                },
                [this, builder, &preImageValue, &path](auto reader) {
                    if (!preImageValue) {
                        // The pre-image's array was shorter than we expected. This means some
                        // future oplog entry will either re-write the value of this array index
                        // (or some parent) so we append a null and move on.
                        builder->appendNull();
                        updateIndexesAffected(path);
                        return;
                    }
                    if constexpr (std::is_same_v<decltype(reader), ArrayDiffReader>) {
                        if (preImageValue->type() == BSONType::Array) {
                            BSONArrayBuilder sub(builder->subarrayStart());
                            applyDiffToArray(preImageValue->embeddedObject(), path, &reader, &sub);
                            return;
                        }
                    } else if constexpr (std::is_same_v<decltype(reader), DocumentDiffReader>) {
                        if (preImageValue->type() == BSONType::Object) {
                            BSONObjBuilder sub(builder->subobjStart());
                            applyDiffToObject(preImageValue->embeddedObject(), path, &reader, &sub);
                            return;
                        }
                    }

                    // The type does not match what we expected. This means some future oplog
                    // entry will either re-write the value of this array index (or some
                    // parent) so we append a null and move on.
                    builder->appendNull();
                    updateIndexesAffected(path);
                },
            },
            modification);
    }

    // Mutually recursive with applyDiffToObject().
    void applyDiffToArray(const BSONObj& arrayPreImage,
                          FieldRef* path,
                          ArrayDiffReader* reader,
                          BSONArrayBuilder* builder) {
        const auto resizeVal = reader->newSize();
        // Each modification is an optional pair where the first component is the array index and
        // the second is the modification type.
        auto nextMod = reader->next();
        BSONObjIterator preImageIt(arrayPreImage);

        // If there is a resize of array, check if indexes are affected by the array modification.
        if (resizeVal) {
            updateIndexesAffected(path);
        }

        size_t idx = 0;
        for (; preImageIt.more() && (!resizeVal || idx < *resizeVal); ++idx, ++preImageIt) {
            auto idxAsStr = std::to_string(idx);
            FieldRef::FieldRefTempAppend tempAppend(*path, idxAsStr);
            if (nextMod && idx == nextMod->first) {
                appendNewValueForArrayIndex(*preImageIt, path, nextMod->second, builder);
                nextMod = reader->next();
            } else {
                invariant(!(*preImageIt).eoo());
                // This index is not in the diff so we keep the value in the pre image.
                builder->append(*preImageIt);
            }
        }

        // Deal with remaining fields in the diff if the pre image was too short.
        for (; (resizeVal && idx < *resizeVal) || nextMod; ++idx) {
            auto idxAsStr = std::to_string(idx);
            FieldRef::FieldRefTempAppend tempAppend(*path, idxAsStr);
            if (nextMod && idx == nextMod->first) {
                appendNewValueForArrayIndex(boost::none, path, nextMod->second, builder);
                nextMod = reader->next();
            } else {
                // This field is not mentioned in the diff so we pad the post image with null.
                updateIndexesAffected(path);
                builder->appendNull();
            }
        }

        invariant(!resizeVal || *resizeVal == idx);
    }

    void updateIndexesAffected(FieldRef* path) {
        if (_indexData) {
            _indexesAffected = _indexesAffected || _indexData->mightBeIndexed(*path);
        }
    }

    const UpdateIndexData* _indexData;
    bool _mustCheckExistenceForInsertOperations = true;
    bool _indexesAffected = false;
};
}  // namespace

ApplyDiffOutput applyDiff(const BSONObj& pre,
                          const Diff& diff,
                          const UpdateIndexData* indexData,
                          bool mustCheckExistenceForInsertOperations) {
    DocumentDiffReader reader(diff);
    BSONObjBuilder out;
    DiffApplier applier(indexData, mustCheckExistenceForInsertOperations);
    FieldRef path;

    // Use size of pre + diff as an approximation for size needed for post object when the diff is
    // insert-only. This is an upper bound for the size needed to avoid re-allocations when applying
    // the diff.
    const auto estimatedSize = pre.objsize() + diff.objsize();
    // The interface around reserving bytes is strange. You need to "claim" the reserved bytes for
    // it to take effect.
    out.bb().reserveBytes(estimatedSize);
    out.bb().claimReservedBytes(estimatedSize);
    applier.applyDiffToObject(pre, &path, &reader, &out);
    return {out.obj(), applier.indexesAffected()};
}

DamagesOutput computeDamages(const BSONObj& pre,
                             const Diff& diff,
                             bool mustCheckExistenceForInsertOperations) {
    DocumentDiffReader reader(diff);
    mutablebson::DamageVector damages;
    BufBuilder b;
    computeDamageOnObject(
        pre, pre, &reader, &damages, &b, 0, mustCheckExistenceForInsertOperations);
    return {pre, b.release(), std::move(damages)};
}
}  // namespace mongo::doc_diff
