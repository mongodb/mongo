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

#include <algorithm>
#include <stack>
#include <string>

#include "mongo/idl/idl_parser.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
/**
 * For a vector of BSONType, return a string of comma separated names.
 *
 * Example: "string, bool, numberDouble"
 */
std::string toCommaDelimitedList(const std::vector<BSONType>& types) {
    str::stream builder;

    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i > 0) {
            builder << ", ";
        }

        builder << typeName(types[i]);
    }

    return builder;
}

}  // namespace

constexpr StringData IDLParserErrorContext::kOpMsgDollarDBDefault;
constexpr StringData IDLParserErrorContext::kOpMsgDollarDB;

bool IDLParserErrorContext::checkAndAssertTypeSlowPath(const BSONElement& element,
                                                       BSONType type) const {
    auto elementType = element.type();

    // If the type is wrong, ignore Null and Undefined values
    if (elementType == jstNULL || elementType == Undefined) {
        return false;
    }

    std::string path = getElementPath(element);
    uasserted(ErrorCodes::TypeMismatch,
              str::stream() << "BSON field '" << path << "' is the wrong type '"
                            << typeName(elementType)
                            << "', expected type '"
                            << typeName(type)
                            << "'");
}

bool IDLParserErrorContext::checkAndAssertBinDataTypeSlowPath(const BSONElement& element,
                                                              BinDataType type) const {
    bool isBinDataType = checkAndAssertType(element, BinData);
    if (!isBinDataType) {
        return false;
    }

    if (element.binDataType() != type) {
        std::string path = getElementPath(element);
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "BSON field '" << path << "' is the wrong bindData type '"
                                << typeName(element.binDataType())
                                << "', expected type '"
                                << typeName(type)
                                << "'");
    }

    return true;
}

bool IDLParserErrorContext::checkAndAssertTypes(const BSONElement& element,
                                                const std::vector<BSONType>& types) const {
    auto elementType = element.type();

    auto pos = std::find(types.begin(), types.end(), elementType);
    if (pos == types.end()) {
        // If the type is wrong, ignore Null and Undefined values
        if (elementType == jstNULL || elementType == Undefined) {
            return false;
        }

        std::string path = getElementPath(element);
        std::string type_str = toCommaDelimitedList(types);
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "BSON field '" << path << "' is the wrong type '"
                                << typeName(element.type())
                                << "', expected types '["
                                << type_str
                                << "']");
    }

    return true;
}


std::string IDLParserErrorContext::getElementPath(const BSONElement& element) const {
    return getElementPath(element.fieldNameStringData());
}

std::string IDLParserErrorContext::getElementPath(StringData fieldName) const {
    if (_predecessor == nullptr) {
        str::stream builder;

        builder << _currentField;

        if (!fieldName.empty()) {
            builder << "." << fieldName;
        }

        return builder;
    } else {
        std::stack<StringData> pieces;

        if (!fieldName.empty()) {
            pieces.push(fieldName);
        }

        pieces.push(_currentField);

        const IDLParserErrorContext* head = _predecessor;
        while (head) {
            pieces.push(head->_currentField);
            head = head->_predecessor;
        }

        str::stream builder;

        while (!pieces.empty()) {
            builder << pieces.top();
            pieces.pop();

            if (!pieces.empty()) {
                builder << ".";
            }
        }

        return builder;
    }
}

void IDLParserErrorContext::throwDuplicateField(StringData fieldName) const {
    std::string path = getElementPath(fieldName);
    uasserted(40413, str::stream() << "BSON field '" << path << "' is a duplicate field");
}

void IDLParserErrorContext::throwDuplicateField(const BSONElement& element) const {
    throwDuplicateField(element.fieldNameStringData());
}

void IDLParserErrorContext::throwMissingField(StringData fieldName) const {
    std::string path = getElementPath(fieldName);
    uasserted(40414,
              str::stream() << "BSON field '" << path << "' is missing but a required field");
}

void IDLParserErrorContext::throwUnknownField(StringData fieldName) const {
    std::string path = getElementPath(fieldName);
    uasserted(40415, str::stream() << "BSON field '" << path << "' is an unknown field.");
}

void IDLParserErrorContext::throwBadArrayFieldNumberValue(StringData value) const {
    std::string path = getElementPath(StringData());
    uasserted(40422,
              str::stream() << "BSON array field '" << path << "' has an invalid value '" << value
                            << "' for an array field name.");
}

void IDLParserErrorContext::throwBadArrayFieldNumberSequence(std::uint32_t actualValue,
                                                             std::uint32_t expectedValue) const {
    std::string path = getElementPath(StringData());
    uasserted(40423,
              str::stream() << "BSON array field '" << path << "' has a non-sequential value '"
                            << actualValue
                            << "' for an array field name, expected value '"
                            << expectedValue
                            << "'.");
}

void IDLParserErrorContext::throwBadEnumValue(int enumValue) const {
    std::string path = getElementPath(StringData());
    uasserted(ErrorCodes::BadValue,
              str::stream() << "Enumeration value '" << enumValue << "' for field '" << path
                            << "' is not a valid value.");
}

void IDLParserErrorContext::throwBadEnumValue(StringData enumValue) const {
    std::string path = getElementPath(StringData());
    uasserted(ErrorCodes::BadValue,
              str::stream() << "Enumeration value '" << enumValue << "' for field '" << path
                            << "' is not a valid value.");
}

NamespaceString IDLParserErrorContext::parseNSCollectionRequired(StringData dbName,
                                                                 const BSONElement& element) {
    const bool isUUID = (element.canonicalType() == canonicalizeBSONType(mongo::BinData) &&
                         element.binDataType() == BinDataType::newUUID);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Collection name must be provided. UUID is not valid in this "
                          << "context",
            !isUUID);
    uassert(ErrorCodes::BadValue,
            str::stream() << "collection name has invalid type " << typeName(element.type()),
            element.canonicalType() == canonicalizeBSONType(mongo::String));

    const NamespaceString nss(dbName, element.valueStringData());

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
            nss.isValid());

    return nss;
}

NamespaceStringOrUUID IDLParserErrorContext::parseNsOrUUID(StringData dbname,
                                                           const BSONElement& element) {
    if (element.type() == BinData && element.binDataType() == BinDataType::newUUID) {
        return {dbname.toString(), uassertStatusOK(UUID::parse(element))};
    } else {
        // Ensure collection identifier is not a Command
        const NamespaceString nss(parseNSCollectionRequired(dbname, element));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid collection name specified '" << nss.ns() << "'",
                nss.isNormal());
        return nss;
    }
}

void IDLParserErrorContext::appendGenericCommandArguments(
    const BSONObj& commandPassthroughFields,
    const std::vector<StringData>& knownFields,
    BSONObjBuilder* builder) {

    for (const auto& element : commandPassthroughFields) {

        StringData name = element.fieldNameStringData();
        // Include a passthrough field as long the IDL class has not defined it.
        if (mongo::isGenericArgument(name) &&
            std::find(knownFields.begin(), knownFields.end(), name) == knownFields.end()) {
            builder->append(element);
        }
    }
}

std::vector<StringData> transformVector(const std::vector<std::string>& input) {
    return std::vector<StringData>(begin(input), end(input));
}

std::vector<std::string> transformVector(const std::vector<StringData>& input) {
    std::vector<std::string> output;

    output.reserve(input.size());

    std::transform(begin(input), end(input), std::back_inserter(output), [](auto&& str) {
        return str.toString();
    });

    return output;
}

std::vector<ConstDataRange> transformVector(const std::vector<std::vector<std::uint8_t>>& input) {
    std::vector<ConstDataRange> output;

    output.reserve(input.size());

    std::transform(begin(input), end(input), std::back_inserter(output), [](auto&& vec) {
        return makeCDR(vec);
    });

    return output;
}

std::vector<std::vector<std::uint8_t>> transformVector(const std::vector<ConstDataRange>& input) {
    std::vector<std::vector<std::uint8_t>> output;

    output.reserve(input.size());

    std::transform(begin(input), end(input), std::back_inserter(output), [](auto&& cdr) {
        return std::vector<std::uint8_t>(reinterpret_cast<const uint8_t*>(cdr.data()),
                                         reinterpret_cast<const uint8_t*>(cdr.data()) +
                                             cdr.length());
    });

    return output;
}
}  // namespace mongo
