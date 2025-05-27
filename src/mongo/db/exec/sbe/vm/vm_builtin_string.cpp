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

#include <boost/algorithm/string/case_conv.hpp>

namespace mongo {
namespace sbe {
namespace vm {
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSplit(ArityType arity) {
    auto [ownedSeparator, tagSeparator, valSeparator] = getFromStack(1);
    auto [ownedInput, tagInput, valInput] = getFromStack(0);

    if (!value::isString(tagSeparator) || !value::isString(tagInput)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto input = value::getStringView(tagInput, valInput);
    auto separator = value::getStringView(tagSeparator, valSeparator);

    auto [tag, val] = value::makeNewArray();
    auto arr = value::getArrayView(val);
    value::ValueGuard guard{tag, val};

    size_t splitPos;
    while ((splitPos = input.find(separator)) != std::string::npos) {
        auto [tag, val] = value::makeNewString(input.substr(0, splitPos));
        arr->push_back(tag, val);

        splitPos += separator.size();
        input = input.substr(splitPos);
    }

    // This is the last string.
    {
        auto [tag, val] = value::makeNewString(input);
        arr->push_back(tag, val);
    }

    guard.reset();
    return {true, tag, val};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinReplaceOne(ArityType arity) {
    invariant(arity == 3);

    auto [ownedInputStr, typeTagInputStr, valueInputStr] = getFromStack(0);
    auto [ownedFindStr, typeTagFindStr, valueFindStr] = getFromStack(1);
    auto [ownedReplacementStr, typeTagReplacementStr, valueReplacementStr] = getFromStack(2);

    if (!value::isString(typeTagInputStr) || !value::isString(typeTagFindStr) ||
        !value::isString(typeTagReplacementStr)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto input = value::getStringView(typeTagInputStr, valueInputStr);
    auto find = value::getStringView(typeTagFindStr, valueFindStr);
    auto replacement = value::getStringView(typeTagReplacementStr, valueReplacementStr);

    // If find string is empty, return nothing, since an empty find will match every position in a
    // string.
    if (find.empty()) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // If find string is not found, return the original string.
    size_t startIndex = input.find(find);
    if (startIndex == std::string::npos) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {ownedInputStr, typeTagInputStr, valueInputStr};
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

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinStrLenBytes(ArityType arity) {
    invariant(arity == 1);

    auto [_, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        StringData str = value::getStringView(operandTag, operandVal);
        size_t strLenBytes = str.size();
        uassert(5155801,
                "string length could not be represented as an int.",
                strLenBytes <= std::numeric_limits<int>::max());
        return {false, value::TypeTags::NumberInt32, strLenBytes};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinStrLenCP(ArityType arity) {
    invariant(arity == 1);

    auto [_, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        StringData str = value::getStringView(operandTag, operandVal);
        size_t strLenCP = str::lengthInUTF8CodePoints(str);
        uassert(5155901,
                "string length could not be represented as an int.",
                strLenCP <= std::numeric_limits<int>::max());
        return {false, value::TypeTags::NumberInt32, strLenCP};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSubstrBytes(ArityType arity) {
    invariant(arity == 3);

    auto [strOwned, strTag, strVal] = getFromStack(0);
    auto [startIndexOwned, startIndexTag, startIndexVal] = getFromStack(1);
    auto [lenOwned, lenTag, lenVal] = getFromStack(2);

    if (!value::isString(strTag) || startIndexTag != value::TypeTags::NumberInt64 ||
        lenTag != value::TypeTags::NumberInt64) {
        return {false, value::TypeTags::Nothing, 0};
    }

    StringData str = value::getStringView(strTag, strVal);
    int64_t startIndexBytes = value::bitcastTo<int64_t>(startIndexVal);
    int64_t lenBytes = value::bitcastTo<int64_t>(lenVal);

    // Check start index is positive.
    if (startIndexBytes < 0) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // If passed length is negative, we should return rest of string.
    const StringData::size_type length =
        lenBytes < 0 ? str.length() : static_cast<StringData::size_type>(lenBytes);

    // Check 'startIndexBytes' and byte after last char is not continuation byte.
    uassert(5155604,
            "Invalid range: starting index is a UTF-8 continuation byte",
            (startIndexBytes >= value::bitcastTo<int64_t>(str.length()) ||
             !str::isUTF8ContinuationByte(str[startIndexBytes])));
    uassert(5155605,
            "Invalid range: ending index is a UTF-8 continuation character",
            (startIndexVal + length >= str.length() ||
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

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinSubstrCP(ArityType arity) {
    invariant(arity == 3);

    auto [strOwned, strTag, strVal] = getFromStack(0);
    auto [startIndexOwned, startIndexTag, startIndexVal] = getFromStack(1);
    auto [lenOwned, lenTag, lenVal] = getFromStack(2);

    if (!value::isString(strTag) || startIndexTag != value::TypeTags::NumberInt32 ||
        lenTag != value::TypeTags::NumberInt32 || startIndexVal < 0 || lenVal < 0) {
        return {false, value::TypeTags::Nothing, 0};
    }

    StringData str = value::getStringView(strTag, strVal);
    auto [outTag, outVal] =
        value::makeNewString(substr_utils::getSubstringCP(str, startIndexVal, lenVal));
    return {true, outTag, outVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinToUpper(ArityType arity) {
    auto [_, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        auto [strTag, strVal] = value::copyValue(operandTag, operandVal);
        auto buf = value::getRawStringView(strTag, strVal);
        auto range = std::make_pair(buf, buf + value::getStringLength(strTag, strVal));
        boost::to_upper(range);
        return {true, strTag, strVal};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinToLower(ArityType arity) {
    auto [_, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        auto [strTag, strVal] = value::copyValue(operandTag, operandVal);
        auto buf = value::getRawStringView(strTag, strVal);
        auto range = std::make_pair(buf, buf + value::getStringLength(strTag, strVal));
        boost::to_lower(range);
        return {true, strTag, strVal};
    }
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinCoerceToString(ArityType arity) {
    auto [operandOwn, operandTag, operandVal] = getFromStack(0);

    if (value::isString(operandTag)) {
        topStack(false, value::TypeTags::Nothing, 0);
        return {operandOwn, operandTag, operandVal};
    }

    if (operandTag == value::TypeTags::bsonSymbol) {
        // Values of type StringBig and Values of type bsonSymbol have identical representations,
        // so we can simply take ownership of the argument, change the type tag to StringBig, and
        // return it.
        topStack(false, value::TypeTags::Nothing, 0);
        return {operandOwn, value::TypeTags::StringBig, operandVal};
    }

    switch (operandTag) {
        case value::TypeTags::NumberInt32: {
            str::stream str;
            str << value::bitcastTo<int32_t>(operandVal);
            auto [strTag, strVal] = value::makeNewString(StringData(str));
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberInt64: {
            str::stream str;
            str << value::bitcastTo<int64_t>(operandVal);
            auto [strTag, strVal] = value::makeNewString(StringData(str));
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberDouble: {
            str::stream str;
            str << value::bitcastTo<double>(operandVal);
            auto [strTag, strVal] = value::makeNewString(StringData(str));
            return {true, strTag, strVal};
        }
        case value::TypeTags::NumberDecimal: {
            std::string str = value::bitcastTo<Decimal128>(operandVal).toString();
            auto [strTag, strVal] = value::makeNewString(str);
            return {true, strTag, strVal};
        }
        case value::TypeTags::Date: {
            if (auto formatted = TimeZoneDatabase::utcZone().formatDate(
                    kIsoFormatStringZ,
                    Date_t::fromMillisSinceEpoch(value::bitcastTo<int64_t>(operandVal)));
                formatted.isOK()) {
                // Date formatting successful.
                auto [strTag, strVal] = value::makeNewString(formatted.getValue());
                return {true, strTag, strVal};
            } else {
                // Date formatting failed. Return stringified status.
                str::stream str;
                str << formatted.getStatus();
                auto [strTag, strVal] = value::makeNewString(StringData(str));
                return {true, strTag, strVal};
            }
        }
        case value::TypeTags::Timestamp: {
            Timestamp ts{value::bitcastTo<uint64_t>(operandVal)};
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
    return {false, value::TypeTags::Nothing, 0};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinConcat(ArityType arity) {
    StringBuilder result;
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto [_, tag, value] = getFromStack(idx);
        if (!value::isString(tag)) {
            return {false, value::TypeTags::Nothing, 0};
        }
        result << sbe::value::getStringView(tag, value);
    }

    auto [strTag, strValue] = sbe::value::makeNewString(result.stringData());
    return {true, strTag, strValue};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinTrim(ArityType arity,
                                                                     bool trimLeft,
                                                                     bool trimRight) {
    auto [ownedChars, tagChars, valChars] = getFromStack(1);
    auto [ownedInput, tagInput, valInput] = getFromStack(0);

    if (!value::isString(tagInput)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    // Nullish 'chars' indicates that it was not provided and the default whitespace characters will
    // be used.
    auto replacementChars = !value::isNullish(tagChars)
        ? str_trim_utils::extractCodePointsFromChars(value::getStringView(tagChars, valChars))
        : str_trim_utils::kDefaultTrimWhitespaceChars;
    auto inputString = value::getStringView(tagInput, valInput);

    auto [strTag, strValue] = sbe::value::makeNewString(
        str_trim_utils::doTrim(inputString, replacementChars, trimLeft, trimRight));
    return {true, strTag, strValue};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinIndexOfBytes(ArityType arity) {
    auto [strOwn, strTag, strVal] = getFromStack(0);
    auto [substrOwn, substrTag, substrVal] = getFromStack(1);
    if ((!value::isString(strTag)) || (!value::isString(substrTag))) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto str = value::getStringView(strTag, strVal);
    auto substring = value::getStringView(substrTag, substrVal);
    int64_t startIndex = 0, endIndex = str.size();

    if (arity >= 3) {
        auto [startOwn, startTag, startVal] = getFromStack(2);
        if (startTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        startIndex = value::bitcastTo<int64_t>(startVal);
        // Check index is positive.
        if (startIndex < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (static_cast<size_t>(startIndex) > str.size()) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }
    if (arity >= 4) {
        auto [endOwn, endTag, endVal] = getFromStack(3);
        if (endTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        endIndex = value::bitcastTo<int64_t>(endVal);
        // Check index is positive.
        if (endIndex < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (endIndex < startIndex) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }
    auto index = str.substr(startIndex, endIndex - startIndex).find(substring);
    if (index != std::string::npos) {
        return {
            false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(startIndex + index)};
    }
    return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinIndexOfCP(ArityType arity) {
    auto [strOwn, strTag, strVal] = getFromStack(0);
    auto [substrOwn, substrTag, substrVal] = getFromStack(1);
    if ((!value::isString(strTag)) || (!value::isString(substrTag))) {
        return {false, value::TypeTags::Nothing, 0};
    }
    auto str = value::getStringView(strTag, strVal);
    auto substr = value::getStringView(substrTag, substrVal);
    int64_t startCodePointIndex = 0, endCodePointIndexArg = str.size();

    if (arity >= 3) {
        auto [startOwn, startTag, startVal] = getFromStack(2);
        if (startTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        startCodePointIndex = value::bitcastTo<int64_t>(startVal);
        // Check index is positive.
        if (startCodePointIndex < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (static_cast<size_t>(startCodePointIndex) > str.size()) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }
    if (arity >= 4) {
        auto [endOwn, endTag, endVal] = getFromStack(3);
        if (endTag != value::TypeTags::NumberInt64) {
            return {false, value::TypeTags::Nothing, 0};
        }
        endCodePointIndexArg = value::bitcastTo<int64_t>(endVal);
        // Check index is positive.
        if (endCodePointIndexArg < 0) {
            return {false, value::TypeTags::Nothing, 0};
        }
        // Check for valid bounds.
        if (endCodePointIndexArg < startCodePointIndex) {
            return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
        }
    }

    // Handle edge case if both string and substring are empty strings.
    if (startCodePointIndex == 0 && str.empty() && substr.empty()) {
        return {true, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(0)};
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
            return {
                false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(codePointIndex)};
        }
        byteIndex += str::getCodePointLength(str[byteIndex]);
    }
    return {false, value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(-1)};
}  // ByteCode::builtinIndexOfCP

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinIsValidToStringFormat(
    ArityType arity) {
    auto [formatOwn, formatTag, formatVal] = getFromStack(0);
    if (!value::isString(formatTag)) {
        return {false, value::TypeTags::Boolean, false};
    }
    auto formatStr = value::getStringView(formatTag, formatVal);
    if (TimeZone::isValidToStringFormat(formatStr)) {
        return {false, value::TypeTags::Boolean, true};
    }
    return {false, value::TypeTags::Boolean, false};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinValidateFromStringFormat(
    ArityType arity) {
    auto [formatOwn, formatTag, formatVal] = getFromStack(0);
    if (!value::isString(formatTag)) {
        return {false, value::TypeTags::Boolean, false};
    }
    auto formatStr = value::getStringView(formatTag, formatVal);
    TimeZone::validateFromStringFormat(formatStr);
    return {false, value::TypeTags::Boolean, true};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinHasNullBytes(ArityType arity) {
    invariant(arity == 1);
    auto [strOwned, strType, strValue] = getFromStack(0);

    if (!value::isString(strType)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto stringView = value::getStringView(strType, strValue);
    auto hasNullBytes = stringView.find('\0') != std::string::npos;

    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(hasNullBytes)};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
