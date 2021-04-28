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
#include "mongo/util/string_map.h"
#include "mongo/util/visit_helper.h"

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

    // Map from field name to modification for that field.
    StringDataMap<FieldModification> fieldMap;

    // Order in which new fields should be added to the pre image.
    std::vector<BSONElement> fieldsToInsert;
    std::size_t sizeOfFieldsToInsert = 0;
    // Diff only inserts fields, no deletes or updates
    bool insertOnly = false;
};

DocumentDiffTables buildObjDiffTables(DocumentDiffReader* reader) {
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

    boost::optional<BSONElement> nextInsert;
    while ((nextInsert = reader->nextInsert())) {
        out.safeInsert(nextInsert->fieldNameStringData(), Insert{*nextInsert});
        out.fieldsToInsert.push_back(*nextInsert);
        out.sizeOfFieldsToInsert += out.fieldsToInsert.back().size();
    }

    for (auto next = reader->nextSubDiff(); next; next = reader->nextSubDiff()) {
        out.safeInsert(next->first, SubDiff{next->second});
        out.insertOnly = false;
    }
    return out;
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
        const DocumentDiffTables tables = buildObjDiffTables(reader);

        if (!_mustCheckExistenceForInsertOperations && tables.insertOnly) {
            builder->bb().reserveBytes(preImage.objsize() + tables.sizeOfFieldsToInsert);
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
                visit_helper::Overloaded{
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
            visit_helper::Overloaded{
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
    applier.applyDiffToObject(pre, &path, &reader, &out);
    return {out.obj(), applier.indexesAffected()};
}
}  // namespace mongo::doc_diff
