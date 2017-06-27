/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/db/update/storage_validation.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"

namespace mongo {

namespace storage_validation {

namespace {

const StringData idFieldName = "_id"_sd;

void storageValidChildren(mutablebson::ConstElement elem,
                          const bool deep,
                          std::uint32_t recursionLevel) {
    if (!elem.hasChildren()) {
        return;
    }

    auto curr = elem.leftChild();
    while (curr.ok()) {
        storageValid(curr, deep, recursionLevel + 1);
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
    if (currName == "$db") {
        uassert(ErrorCodes::InvalidDBRef,
                str::stream() << "The DBRef $db field must be a String, not a "
                              << typeName(curr.getType()),
                curr.getType() == BSONType::String);
        curr = curr.leftSibling();

        uassert(ErrorCodes::InvalidDBRef,
                "Found $db field without a $id before it, which is invalid.",
                curr.ok() && (curr.getFieldName() == "$id"));

        currName = curr.getFieldName();
    }

    // Found a $id field.
    if (currName == "$id") {
        curr = curr.leftSibling();
        uassert(ErrorCodes::InvalidDBRef,
                "Found $id field without a $ref before it, which is invalid.",
                curr.ok() && (curr.getFieldName() == "$ref"));

        currName = curr.getFieldName();
    }

    // Found a $ref field.
    if (currName == "$ref") {
        uassert(ErrorCodes::InvalidDBRef,
                str::stream() << "The DBRef $ref field must be a String, not a "
                              << typeName(curr.getType()),
                curr.getType() == BSONType::String);

        uassert(ErrorCodes::InvalidDBRef,
                "The DBRef $ref field must be followed by a $id field",
                curr.rightSibling().ok() && curr.rightSibling().getFieldName() == "$id");
    } else {

        // Not an okay, $ prefixed field name.
        uasserted(ErrorCodes::DollarPrefixedFieldName,
                  str::stream() << "The dollar ($) prefixed field '" << elem.getFieldName()
                                << "' in '"
                                << mutablebson::getFullName(elem)
                                << "' is not valid for storage.");
    }
}
}  // namespace

void storageValid(const mutablebson::Document& doc) {
    auto currElem = doc.root().leftChild();
    while (currElem.ok()) {
        if (currElem.getFieldName() == idFieldName) {
            switch (currElem.getType()) {
                case BSONType::RegEx:
                case BSONType::Array:
                case BSONType::Undefined:
                    uasserted(ErrorCodes::InvalidIdField,
                              str::stream() << "The '_id' value cannot be of type "
                                            << typeName(currElem.getType()));
                default:
                    break;
            }
        }

        // Validate this child element.
        const auto deep = true;
        const uint32_t recursionLevel = 1;
        storageValid(currElem, deep, recursionLevel);

        currElem = currElem.rightSibling();
    }
}

void storageValid(mutablebson::ConstElement elem, const bool deep, std::uint32_t recursionLevel) {
    uassert(ErrorCodes::BadValue, "Invalid elements cannot be stored.", elem.ok());

    uassert(ErrorCodes::Overflow,
            str::stream() << "Document exceeds maximum nesting depth of "
                          << BSONDepth::getMaxDepthForUserStorage(),
            recursionLevel <= BSONDepth::getMaxDepthForUserStorage());

    // Field names of elements inside arrays are not meaningful in mutable bson,
    // so we do not want to validate them.
    const mutablebson::ConstElement& parent = elem.parent();
    const bool childOfArray = parent.ok() ? (parent.getType() == BSONType::Array) : false;

    if (!childOfArray) {
        auto fieldName = elem.getFieldName();

        // Cannot start with "$", unless dbref.
        if (fieldName[0] == '$') {
            validateDollarPrefixElement(elem);
        }
    }

    if (deep) {

        // Check children if there are any.
        storageValidChildren(elem, deep, recursionLevel);
    }
}

std::uint32_t storageValidParents(mutablebson::ConstElement elem, std::uint32_t recursionLevel) {
    uassert(ErrorCodes::Overflow,
            str::stream() << "Document exceeds maximum nesting depth of "
                          << BSONDepth::getMaxDepthForUserStorage(),
            recursionLevel <= BSONDepth::getMaxDepthForUserStorage());

    auto root = elem.getDocument().root();
    if (elem != root) {
        auto parent = elem.parent();
        if (parent.ok() && parent != root) {
            const bool doRecursiveCheck = false;
            const uint32_t parentsRecursionLevel = 0;
            storageValid(parent, doRecursiveCheck, parentsRecursionLevel);
            return storageValidParents(parent, recursionLevel + 1);
        }
        return recursionLevel + 1;
    }
    return recursionLevel;
}

}  // namespace storage_validation
}  // namespace mongo
