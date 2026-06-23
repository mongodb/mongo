/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/exec/str_trim_utils.h"
#include "mongo/db/exec/substr_utils.h"

#include <string_view>

#include <boost/algorithm/string/case_conv.hpp>

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinSplit(ArityType arity) {
    auto separator = viewFromStack(1);
    auto input = viewFromStack(0);

    if (!value::isString(separator.tag) || !value::isString(input.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto inputStr = value::getStringView(input.tag, input.value);
    auto separatorStr = value::getStringView(separator.tag, separator.value);

    value::TagValueOwned result{value::makeNewArray()};
    auto arr = value::getArrayView(result.value());

    size_t splitPos;
    while ((splitPos = inputStr.find(separatorStr)) != std::string::npos) {
        auto [tag, val] = value::makeNewString(inputStr.substr(0, splitPos));
        arr->push_back_raw(tag, val);

        splitPos += separatorStr.size();
        inputStr = inputStr.substr(splitPos);
    }

    // This is the last string.
    {
        auto [tag, val] = value::makeNewString(inputStr);
        arr->push_back_raw(tag, val);
    }

    return std::move(result);
}

value::TagValueMaybeOwned ByteCode::builtinReplaceOne(ArityType arity) {
    tassert(11080005, "Unexpected arity value", arity == 3);

    auto inputStr = viewFromStack(0);
    auto findStr = viewFromStack(1);
    auto replacementStr = viewFromStack(2);

    if (!value::isString(inputStr.tag) || !value::isString(findStr.tag) ||
        !value::isString(replacementStr.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto input = value::getStringView(inputStr.tag, inputStr.value);
    auto find = value::getStringView(findStr.tag, findStr.value);
    auto replacement = value::getStringView(replacementStr.tag, replacementStr.value);

    // If 'find' string is empty, return nothing, since an empty find will match every position in a
    // string.
    if (find.empty()) {
        return value::TagValueMaybeOwned::nothing();
    }

    // If 'find' string is not found, return the original string. Ownership is only transferred on
    // this path; the found path lets popAndReleaseStack clean up slot 0.
    size_t startIndex = input.find(find);
    if (startIndex == std::string::npos) {
        return moveMaybeOwnedFromStack(0);
    }

    StringBuilder output;
    size_t endIndex = startIndex + find.size();
    output << input.substr(0, startIndex);
    output << replacement;
    output << input.substr(endIndex);

    auto strData = output.stringData();
    auto [outputStrTypeTag, outputStrValue] = sbe::value::makeNewString(strData);
    return {true, outputStrTypeTag, outputStrValue};
}

value::TagValueMaybeOwned ByteCode::builtinStrLenBytes(ArityType arity) {
    tassert(11080004, "Unexpected arity value", arity == 1);

    auto operand = viewFromStack(0);

    if (value::isString(operand.tag)) {
        std::string_view str = value::getStringView(operand.tag, operand.value);
        size_t strLenBytes = str.size();
        uassert(5155801,
                "string length could not be represented as an int.",
                strLenBytes <= std::numeric_limits<int>::max());
        return {false, value::TypeTags::NumberInt32, strLenBytes};
    }
    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinStrLenCP(ArityType arity) {
    tassert(11080003, "Unexpected arity value", arity == 1);

    auto operand = viewFromStack(0);

    if (value::isString(operand.tag)) {
        std::string_view str = value::getStringView(operand.tag, operand.value);
        size_t strLenCP = str::lengthInUTF8CodePoints(str);
        uassert(5155901,
                "string length could not be represented as an int.",
                strLenCP <= std::numeric_limits<int>::max());
        return {false, value::TypeTags::NumberInt32, strLenCP};
    }
    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinSubstrBytes(ArityType arity) {
    tassert(11080002, "Unexpected arity value", arity == 3);

    auto strView = viewFromStack(0);
    auto startIndexView = viewFromStack(1);
    auto lenView = viewFromStack(2);

    if (!value::isString(strView.tag) || startIndexView.tag != value::TypeTags::NumberInt64 ||
        lenView.tag != value::TypeTags::NumberInt64) {
        return value::TagValueMaybeOwned::nothing();
    }

    std::string_view str = value::getStringView(strView.tag, strView.value);
    int64_t startIndexBytes = value::bitcastTo<int64_t>(startIndexView.value);
    int64_t lenBytes = value::bitcastTo<int64_t>(lenView.value);

    // Check start index is positive.
    if (startIndexBytes < 0) {
        return value::TagValueMaybeOwned::nothing();
    }

    // If passed length is negative, we should return rest of string.
    const std::string_view::size_type length =
        lenBytes < 0 ? str.length() : static_cast<std::string_view::size_type>(lenBytes);

    // Check 'startIndexBytes' and byte after last char is not continuation byte.
    uassert(5155604,
            "Invalid range: starting index is a UTF-8 continuation byte",
            (startIndexBytes >= value::bitcastTo<int64_t>(str.length()) ||
             !str::isUTF8ContinuationByte(str[startIndexBytes])));
    uassert(5155605,
            "Invalid range: ending index is a UTF-8 continuation character",
            (startIndexBytes + length >= str.length() ||
             !str::isUTF8ContinuationByte(str[startIndexBytes + length])));

    // If 'startIndexVal' > str.length() then string::substr() will throw out_of_range, so return
    // empty string if 'startIndexVal' is not a valid string index.
    if (startIndexBytes >= value::bitcastTo<int64_t>(str.length())) {
        auto [outTag, outVal] = value::makeNewString("");
        return {true, outTag, outVal};
    }
    auto [outTag, outVal] = value::makeNewString(str.substr(startIndexBytes, lenBytes));
    return {true, outTag, outVal};
}

value::TagValueMaybeOwned ByteCode::builtinSubstrCP(ArityType arity) {
    tassert(11080001, "Unexpected arity value", arity == 3);

    auto strView = viewFromStack(0);
    auto startIndexView = viewFromStack(1);
    auto lenView = viewFromStack(2);

    if (!value::isString(strView.tag) || startIndexView.tag != value::TypeTags::NumberInt32 ||
        lenView.tag != value::TypeTags::NumberInt32) {
        return value::TagValueMaybeOwned::nothing();
    }

    int32_t startIndex = value::bitcastTo<int32_t>(startIndexView.value);
    int32_t len = value::bitcastTo<int32_t>(lenView.value);
    if (startIndex < 0 || len < 0) {
        return value::TagValueMaybeOwned::nothing();
    }

    std::string_view str = value::getStringView(strView.tag, strView.value);
    auto [outTag, outVal] =
        value::makeNewString(substr_utils::getSubstringCP(str, startIndex, len));
    return {true, outTag, outVal};
}

value::TagValueMaybeOwned ByteCode::builtinToUpper(ArityType arity) {
    auto operand = viewFromStack(0);

    if (value::isString(operand.tag)) {
        auto [strTag, strVal] = value::copyValue(operand.tag, operand.value);
        auto buf = value::getRawStringView(strTag, strVal);
        auto range = std::make_pair(buf, buf + value::getStringLength(strTag, strVal));
        boost::to_upper(range);
        return {true, strTag, strVal};
    }
    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinToLower(ArityType arity) {
    auto operand = viewFromStack(0);

    if (value::isString(operand.tag)) {
        auto [strTag, strVal] = value::copyValue(operand.tag, operand.value);
        auto buf = value::getRawStringView(strTag, strVal);
        auto range = std::make_pair(buf, buf + value::getStringLength(strTag, strVal));
        boost::to_lower(range);
        return {true, strTag, strVal};
    }
    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinCoerceToString(ArityType arity) {
    auto operand = moveMaybeOwnedFromStack(0);

    if (value::isString(operand.tag())) {
        return operand;
    }

    if (operand.tag() == value::TypeTags::bsonSymbol) {
        // Values of type StringBig and Values of type bsonSymbol have identical representations,
        // so we can simply take ownership of the argument, change the type tag to StringBig, and
        // return it.
        auto [owned, _, val] = operand.releaseToRaw();
        return {owned, value::TypeTags::StringBig, val};
    }

    switch (operand.tag()) {
        case value::TypeTags::NumberInt32: {
            str::stream str;
            str << value::bitcastTo<int32_t>(operand.value());
            auto [strTag, strVal] = value::makeNewString(std::string_view(str));
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberInt64: {
            str::stream str;
            str << value::bitcastTo<int64_t>(operand.value());
            auto [strTag, strVal] = value::makeNewString(std::string_view(str));
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberDouble: {
            str::stream str;
            str << value::bitcastTo<double>(operand.value());
            auto [strTag, strVal] = value::makeNewString(std::string_view(str));
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberDecimal: {
            std::string str = value::bitcastTo<Decimal128>(operand.value()).toString();
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::Date: {
            if (auto formatted = TimeZoneDatabase::utcZone().formatDate(
                    kIsoFormatStringZ,
                    Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(operand.value())));
                formatted.isOK()) {
                // Date formatting successful.
                auto [strTag, strVal] = value::makeNewString(formatted.getValue());
                return {true, strTag, strVal};
            } else {
                // Date formatting failed. Return stringified status.
                str::stream str;
                str << formatted.getStatus();
                auto [strTag, strVal] = value::makeNewString(std::string_view(str));
                return {true, strTag, strVal};
            }
        }
        case value::TypeTags::Timestamp: {
            Timestamp ts{value::bitcastTo<uint64_t>(operand.value())};
            auto [strTag, strVal] = value::makeNewString(ts.toString());
            return {true, strTag, strVal};
        }
        case value::TypeTags::Null: {
            auto [strTag, strVal] = value::makeNewString("");
            return {true, strTag, strVal};
        }
        default:
            break;
    }
    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinConcat(ArityType arity) {
    StringBuilder result;
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto kv = viewFromStack(idx);
        if (!value::isString(kv.tag)) {
            return value::TagValueMaybeOwned::nothing();
        }
        result << sbe::value::getStringView(kv.tag, kv.value);
    }

    auto [strTag, strValue] = sbe::value::makeNewString(result.stringData());
    return {true, strTag, strValue};
}

value::TagValueMaybeOwned ByteCode::builtinTrim(ArityType arity, bool trimLeft, bool trimRight) {
    auto charsView = viewFromStack(1);
    auto inputView = viewFromStack(0);

    if (!value::isString(inputView.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    std::vector<std::string_view> replacementChars;
    // Nullish 'chars' indicates that it was not provided and the default whitespace characters will
    // be used.
    if (value::isNullish(charsView.tag)) {
        replacementChars = str_trim_utils::defaultTrimWhitespaceChars();
    } else {
        auto charsStringData = value::getStringView(charsView.tag, charsView.value);
        uassert(12066801,
                str::stream() << "$trim/$ltrim/$rtrim requires 'chars' to be not greater than "
                              << str_trim_utils::kMaximumAllowedTrimStringBytes << " bytes, got "
                              << charsStringData.length() << " bytes instead.",
                charsStringData.length() <= str_trim_utils::kMaximumAllowedTrimStringBytes);
        replacementChars = str_trim_utils::extractCodePointsFromChars(charsStringData);
    }

    auto inputString = value::getStringView(inputView.tag, inputView.value);

    auto [strTag, strValue] = sbe::value::makeNewString(
        str_trim_utils::doTrim(inputString, replacementChars, trimLeft, trimRight));
    return {true, strTag, strValue};
}

value::TagValueMaybeOwned ByteCode::builtinIndexOfBytes(ArityType arity) {
    auto strView = viewFromStack(0);
    auto substrView = viewFromStack(1);
    if ((!value::isString(strView.tag)) || (!value::isString(substrView.tag))) {
        return value::TagValueMaybeOwned::nothing();
    }
    auto str = value::getStringView(strView.tag, strView.value);
    auto substring = value::getStringView(substrView.tag, substrView.value);
    int64_t startIndex = 0, endIndex = str.size();

    if (arity >= 3) {
        auto startView = viewFromStack(2);
        if (startView.tag != value::TypeTags::NumberInt64) {
            return value::TagValueMaybeOwned::nothing();
        }
        startIndex = value::bitcastTo<int64_t>(startView.value);
        // Check index is positive.
        if (startIndex < 0) {
            return value::TagValueMaybeOwned::nothing();
        }
        // Check for valid bounds.
        if (static_cast<size_t>(startIndex) > str.size()) {
            return value::TagValueMaybeOwned::numberInt32(-1);
        }
    }
    if (arity >= 4) {
        auto endView = viewFromStack(3);
        if (endView.tag != value::TypeTags::NumberInt64) {
            return value::TagValueMaybeOwned::nothing();
        }
        endIndex = value::bitcastTo<int64_t>(endView.value);
        // Check index is positive.
        if (endIndex < 0) {
            return value::TagValueMaybeOwned::nothing();
        }
        // Check for valid bounds.
        if (endIndex < startIndex) {
            return value::TagValueMaybeOwned::numberInt32(-1);
        }
    }
    auto index = str.substr(startIndex, endIndex - startIndex).find(substring);
    if (index != std::string::npos) {
        return value::TagValueMaybeOwned::numberInt32(static_cast<int32_t>(startIndex + index));
    }
    return value::TagValueMaybeOwned::numberInt32(-1);
}

value::TagValueMaybeOwned ByteCode::builtinIndexOfCP(ArityType arity) {
    auto strView = viewFromStack(0);
    auto substrView = viewFromStack(1);
    if ((!value::isString(strView.tag)) || (!value::isString(substrView.tag))) {
        return value::TagValueMaybeOwned::nothing();
    }
    auto str = value::getStringView(strView.tag, strView.value);
    auto substr = value::getStringView(substrView.tag, substrView.value);
    int64_t startCodePointIndex = 0, endCodePointIndexArg = str.size();

    if (arity >= 3) {
        auto startView = viewFromStack(2);
        if (startView.tag != value::TypeTags::NumberInt64) {
            return value::TagValueMaybeOwned::nothing();
        }
        startCodePointIndex = value::bitcastTo<int64_t>(startView.value);
        // Check index is positive.
        if (startCodePointIndex < 0) {
            return value::TagValueMaybeOwned::nothing();
        }
        // Check for valid bounds.
        if (static_cast<size_t>(startCodePointIndex) > str.size()) {
            return value::TagValueMaybeOwned::numberInt32(-1);
        }
    }
    if (arity >= 4) {
        auto endView = viewFromStack(3);
        if (endView.tag != value::TypeTags::NumberInt64) {
            return value::TagValueMaybeOwned::nothing();
        }
        endCodePointIndexArg = value::bitcastTo<int64_t>(endView.value);
        // Check index is positive.
        if (endCodePointIndexArg < 0) {
            return value::TagValueMaybeOwned::nothing();
        }
        // Check for valid bounds.
        if (endCodePointIndexArg < startCodePointIndex) {
            return value::TagValueMaybeOwned::numberInt32(-1);
        }
    }

    // Handle edge case if both string and substring are empty strings.
    if (startCodePointIndex == 0 && str.empty() && substr.empty()) {
        return value::TagValueMaybeOwned::numberInt32(0);
    }

    // Need to get byte indexes for start and end indexes.
    int64_t startByteIndex = 0, byteIndex = 0, codePointIndex;
    for (codePointIndex = 0; static_cast<size_t>(byteIndex) < str.size(); codePointIndex++) {
        if (codePointIndex == startCodePointIndex) {
            startByteIndex = byteIndex;
        }
        uassert(5075307,
                "$indexOfCP found bad UTF-8 in the input",
                !str::isUTF8ContinuationByte(str[byteIndex]));
        byteIndex += str::getCodePointLength(str[byteIndex]);
    }

    int64_t endCodePointIndex = std::min(codePointIndex, endCodePointIndexArg);
    byteIndex = startByteIndex;
    for (codePointIndex = startCodePointIndex; codePointIndex < endCodePointIndex;
         ++codePointIndex) {
        if (str.substr(byteIndex, substr.size()) == substr) {
            return value::TagValueMaybeOwned::numberInt32(static_cast<int32_t>(codePointIndex));
        }
        byteIndex += str::getCodePointLength(str[byteIndex]);
    }
    return value::TagValueMaybeOwned::numberInt32(-1);
}  // ByteCode::builtinIndexOfCP

value::TagValueMaybeOwned ByteCode::builtinIsValidToStringFormat(ArityType arity) {
    auto formatView = viewFromStack(0);
    if (!value::isString(formatView.tag)) {
        return value::TagValueMaybeOwned::boolean(false);
    }
    auto formatStr = value::getStringView(formatView.tag, formatView.value);
    if (TimeZone::isValidToStringFormat(formatStr)) {
        return value::TagValueMaybeOwned::boolean(true);
    }
    return value::TagValueMaybeOwned::boolean(false);
}

value::TagValueMaybeOwned ByteCode::builtinValidateFromStringFormat(ArityType arity) {
    auto formatView = viewFromStack(0);
    if (!value::isString(formatView.tag)) {
        return value::TagValueMaybeOwned::boolean(false);
    }
    auto formatStr = value::getStringView(formatView.tag, formatView.value);
    TimeZone::validateFromStringFormat(formatStr);
    return value::TagValueMaybeOwned::boolean(true);
}

value::TagValueMaybeOwned ByteCode::builtinHasNullBytes(ArityType arity) {
    tassert(11080000, "Unexpected arity value", arity == 1);
    auto strView = viewFromStack(0);

    if (!value::isString(strView.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto stringView = value::getStringView(strView.tag, strView.value);
    auto hasNullBytes = stringView.find('\0') != std::string::npos;

    return value::TagValueMaybeOwned::boolean(hasNullBytes);
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
