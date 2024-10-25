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

#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <stack>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

/**
 * For a vector of BSONType, return a string of comma separated names.
 *
 * Example: "string, bool, numberDouble"
 */
std::string toCommaDelimitedList(std::span<const BSONType> types) {
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

constexpr StringData IDLParserContext::kOpMsgDollarDBDefault;
constexpr StringData IDLParserContext::kOpMsgDollarDB;
constexpr auto collectionlessAggregateCursorCol = "$cmd.aggregate"_sd;

bool IDLParserContext::checkAndAssertTypeSlowPath(const BSONElement& element, BSONType type) const {
    auto elementType = element.type();

    // If the type is wrong, ignore Null and Undefined values
    if (elementType == jstNULL || elementType == Undefined) {
        return false;
    }

    std::string path = getElementPath(element);
    uasserted(ErrorCodes::TypeMismatch,
              str::stream() << "BSON field '" << path << "' is the wrong type '"
                            << typeName(elementType) << "', expected type '" << typeName(type)
                            << "'");
}

bool IDLParserContext::checkAndAssertBinDataTypeSlowPath(const BSONElement& element,
                                                         BinDataType type) const {
    bool isBinDataType = checkAndAssertType(element, BinData);
    if (!isBinDataType) {
        return false;
    }

    if (element.binDataType() != type) {
        std::string path = getElementPath(element);
        uasserted(ErrorCodes::TypeMismatch,
                  str::stream() << "BSON field '" << path << "' is the wrong binData type '"
                                << typeName(element.binDataType()) << "', expected type '"
                                << typeName(type) << "'");
    }

    return true;
}

bool IDLParserContext::checkAndAssertTypes(const BSONElement& element,
                                           std::span<const BSONType> types) const {
    auto elementType = element.type();

    auto pos = std::find(types.begin(), types.end(), elementType);
    if (pos == types.end()) {
        // If the type is wrong, ignore Null and Undefined values
        if (elementType == jstNULL || elementType == Undefined) {
            return false;
        }

        throwBadType(element, types);
    }

    return true;
}


std::string IDLParserContext::getElementPath(const BSONElement& element) const {
    return getElementPath(element.fieldNameStringData());
}

std::string IDLParserContext::getElementPath(StringData fieldName) const {
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

        const IDLParserContext* head = _predecessor;
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

void IDLParserContext::throwDuplicateField(StringData fieldName) const {
    std::string path = getElementPath(fieldName);
    uasserted(ErrorCodes::IDLDuplicateField,
              str::stream() << "BSON field '" << path << "' is a duplicate field");
}

void IDLParserContext::throwDuplicateField(const BSONElement& element) const {
    throwDuplicateField(element.fieldNameStringData());
}

void IDLParserContext::throwMissingField(StringData fieldName) const {
    std::string path = getElementPath(fieldName);
    uasserted(ErrorCodes::IDLFailedToParse,
              str::stream() << "BSON field '" << path << "' is missing but a required field");
}

bool isMongocryptdArgument(StringData arg) {
    return arg == "jsonSchema"_sd;
}

void IDLParserContext::throwUnknownField(StringData fieldName) const {
    std::string path = getElementPath(fieldName);
    if (isMongocryptdArgument(fieldName)) {
        uasserted(
            ErrorCodes::IDLUnknownFieldPossibleMongocryptd,
            str::stream()
                << "BSON field '" << path
                << "' is an unknown field. This command may be meant for a mongocryptd process.");
    }

    uasserted(ErrorCodes::IDLUnknownField,
              str::stream() << "BSON field '" << path << "' is an unknown field.");
}

void IDLParserContext::throwBadArrayFieldNumberSequence(StringData actual,
                                                        StringData expected) const {
    uasserted(
        ErrorCodes::BadValue,
        fmt::format("BSON array field '{}' has an invalid index field name: '{}', expected '{}'",
                    getElementPath(StringData()),
                    actual,
                    expected));
}

void IDLParserContext::throwBadEnumValue(int enumValue) const {
    std::string path = getElementPath(StringData());
    uasserted(ErrorCodes::BadValue,
              str::stream() << "Enumeration value '" << enumValue << "' for field '" << path
                            << "' is not a valid value.");
}

void IDLParserContext::throwBadEnumValue(StringData enumValue) const {
    std::string path = getElementPath(StringData());
    uasserted(ErrorCodes::BadValue,
              str::stream() << "Enumeration value '" << enumValue << "' for field '" << path
                            << "' is not a valid value.");
}

void IDLParserContext::throwBadType(const BSONElement& element,
                                    std::span<const BSONType> types) const {
    std::string path = getElementPath(element);
    std::string type_str = toCommaDelimitedList(types);
    uasserted(ErrorCodes::TypeMismatch,
              str::stream() << "BSON field '" << path << "' is the wrong type '"
                            << typeName(element.type()) << "', expected types '[" << type_str
                            << "]'");
}

StringData IDLParserContext::checkAndAssertCollectionName(const BSONElement& element,
                                                          bool allowGlobalCollectionName) {
    const bool isUUID = (element.canonicalType() == canonicalizeBSONType(mongo::BinData) &&
                         element.binDataType() == BinDataType::newUUID);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Collection name must be provided. UUID is not valid in this "
                          << "context",
            !isUUID);

    if (allowGlobalCollectionName && element.isNumber()) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "Invalid command format: the '" << element.fieldNameStringData()
                              << "' field must specify a collection name or 1",
                element.number() == 1);
        return collectionlessAggregateCursorCol;
    }

    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "collection name has invalid type " << typeName(element.type()),
            element.canonicalType() == canonicalizeBSONType(mongo::String));

    return element.valueStringData();
}

std::variant<UUID, StringData> IDLParserContext::checkAndAssertCollectionNameOrUUID(
    const BSONElement& element) {
    if (element.type() == BinData && element.binDataType() == BinDataType::newUUID) {
        return uassertStatusOK(UUID::parse(element));
    } else {
        // Ensure collection identifier is not a Command
        return checkAndAssertCollectionName(element, false);
    }
}

const boost::optional<TenantId>& IDLParserContext::getTenantId() const {
    if (_tenantId || _predecessor == nullptr)
        return _tenantId;

    return _predecessor->getTenantId();
}

const SerializationContext& IDLParserContext::getSerializationContext() const {
    return _serializationContext;
}

const boost::optional<auth::ValidatedTenancyScope>& IDLParserContext::getValidatedTenancyScope()
    const {
    return _validatedTenancyScope;
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
        return ConstDataRange(vec);
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

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void noOpSerializer(bool, StringData fieldName, BSONObjBuilder* bob) {}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeBSONWhenNotEmpty(BSONObj obj, StringData fieldName, BSONObjBuilder* bob) {
    if (!obj.isEmpty()) {
        bob->append(fieldName, obj);
    }
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
BSONObj parseOwnedBSON(BSONElement element) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Expected field " << element.fieldNameStringData()
                          << "to be of type object",
            element.type() == BSONType::Object);
    return element.Obj().getOwned();
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
bool parseBoolean(BSONElement element) {
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Expected field " << element.fieldNameStringData()
                          << "to be of type object",
            element.type() == BSONType::Bool);
    return element.boolean();
}

}  // namespace mongo
