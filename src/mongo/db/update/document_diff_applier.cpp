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

#include "mongo/db/update/document_diff_applier.h"

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
};

DocumentDiffTables buildObjDiffTables(DocumentDiffReader* reader) {
    DocumentDiffTables out;

    boost::optional<StringData> optFieldName;
    while ((optFieldName = reader->nextDelete())) {
        out.safeInsert(*optFieldName, Delete{});
    }

    boost::optional<BSONElement> nextUpdate;
    while ((nextUpdate = reader->nextUpdate())) {
        out.safeInsert(nextUpdate->fieldNameStringData(), Update{*nextUpdate});
        out.fieldsToInsert.push_back(*nextUpdate);
    }

    boost::optional<BSONElement> nextInsert;
    while ((nextInsert = reader->nextInsert())) {
        out.safeInsert(nextInsert->fieldNameStringData(), Insert{*nextInsert});
        out.fieldsToInsert.push_back(*nextInsert);
    }

    for (auto next = reader->nextSubDiff(); next; next = reader->nextSubDiff()) {
        out.safeInsert(next->first, SubDiff{next->second});
    }
    return out;
}

// Mutually recursive with applyDiffToObject().
void applyDiffToArray(const BSONObj& preImage, ArrayDiffReader* reader, BSONArrayBuilder* builder);

void applyDiffToObject(const BSONObj& preImage,
                       DocumentDiffReader* reader,
                       BSONObjBuilder* builder) {
    // First build some tables so we can quickly apply the diff. We shouldn't need to examine the
    // diff again once this is done.
    const DocumentDiffTables tables = buildObjDiffTables(reader);

    // Keep track of what fields we already appended, so that we can insert the rest at the end.
    StringDataSet fieldsToSkipInserting;

    for (auto&& elt : preImage) {
        auto it = tables.fieldMap.find(elt.fieldNameStringData());
        if (it == tables.fieldMap.end()) {
            // Field is not modified, so we append it as is.
            builder->append(elt);
            continue;
        }

        stdx::visit(
            visit_helper::Overloaded{
                [](Delete) {
                    // Do nothing.
                },

                [&builder, &elt, &fieldsToSkipInserting](const Update& update) {
                    builder->append(update.newElt);
                    fieldsToSkipInserting.insert(elt.fieldNameStringData());
                },

                [](const Insert&) {
                    // Skip the pre-image version of the field. We'll add it at the end.
                },

                [&builder, &elt](const SubDiff& subDiff) {
                    const auto type = subDiff.type();
                    if (elt.type() == BSONType::Object && type == DiffType::kDocument) {
                        BSONObjBuilder subBob(builder->subobjStart(elt.fieldNameStringData()));
                        auto reader = stdx::get<DocumentDiffReader>(subDiff.reader);
                        applyDiffToObject(elt.embeddedObject(), &reader, &subBob);
                    } else if (elt.type() == BSONType::Array && type == DiffType::kArray) {
                        BSONArrayBuilder subBob(builder->subarrayStart(elt.fieldNameStringData()));
                        auto reader = stdx::get<ArrayDiffReader>(subDiff.reader);
                        applyDiffToArray(elt.embeddedObject(), &reader, &subBob);
                    } else {
                        // There's a type mismatch. The diff was expecting one type but the pre
                        // image contains a value of a different type. This means we are
                        // re-applying a diff.

                        // There must be some future operation which changed the type of this field
                        // from object/array to something else. So we set this field to null field
                        // and expect the future value to overwrite the value here.

                        builder->appendNull(elt.fieldNameStringData());
                    }

                    // Note: There's no need to update 'fieldsToSkipInserting' here, because a
                    // field cannot appear in both the sub-diff and insert section.
                },
            },
            it->second);
    }

    // Insert remaining fields to the end.
    for (auto&& elt : tables.fieldsToInsert) {
        if (!fieldsToSkipInserting.count(elt.fieldNameStringData())) {
            builder->append(elt);
        }
    }
}

/**
 * Given an (optional) member of the pre image array and a modification, apply the modification and
 * add it to the post image array in 'builder'.
 */
void appendNewValueForIndex(boost::optional<BSONElement> preImageValue,
                            const ArrayDiffReader::ArrayModification& modification,
                            BSONArrayBuilder* builder) {
    stdx::visit(
        visit_helper::Overloaded{
            [builder](const BSONElement& update) { builder->append(update); },
            [builder, &preImageValue](auto reader) {
                if (!preImageValue) {
                    // The pre-image's array was shorter than we expected. This means some
                    // future oplog entry will either re-write the value of this array index
                    // (or some parent) so we append a null and move on.
                    builder->appendNull();
                    return;
                }

                if constexpr (std::is_same_v<decltype(reader), ArrayDiffReader>) {
                    if (preImageValue->type() == BSONType::Array) {
                        BSONArrayBuilder sub(builder->subarrayStart());
                        applyDiffToArray(preImageValue->embeddedObject(), &reader, &sub);
                        return;
                    }
                } else if constexpr (std::is_same_v<decltype(reader), DocumentDiffReader>) {
                    if (preImageValue->type() == BSONType::Object) {
                        BSONObjBuilder sub(builder->subobjStart());
                        applyDiffToObject(preImageValue->embeddedObject(), &reader, &sub);
                        return;
                    }
                }

                // The type does not match what we expected. This means some future oplog
                // entry will either re-write the value of this array index (or some
                // parent) so we append a null and move on.
                builder->appendNull();
            },
        },
        modification);
}

void applyDiffToArray(const BSONObj& arrayPreImage,
                      ArrayDiffReader* reader,
                      BSONArrayBuilder* builder) {
    const auto resizeVal = reader->newSize();
    // Each modification is an optional pair where the first component is the array index and the
    // second is the modification type.
    auto nextMod = reader->next();
    BSONObjIterator preImageIt(arrayPreImage);

    size_t idx = 0;
    for (; preImageIt.more() && (!resizeVal || idx < *resizeVal); ++idx, ++preImageIt) {
        if (nextMod && idx == nextMod->first) {
            appendNewValueForIndex(*preImageIt, nextMod->second, builder);
            nextMod = reader->next();
        } else {
            // This index is not in the diff so we keep the value in the pre image.
            builder->append(*preImageIt);
        }
    }

    // Deal with remaining fields in the diff if the pre image was too short.
    for (; (resizeVal && idx < *resizeVal) || nextMod; ++idx) {
        if (nextMod && idx == nextMod->first) {
            appendNewValueForIndex(boost::none, nextMod->second, builder);
            nextMod = reader->next();
        } else {
            // This field is not mentioned in the diff so we pad the post image with null.
            builder->appendNull();
        }
    }

    invariant(!resizeVal || *resizeVal == idx);
}
}  // namespace

BSONObj applyDiff(const BSONObj& pre, const Diff& diff) {
    DocumentDiffReader reader(diff);
    BSONObjBuilder out;
    applyDiffToObject(pre, &reader, &out);
    return out.obj();
}
}  // namespace mongo::doc_diff
