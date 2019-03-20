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

#pragma once

#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"

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

    template <typename T>
    friend void throwComparisonError(IDLParserErrorContext& ctxt,
                                     StringData fieldName,
                                     StringData op,
                                     T actualValue,
                                     T expectedValue);

public:
    /**
     * String constants for well-known IDL fields.
     */
    static constexpr auto kOpMsgDollarDB = "$db"_sd;
    static constexpr auto kOpMsgDollarDBDefault = "admin"_sd;

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
    bool checkAndAssertType(const BSONElement& element, BSONType type) const {
        if (MONGO_likely(element.type() == type)) {
            return true;
        }

        return checkAndAssertTypeSlowPath(element, type);
    }

    /**
     * Check that BSON element is a bin data type, and has the specified bin data subtype, or
     * whether the field should be skipped.
     *
     * Returns true if the BSON element is the correct type.
     * Return false if the BSON element is Null or Undefined and the field's value should not be
     * processed.
     * Throws an exception if the BSON element's type is wrong.
     */
    bool checkAndAssertBinDataType(const BSONElement& element, BinDataType type) const {
        if (MONGO_likely(element.type() == BinData && element.binDataType() == type)) {
            return true;
        }

        return checkAndAssertBinDataTypeSlowPath(element, type);
    }

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
    MONGO_COMPILER_NORETURN void throwDuplicateField(const BSONElement& element) const;

    /**
     * Throw an error message about the BSONElement being a duplicate field.
     */
    MONGO_COMPILER_NORETURN void throwDuplicateField(StringData fieldName) const;

    /**
     * Throw an error message about the required field missing from the document.
     */
    MONGO_COMPILER_NORETURN void throwMissingField(StringData fieldName) const;

    /**
     * Throw an error message about an unknown field in a document.
     */
    MONGO_COMPILER_NORETURN void throwUnknownField(StringData fieldName) const;

    /**
     * Throw an error message about an array field name not being a valid unsigned integer.
     */
    MONGO_COMPILER_NORETURN void throwBadArrayFieldNumberValue(StringData value) const;

    /**
     * Throw an error message about the array field name not being the next number in the sequence.
     */
    MONGO_COMPILER_NORETURN void throwBadArrayFieldNumberSequence(
        std::uint32_t actualValue, std::uint32_t expectedValue) const;

    /**
     * Throw an error message about an unrecognized enum value.
     */
    MONGO_COMPILER_NORETURN void throwBadEnumValue(StringData enumValue) const;
    MONGO_COMPILER_NORETURN void throwBadEnumValue(int enumValue) const;

    /**
     * Equivalent to CommandHelpers::parseNsCollectionRequired
     */
    static NamespaceString parseNSCollectionRequired(StringData dbName, const BSONElement& element);

    /**
     * Equivalent to CommandHelpers::parseNsOrUUID
     */
    static NamespaceStringOrUUID parseNsOrUUID(StringData dbname, const BSONElement& element);

    /**
     * Take all the well known command generic arguments from commandPassthroughFields, but ignore
     * fields that are already part of the command and append the rest to builder.
     */
    static void appendGenericCommandArguments(const BSONObj& commandPassthroughFields,
                                              const std::vector<StringData>& knownFields,
                                              BSONObjBuilder* builder);

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

    /**
     * See comment on checkAndAssertType.
     */
    bool checkAndAssertTypeSlowPath(const BSONElement& element, BSONType type) const;

    /**
    * See comment on checkAndAssertBinDataType.
    */
    bool checkAndAssertBinDataTypeSlowPath(const BSONElement& element, BinDataType type) const;

private:
    // Name of the current field that is being parsed.
    const StringData _currentField;

    // Pointer to a parent parser context.
    // This provides a singly linked list of parent pointers, and use to produce a full path to a
    // field with an error.
    const IDLParserErrorContext* _predecessor;
};

/**
 * Throw an error when BSON validation fails during parse.
 */
template <typename T>
void throwComparisonError(IDLParserErrorContext& ctxt,
                          StringData fieldName,
                          StringData op,
                          T actualValue,
                          T expectedValue) {
    std::string path = ctxt.getElementPath(fieldName);
    throwComparisonError(path, op, actualValue, expectedValue);
}

/**
 * Throw an error when a user calls a setter and it fails the comparison.
 */
template <typename T>
void throwComparisonError(StringData fieldName, StringData op, T actualValue, T expectedValue) {
    uasserted(51024,
              str::stream() << "BSON field '" << fieldName << "' value must be " << op << " "
                            << expectedValue
                            << ", actual value '"
                            << actualValue
                            << "'");
}


/**
 * Transform a vector of input type to a vector of output type.
 *
 * Used by the IDL generated code to transform between vectors of view, and non-view types.
 */
std::vector<StringData> transformVector(const std::vector<std::string>& input);
std::vector<std::string> transformVector(const std::vector<StringData>& input);
std::vector<ConstDataRange> transformVector(const std::vector<std::vector<std::uint8_t>>& input);
std::vector<std::vector<std::uint8_t>> transformVector(const std::vector<ConstDataRange>& input);

/**
 * Get a ConstDataRange from a vector or an array of bytes.
 */
inline ConstDataRange makeCDR(const std::vector<uint8_t>& value) {
    return ConstDataRange(reinterpret_cast<const char*>(value.data()), value.size());
}

inline ConstDataRange makeCDR(const std::array<uint8_t, 16>& value) {
    return ConstDataRange(reinterpret_cast<const char*>(value.data()), value.size());
}

}  // namespace mongo
