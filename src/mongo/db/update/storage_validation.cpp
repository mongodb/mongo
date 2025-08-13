/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/update/storage_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/query/dbref.h"
#include "mongo/db/query/util/validate_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace storage_validation {

namespace {

const StringData idFieldName = "_id"_sd;

void scanDocumentChildren(mutablebson::ConstElement elem,
                          const bool deep,
                          std::uint32_t recursionLevel,
                          const bool allowTopLevelDollarPrefixes,
                          const bool shouldValidate,
                          const bool isEmbeddedInIdField,
                          bool* containsDotsAndDollarsField) {
    if (!elem.hasChildren()) {
        return;
    }

    auto curr = elem.leftChild();
    while (curr.ok()) {
        scanDocument(curr,
                     deep,
                     recursionLevel + 1,
                     allowTopLevelDollarPrefixes,
                     shouldValidate,
                     isEmbeddedInIdField,
                     containsDotsAndDollarsField);
        curr = curr.rightSibling();
    }
}

/**
 * Validates an element that has a field name which starts with a dollar sign ($).
 * In the case of a DBRef field ($id, $ref, [$db]) these fields may be valid in
 * the correct order/context only.
 */
void validateDollarPrefixElement(mutablebson::ConstElement elem) {
    auto curr = elem;
    auto currName = elem.getFieldName();

    // Found a $db field.
    if (currName == dbref::kDbFieldName) {
        uassert(ErrorCodes::InvalidDBRef,
                str::stream() << "The DBRef $db field must be a String, not a "
                              << typeName(curr.getType()),
                curr.getType() == BSONType::string);
        curr = curr.leftSibling();

        uassert(ErrorCodes::InvalidDBRef,
                "Found $db field without a $id before it, which is invalid.",
                curr.ok() && (curr.getFieldName() == "$id"));

        currName = curr.getFieldName();
    }

    // Found a $id field.
    if (currName == dbref::kIdFieldName) {
        curr = curr.leftSibling();
        uassert(ErrorCodes::InvalidDBRef,
                "Found $id field without a $ref before it, which is invalid.",
                curr.ok() && (curr.getFieldName() == "$ref"));

        currName = curr.getFieldName();
    }

    // Found a $ref field.
    if (currName == dbref::kRefFieldName) {
        uassert(ErrorCodes::InvalidDBRef,
                str::stream() << "The DBRef $ref field must be a String, not a "
                              << typeName(curr.getType()),
                curr.getType() == BSONType::string);

        uassert(ErrorCodes::InvalidDBRef,
                "The DBRef $ref field must be followed by a $id field",
                curr.rightSibling().ok() && curr.rightSibling().getFieldName() == "$id");
    } else {
        // Not an okay, $ prefixed field name.
        uasserted(ErrorCodes::DollarPrefixedFieldName,
                  str::stream() << "The dollar ($) prefixed field '" << elem.getFieldName()
                                << "' in '" << mutablebson::getFullName(elem)
                                << "' is not allowed in the context of an update's replacement"
                                   " document. Consider using an aggregation pipeline with"
                                   " $replaceWith.");
    }
}
}  // namespace

void scanDocument(const mutablebson::Document& doc,
                  const bool allowTopLevelDollarPrefixes,
                  const bool shouldValidate,
                  bool* containsDotsAndDollarsField) {
    bool hasId = false;
    auto currElem = doc.root().leftChild();
    while (currElem.ok()) {
        if (currElem.getFieldName() == idFieldName && shouldValidate) {
            if (currElem.getType() == BSONType::object) {
                // We need to recursively validate the _id field while ensuring we disallow
                // top-level $-prefix fields in the _id object.
                scanDocument(currElem,
                             true /* deep */,
                             0 /* recursionLevel - forces _id fields to be treated as top-level. */,
                             false /* Top-level _id fields cannot be $-prefixed. */,
                             shouldValidate,
                             true /* Indicates the element is embedded inside an _id field. */,
                             containsDotsAndDollarsField);
            } else {
                uassertStatusOK(validIdField(currElem.getValue()));
            }
            uassert(ErrorCodes::BadValue, "Can't have multiple _id fields in one document", !hasId);
            hasId = true;
        } else {
            // Validate this child element.
            const auto deep = true;
            const uint32_t recursionLevel = 1;
            scanDocument(currElem,
                         deep,
                         recursionLevel,
                         allowTopLevelDollarPrefixes,
                         shouldValidate,
                         false /* Not embedded inside an _id field. */,
                         containsDotsAndDollarsField);
        }

        currElem = currElem.rightSibling();
    }
}

void scanDocument(mutablebson::ConstElement elem,
                  const bool deep,
                  std::uint32_t recursionLevel,
                  const bool allowTopLevelDollarPrefixes,
                  const bool shouldValidate,
                  const bool isEmbeddedInIdField,
                  bool* containsDotsAndDollarsField) {
    if (shouldValidate) {
        uassert(ErrorCodes::BadValue, "Invalid elements cannot be stored.", elem.ok());

        uassert(ErrorCodes::Overflow,
                str::stream() << "Document exceeds maximum nesting depth of "
                              << BSONDepth::getMaxDepthForUserStorage(),
                recursionLevel <= BSONDepth::getMaxDepthForUserStorage());
    }

    // Field names of elements inside arrays are not meaningful in mutable bson,
    // so we do not want to validate them.
    const mutablebson::ConstElement& parent = elem.parent();
    const bool childOfArray = parent.ok() ? (parent.getType() == BSONType::array) : false;

    // Only check top-level fields if 'allowTopLevelDollarPrefixes' is false, and don't validate any
    // fields for '$'-prefixes if 'allowTopLevelDollarPrefixes' is true. If 'isEmbeddedInIdField' is
    // true, check for '$'-prefixes at all the levels.
    const bool checkTopLevelFields =
        !allowTopLevelDollarPrefixes && (recursionLevel == 1 || isEmbeddedInIdField);

    auto fieldName = elem.getFieldName();
    if (fieldName.starts_with('$')) {
        if (containsDotsAndDollarsField) {
            *containsDotsAndDollarsField = true;
            // If we are not validating for storage, return once a $-prefixed field is found.
            if (!shouldValidate)
                return;
        }
        if (!childOfArray && checkTopLevelFields && shouldValidate) {
            // Cannot start with "$", unless dbref.
            validateDollarPrefixElement(elem);
        }
    }

    if (deep) {

        // Check children if there are any.
        scanDocumentChildren(elem,
                             deep,
                             recursionLevel,
                             allowTopLevelDollarPrefixes,
                             shouldValidate,
                             isEmbeddedInIdField,
                             containsDotsAndDollarsField);
    }
}

}  // namespace storage_validation
}  // namespace mongo
