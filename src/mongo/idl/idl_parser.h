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

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"

namespace mongo {

/**
 * IDLParserErrorContext manages the current parser context for parsing BSON documents.
 *
 * The class stores the path to the current document to enable it provide more useful error
 * messages. The path is a dot delimited list of field names which is useful for nested struct
 * parsing.
 *
 * This class is responsible for throwing all error messages the IDL generated parsers throw,
 * and provide utility methods like checking a BSON type or set of BSON types.
 */
class IDLParserErrorContext {
    MONGO_DISALLOW_COPYING(IDLParserErrorContext);

public:
    IDLParserErrorContext(StringData fieldName) : _currentField(fieldName), _predecessor(nullptr) {}

    IDLParserErrorContext(StringData fieldName, const IDLParserErrorContext* predecessor)
        : _currentField(fieldName), _predecessor(predecessor) {}

    /**
     * Check that BSON element is a given type or whether the field should be skipped.
     *
     * Returns true if the BSON element is the correct type.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertType(const BSONElement& element, BSONType type) const;

    /**
     * Check that BSON element is a bin data type, and has the specified bin data subtype, or
     * whether the field should be skipped.
     *
     * Returns true if the BSON element is the correct type.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertBinDataType(const BSONElement& element, BinDataType type) const;

    /**
     * Check that BSON element is one of a given type or whether the field should be skipped.
     *
     * Returns true if the BSON element is one of the types.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertTypes(const BSONElement& element, const std::vector<BSONType>& types) const;

    /**
     * Throw an error message about the BSONElement being a duplicate field.
     */
    void throwDuplicateField(const BSONElement& element) const;

    /**
     * Throw an error message about the required field missing form the document.
     */
    void throwMissingField(StringData fieldName) const;

    /**
     * Throw an error message about the required field missing form the document.
     */
    void throwUnknownField(StringData fieldName) const;

private:
    /**
     * See comment on getElementPath below.
     */
    std::string getElementPath(const BSONElement& element) const;

    /**
     * Return a dot seperated path to the specified field. For instance, if the code is parsing a
     * grandchild field that has an error, this will return "grandparent.parent.child".
     */
    std::string getElementPath(StringData fieldName) const;

private:
    // Name of the current field that is being parsed.
    const StringData _currentField;

    // Pointer to a parent parser context.
    // This provides a singly linked list of parent pointers, and use to produce a full path to a
    // field with an error.
    const IDLParserErrorContext* _predecessor;
};

}  // namespace mongo
