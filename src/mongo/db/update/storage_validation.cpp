// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/storage_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/query/dbref.h"
#include "mongo/db/query/util/validate_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

namespace storage_validation {

namespace {
using namespace std::literals::string_view_literals;

const std::string_view idFieldName = "_id"sv;

void scanDocumentChildren(mutablebson::ConstElement elem,
                          const bool deep,
                          std::uint32_t recursionLevel,
                          const bool allowTopLevelDollarPrefixes,
                          const bool shouldValidate,
                          const bool isEmbeddedInIdField,
                          bool* containsDotsAndDollarsField,
                          const bool fromOplogApplication) {
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
                     containsDotsAndDollarsField,
                     fromOplogApplication);
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
                  bool* containsDotsAndDollarsField,
                  const bool fromOplogApplication) {
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
                             containsDotsAndDollarsField,
                             fromOplogApplication);
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
                         containsDotsAndDollarsField,
                         fromOplogApplication);
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
                  bool* containsDotsAndDollarsField,
                  const bool fromOplogApplication) {
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

    if (shouldValidate && fromOplogApplication && elem.getType() == BSONType::binData) {
        BSONElement bsonElem = elem.getValue();
        if (bsonElem.binDataType() == BinDataType::Column) {
            int len = 0;
            const char* buf = bsonElem.binData(len /*out*/);
            auto status = validateBSONColumn(buf, len);
            if (!status.isOK()) {
                uasserted(ErrorCodes::InvalidBSONColumn,
                          str::stream()
                              << "Invalid BSONColumn at field '" << elem.getFieldName() << "'");
            }
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
                             containsDotsAndDollarsField,
                             fromOplogApplication);
    }
}

}  // namespace storage_validation
}  // namespace mongo
