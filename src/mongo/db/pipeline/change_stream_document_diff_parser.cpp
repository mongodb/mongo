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

#include "mongo/db/pipeline/change_stream_document_diff_parser.h"
#include "mongo/db/field_ref.h"

namespace mongo {
using doc_diff::ArrayDiffReader;
using doc_diff::Diff;
using doc_diff::DocumentDiffReader;

namespace {
// If the terminal fieldname in the given FieldRef has an embedded dot, add it into the
// dottedFieldNames vector.
void appendIfDottedField(FieldRef* fieldRef, std::vector<Value>* dottedFieldNames) {
    auto fieldName = fieldRef->getPart(fieldRef->numParts() - 1);
    if (fieldName.find('.') != std::string::npos) {
        dottedFieldNames->push_back(Value(fieldName));
    }
}

void buildUpdateDescriptionWithDeltaOplog(
    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> reader,
    FieldRef* fieldRef,
    MutableDocument* updatedFields,
    std::vector<Value>* removedFields,
    std::vector<Value>* truncatedArrays,
    MutableDocument* arrayIndices,
    MutableDocument* dottedFields) {

    stdx::visit(
        visit_helper::Overloaded{
            [&](DocumentDiffReader* reader) {
                // Used to track dotted fieldnames at the current level of the diff.
                std::vector<Value> currentDottedFieldNames;

                boost::optional<BSONElement> nextMod;
                while ((nextMod = reader->nextUpdate()) || (nextMod = reader->nextInsert())) {
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef,
                                                           nextMod->fieldNameStringData());
                    updatedFields->addField(fieldRef->dottedField(), Value(*nextMod));
                    appendIfDottedField(fieldRef, &currentDottedFieldNames);
                }

                boost::optional<StringData> nextDelete;
                while ((nextDelete = reader->nextDelete())) {
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, *nextDelete);
                    removedFields->push_back(Value(fieldRef->dottedField()));
                    appendIfDottedField(fieldRef, &currentDottedFieldNames);
                }

                boost::optional<
                    std::pair<StringData, stdx::variant<DocumentDiffReader, ArrayDiffReader>>>
                    nextSubDiff;
                while ((nextSubDiff = reader->nextSubDiff())) {
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, nextSubDiff->first);
                    appendIfDottedField(fieldRef, &currentDottedFieldNames);

                    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> nextReader;
                    stdx::visit(visit_helper::Overloaded{[&nextReader](auto& reader) {
                                    nextReader = &reader;
                                }},
                                nextSubDiff->second);
                    buildUpdateDescriptionWithDeltaOplog(nextReader,
                                                         fieldRef,
                                                         updatedFields,
                                                         removedFields,
                                                         truncatedArrays,
                                                         arrayIndices,
                                                         dottedFields);
                }

                // Now that we have iterated through all fields at this level of the diff, add any
                // dotted fieldnames we encountered into the 'dottedFields' output document.
                if (!currentDottedFieldNames.empty()) {
                    dottedFields->addField(fieldRef->dottedField(),
                                           Value(std::move(currentDottedFieldNames)));
                }
            },

            [&](ArrayDiffReader* reader) {
                // ArrayDiffReader can not be the root of the diff object, so 'fieldRef' should not
                // be empty.
                invariant(!fieldRef->empty());

                const auto newSize = reader->newSize();
                if (newSize) {
                    const int sz = *newSize;
                    truncatedArrays->push_back(
                        Value(Document{{"field", fieldRef->dottedField()}, {"newSize", sz}}));
                }

                // Used to track the array indices at the current level of the diff.
                std::vector<Value> currentArrayIndices;
                for (auto nextMod = reader->next(); nextMod; nextMod = reader->next()) {
                    const auto& fieldName = std::to_string(nextMod->first);
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, fieldName);

                    currentArrayIndices.push_back(Value(static_cast<int>(nextMod->first)));

                    stdx::visit(
                        visit_helper::Overloaded{
                            [&](BSONElement elem) {
                                updatedFields->addField(fieldRef->dottedField(), Value(elem));
                            },

                            [&](auto& nextReader) {
                                buildUpdateDescriptionWithDeltaOplog(&nextReader,
                                                                     fieldRef,
                                                                     updatedFields,
                                                                     removedFields,
                                                                     truncatedArrays,
                                                                     arrayIndices,
                                                                     dottedFields);
                            },
                        },
                        nextMod->second);
                }

                // Now that we have iterated through all fields at this level of the diff, add all
                // the array indices we encountered into the 'arrayIndices' output document.
                if (!currentArrayIndices.empty()) {
                    arrayIndices->addField(fieldRef->dottedField(),
                                           Value(std::move(currentArrayIndices)));
                }
            },
        },
        reader);
    return;
}

}  // namespace

namespace change_stream_document_diff_parser {

DeltaUpdateDescription parseDiff(const Diff& diff) {
    DeltaUpdateDescription updatedDesc;
    MutableDocument updatedFields;
    MutableDocument dottedFields;
    MutableDocument arrayIndices;
    DocumentDiffReader docReader(diff);
    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> reader = &docReader;
    FieldRef path;
    buildUpdateDescriptionWithDeltaOplog(reader,
                                         &path,
                                         &updatedFields,
                                         &updatedDesc.removedFields,
                                         &updatedDesc.truncatedArrays,
                                         &arrayIndices,
                                         &dottedFields);
    updatedDesc.updatedFields = updatedFields.freeze();
    updatedDesc.arrayIndices = arrayIndices.freeze();
    updatedDesc.dottedFields = dottedFields.freeze();

    return updatedDesc;
}

}  // namespace change_stream_document_diff_parser
}  // namespace mongo
