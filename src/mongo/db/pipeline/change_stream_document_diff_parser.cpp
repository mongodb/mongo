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
void buildUpdateDescriptionWithDeltaOplog(
    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> reader,
    FieldRef* fieldRef,
    MutableDocument* updatedFields,
    std::vector<Value>* removedFields,
    std::vector<Value>* truncatedArrays) {

    stdx::visit(
        visit_helper::Overloaded{
            [&](DocumentDiffReader* reader) {
                boost::optional<BSONElement> nextMod;

                while ((nextMod = reader->nextUpdate()) || (nextMod = reader->nextInsert())) {
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef,
                                                           nextMod->fieldNameStringData());
                    updatedFields->addField(fieldRef->dottedField(), Value(*nextMod));
                }

                boost::optional<StringData> nextDelete;
                while ((nextDelete = reader->nextDelete())) {
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, *nextDelete);

                    removedFields->push_back(Value(fieldRef->dottedField()));
                }

                boost::optional<
                    std::pair<StringData, stdx::variant<DocumentDiffReader, ArrayDiffReader>>>
                    nextSubDiff;

                while ((nextSubDiff = reader->nextSubDiff())) {
                    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> nextReader;
                    stdx::visit(visit_helper::Overloaded{[&nextReader](auto& reader) {
                                    nextReader = &reader;
                                }},
                                nextSubDiff->second);
                    FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, nextSubDiff->first);
                    buildUpdateDescriptionWithDeltaOplog(
                        nextReader, fieldRef, updatedFields, removedFields, truncatedArrays);
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
                for (auto nextMod = reader->next(); nextMod; nextMod = reader->next()) {
                    stdx::visit(
                        visit_helper::Overloaded{
                            [&](BSONElement elem) {
                                const auto& fieldName = std::to_string(nextMod->first);
                                FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, fieldName);
                                updatedFields->addField(fieldRef->dottedField(), Value(elem));
                            },

                            [&](auto& reader) {
                                const auto& fieldName = std::to_string(nextMod->first);
                                FieldRef::FieldRefTempAppend tmpAppend(*fieldRef, fieldName);

                                buildUpdateDescriptionWithDeltaOplog(&reader,
                                                                     fieldRef,
                                                                     updatedFields,
                                                                     removedFields,
                                                                     truncatedArrays);
                            },
                        },
                        nextMod->second);
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
    DocumentDiffReader docReader(diff);
    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> reader = &docReader;
    FieldRef path;
    buildUpdateDescriptionWithDeltaOplog(
        reader, &path, &updatedFields, &updatedDesc.removedFields, &updatedDesc.truncatedArrays);
    updatedDesc.updatedFields = updatedFields.freeze();

    return updatedDesc;
}

}  // namespace change_stream_document_diff_parser
}  // namespace mongo
