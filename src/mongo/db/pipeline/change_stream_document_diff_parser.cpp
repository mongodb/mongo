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
using DeltaUpdateDescription = change_stream_document_diff_parser::DeltaUpdateDescription;
using FieldNameOrArrayIndex = stdx::variant<StringData, size_t>;

/**
 * DeltaUpdateDescriptionBuilder is responsible both for tracking the current path as we traverse
 * the diff, and for populating a DeltaUpdateDescription reflecting the contents of that diff.
 */
struct DeltaUpdateDescriptionBuilder {
    // Adds the specified entry to the 'updateFields' document in the DeltaUpdateDescription.
    void addToUpdatedFields(FieldNameOrArrayIndex terminalField, Value updatedValue) {
        DeltaUpdateDescriptionBuilder::TempAppendToPath tmpAppend(*this, terminalField);
        _updatedFields.addField(_fieldRef.dottedField(), updatedValue);
        _addToDisambiguatedPathsIfRequired();
    }

    // Adds the specified entry to the 'removedFields' vector in the DeltaUpdateDescription.
    void addToRemovedFields(StringData terminalFieldName) {
        DeltaUpdateDescriptionBuilder::TempAppendToPath tmpAppend(*this, terminalFieldName);
        _updateDesc.removedFields.push_back(Value(_fieldRef.dottedField()));
        _addToDisambiguatedPathsIfRequired();
    }

    // Adds the current path to the 'truncatedArrays' vector in the DeltaUpdateDescription.
    void addToTruncatedArrays(int newSize) {
        _updateDesc.truncatedArrays.push_back(
            Value(Document{{"field", _fieldRef.dottedField()}, {"newSize", newSize}}));
        _addToDisambiguatedPathsIfRequired();
    }

    // Called once the diff traversal is complete. Freezes and returns the DeltaUpdateDescription.
    // It is an error to use the DeltaUpdateDescriptionBuilder again after this method is called.
    DeltaUpdateDescription&& freezeDeltaUpdateDescription() {
        _updateDesc.updatedFields = _updatedFields.freeze();
        _updateDesc.disambiguatedPaths = _disambiguatedPaths.freeze();
        return std::move(_updateDesc);
    }

    // Returns the last field in the current path.
    StringData lastPart() const {
        return _fieldRef.getPart(_fieldRef.numParts() - 1);
    }

    // Returns the number of fields in the current path.
    FieldIndex numParts() const {
        return _fieldRef.numParts();
    }

    // A structure used to add a scope-guarded field to the current path maintained by the builder.
    // When this object goes out of scope, it will automatically remove the field from the path.
    struct TempAppendToPath {
        TempAppendToPath(DeltaUpdateDescriptionBuilder& builder, FieldNameOrArrayIndex field)
            : _builder(builder) {
            // Append the specified field to the builder's path.
            _builder._appendFieldToPath(std::move(field));
        }

        ~TempAppendToPath() {
            // Remove the last field from the path when we go out of scope.
            _builder._removeLastFieldfromPath();
        }

    private:
        DeltaUpdateDescriptionBuilder& _builder;
    };

private:
    // A structure for tracking path ambiguity information. Maps 1:1 to fields in the FieldRef via
    // the _pathAmbiguity list. The 'pathIsAmbiguous' bool indicates whether the path as a whole is
    // ambiguous as of the corresponding field. Once a path is marked as ambiguous, all subsequent
    // entries must also be marked as ambiguous.
    struct AmbiguityInfo {
        bool pathIsAmbiguous = false;
        BSONType fieldType = BSONType::String;
    };

    // Append the given field to the path, and update the path ambiguity information accordingly.
    void _appendFieldToPath(FieldNameOrArrayIndex field) {
        // Resolve the FieldNameOrArrayIndex to one or the other, and append it to the path.
        const bool isArrayIndex = stdx::holds_alternative<size_t>(field);
        _fieldRef.appendPart(isArrayIndex ? std::to_string(stdx::get<size_t>(field))
                                          : stdx::get<StringData>(field));

        // Once a path has become ambiguous, it will remain so as new fields are added. If the final
        // path component is marked ambiguous, retain that value and add the type of the new field.
        const auto fieldType = (isArrayIndex ? BSONType::NumberInt : BSONType::String);
        if (!_pathAmbiguity.empty() && _pathAmbiguity.back().pathIsAmbiguous) {
            _pathAmbiguity.push_back({true /* pathIsAmbiguous */, fieldType});
            return;
        }
        // If the field is a numeric string or contains an embedded dot, it's ambiguous. We record
        // array indices so that we can reconstruct the path, but the presence of an array index is
        // not itself sufficient to make the path ambiguous. We don't include numeric fields at the
        // start of the path because those are unambiguous.
        const bool isNumeric = (!isArrayIndex && _fieldRef.numParts() > 1 &&
                                FieldRef::isNumericPathComponentStrict(lastPart()));
        const bool isDotted =
            (!isArrayIndex && !isNumeric && lastPart().find('.') != std::string::npos);

        // Add to the field list, marking the path as ambiguous if this field is dotted or numeric.
        _pathAmbiguity.push_back({(isNumeric || isDotted), fieldType});
    }

    // Remove the last field from the path, along with its entry in the ambiguity list.
    void _removeLastFieldfromPath() {
        _fieldRef.removeLastPart();
        _pathAmbiguity.pop_back();
    }

    // If this path is marked as ambiguous, add a new entry for it to 'disambiguatedPaths'.
    void _addToDisambiguatedPathsIfRequired() {
        // The final entry in _pathAmbiguity will always be marked as ambiguous if any field in the
        // path is ambiguous. If so, iterate over the list and create a vector of individual fields.
        if (!_pathAmbiguity.empty() && _pathAmbiguity.back().pathIsAmbiguous) {
            std::vector<Value> disambiguatedPath;
            FieldIndex fieldNum = 0;
            for (const auto& fieldInfo : _pathAmbiguity) {
                auto fieldVal = _fieldRef.getPart(fieldNum++);
                disambiguatedPath.push_back(fieldInfo.fieldType == BSONType::NumberInt
                                                ? Value(std::stoi(fieldVal.toString()))
                                                : Value(fieldVal));
            }
            // Add the vector of individual fields into the 'disambiguatedPaths' document. The name
            // of the field matches the entry in updatedFields, removedFields, or truncatedArrays.
            _disambiguatedPaths.addField(_fieldRef.dottedField(),
                                         Value(std::move(disambiguatedPath)));
        }
    }

    friend struct DeltaUpdateDescriptionBuilder::TempAppendToPath;

    // Each element in the _pathAmbiguity list annotates the field at the corresponding index in the
    // _fieldRef, indicating the type of that field and whether the path is ambiguous at that point.
    std::list<AmbiguityInfo> _pathAmbiguity;
    FieldRef _fieldRef;

    DeltaUpdateDescription _updateDesc;
    MutableDocument _updatedFields;
    MutableDocument _disambiguatedPaths;
};

void buildUpdateDescriptionWithDeltaOplog(
    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> reader,
    DeltaUpdateDescriptionBuilder* builder,
    boost::optional<FieldNameOrArrayIndex> currentSubField) {

    // Append the field name associated with the current level of the diff to the path.
    boost::optional<DeltaUpdateDescriptionBuilder::TempAppendToPath> tempAppend;
    if (currentSubField) {
        tempAppend.emplace(*builder, std::move(*currentSubField));
    }

    stdx::visit(
        OverloadedVisitor{
            [&](DocumentDiffReader* reader) {
                boost::optional<BSONElement> nextMod;
                while ((nextMod = reader->nextUpdate()) || (nextMod = reader->nextInsert())) {
                    builder->addToUpdatedFields(nextMod->fieldNameStringData(), Value(*nextMod));
                }

                while (auto nextDelete = reader->nextDelete()) {
                    builder->addToRemovedFields(*nextDelete);
                }

                while (auto nextSubDiff = reader->nextSubDiff()) {
                    stdx::variant<DocumentDiffReader*, ArrayDiffReader*> nextReader;
                    stdx::visit(
                        OverloadedVisitor{[&nextReader](auto& reader) { nextReader = &reader; }},
                        nextSubDiff->second);
                    buildUpdateDescriptionWithDeltaOplog(
                        nextReader, builder, {{nextSubDiff->first}});
                }
            },

            [&](ArrayDiffReader* reader) {
                // Cannot be the root of the diff object, so 'fieldRef' should not be empty.
                tassert(6697700, "Invalid diff or parsing error", builder->numParts() > 0);

                // We don't need to add a fieldname, since we already descended into the array diff.
                if (auto newSize = reader->newSize()) {
                    builder->addToTruncatedArrays(*newSize);
                }

                for (auto nextMod = reader->next(); nextMod; nextMod = reader->next()) {
                    stdx::visit(
                        OverloadedVisitor{
                            [&](BSONElement elem) {
                                builder->addToUpdatedFields(nextMod->first, Value(elem));
                            },

                            [&](auto& nextReader) {
                                buildUpdateDescriptionWithDeltaOplog(
                                    &nextReader, builder, {{nextMod->first}});
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
    DeltaUpdateDescriptionBuilder builder;
    DocumentDiffReader docReader(diff);

    buildUpdateDescriptionWithDeltaOplog(&docReader, &builder, boost::none);

    return builder.freezeDeltaUpdateDescription();
}

}  // namespace change_stream_document_diff_parser
}  // namespace mongo
