/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/exec/str_trim_utils.h"
#include "mongo/db/exec/substr_utils.h"

#include <boost/algorithm/string/case_conv.hpp>

namespace mongo {

namespace exec::expression {

Value evaluate(const ExpressionConcat& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    const size_t n = children.size();

    StringBuilder result;
    for (size_t i = 0; i < n; ++i) {
        Value val = children[i]->evaluate(root, variables);
        if (val.nullish()) {
            return Value(BSONNULL);
        }

        uassert(16702,
                str::stream() << "$concat only supports strings, not " << typeName(val.getType()),
                val.getType() == BSONType::string);

        result << val.coerceToString();
    }

    return Value(result.stringData());
}

Value evaluate(const ExpressionStrcasecmp& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value pString1(children[0]->evaluate(root, variables));
    Value pString2(children[1]->evaluate(root, variables));

    /* boost::iequals returns a bool not an int so strings must actually be allocated */
    std::string str1 = boost::to_upper_copy(pString1.coerceToString());
    std::string str2 = boost::to_upper_copy(pString2.coerceToString());
    int result = str1.compare(str2);

    if (result == 0) {
        return Value(0);
    } else if (result > 0) {
        return Value(1);
    } else {
        return Value(-1);
    }
}

Value evaluate(const ExpressionSubstrBytes& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value pString(children[0]->evaluate(root, variables));
    Value pLower(children[1]->evaluate(root, variables));
    Value pLength(children[2]->evaluate(root, variables));

    std::string str = pString.coerceToString();
    uassert(16034,
            str::stream() << expr.getOpName()
                          << ":  starting index must be a numeric type (is BSON type "
                          << typeName(pLower.getType()) << ")",
            pLower.numeric());
    uassert(16035,
            str::stream() << expr.getOpName() << ":  length must be a numeric type (is BSON type "
                          << typeName(pLength.getType()) << ")",
            pLength.numeric());

    const long long signedLower = pLower.coerceToLong();

    uassert(50752,
            str::stream() << expr.getOpName()
                          << ":  starting index must be non-negative (got: " << signedLower << ")",
            signedLower >= 0);

    const std::string::size_type lower = static_cast<std::string::size_type>(signedLower);

    // If the passed length is negative, we should return the rest of the string.
    const long long signedLength = pLength.coerceToLong();
    const std::string::size_type length =
        signedLength < 0 ? str.length() : static_cast<std::string::size_type>(signedLength);

    uassert(28656,
            str::stream() << expr.getOpName()
                          << ":  Invalid range, starting index is a UTF-8 continuation byte.",
            (lower >= str.length() || !str::isUTF8ContinuationByte(str[lower])));

    // Check the byte after the last character we'd return. If it is a continuation byte, that
    // means we're in the middle of a UTF-8 character.
    uassert(
        28657,
        str::stream() << expr.getOpName()
                      << ":  Invalid range, ending index is in the middle of a UTF-8 character.",
        (lower + length >= str.length() || !str::isUTF8ContinuationByte(str[lower + length])));

    if (lower >= str.length()) {
        // If lower > str.length() then string::substr() will throw out_of_range, so return an
        // empty string if lower is not a valid string index.
        return Value(StringData());
    }
    return Value(StringData(str).substr(lower, length));
}

Value evaluate(const ExpressionSubstrCP& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value inputVal(children[0]->evaluate(root, variables));
    Value lowerVal(children[1]->evaluate(root, variables));
    Value lengthVal(children[2]->evaluate(root, variables));

    std::string str = inputVal.coerceToString();
    uassert(34450,
            str::stream() << expr.getOpName()
                          << ": starting index must be a numeric type (is BSON type "
                          << typeName(lowerVal.getType()) << ")",
            lowerVal.numeric());
    uassert(34451,
            str::stream() << expr.getOpName()
                          << ": starting index cannot be represented as a 32-bit integral value: "
                          << lowerVal.toString(),
            lowerVal.integral());
    uassert(34452,
            str::stream() << expr.getOpName() << ": length must be a numeric type (is BSON type "
                          << typeName(lengthVal.getType()) << ")",
            lengthVal.numeric());
    uassert(34453,
            str::stream() << expr.getOpName()
                          << ": length cannot be represented as a 32-bit integral value: "
                          << lengthVal.toString(),
            lengthVal.integral());

    int startIndexCodePoints = lowerVal.coerceToInt();
    int length = lengthVal.coerceToInt();

    uassert(34454,
            str::stream() << expr.getOpName() << ": length must be a nonnegative integer.",
            length >= 0);

    uassert(34455,
            str::stream() << expr.getOpName()
                          << ": the starting index must be nonnegative integer.",
            startIndexCodePoints >= 0);

    return Value(substr_utils::getSubstringCP(str, startIndexCodePoints, length));
}

namespace {
Value strLenBytes(StringData str) {
    size_t strLen = str.size();

    uassert(34470,
            "string length could not be represented as an int.",
            strLen <= std::numeric_limits<int>::max());
    return Value(static_cast<int>(strLen));
}
}  // namespace

Value evaluate(const ExpressionStrLenBytes& expr, const Document& root, Variables* variables) {
    Value str(expr.getChildren()[0]->evaluate(root, variables));

    uassert(34473,
            str::stream() << "$strLenBytes requires a string argument, found: "
                          << typeName(str.getType()),
            str.getType() == BSONType::string);

    return strLenBytes(str.getStringData());
}

Value evaluate(const ExpressionBinarySize& expr, const Document& root, Variables* variables) {
    Value arg = expr.getChildren()[0]->evaluate(root, variables);
    if (arg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(51276,
            str::stream() << "$binarySize requires a string or BinData argument, found: "
                          << typeName(arg.getType()),
            arg.getType() == BSONType::binData || arg.getType() == BSONType::string);

    if (arg.getType() == BSONType::string) {
        return strLenBytes(arg.getStringData());
    }

    BSONBinData binData = arg.getBinData();
    return Value(binData.length);
}

Value evaluate(const ExpressionStrLenCP& expr, const Document& root, Variables* variables) {
    Value val(expr.getChildren()[0]->evaluate(root, variables));

    uassert(34471,
            str::stream() << "$strLenCP requires a string argument, found: "
                          << typeName(val.getType()),
            val.getType() == BSONType::string);

    std::string stringVal = val.getString();
    size_t strLen = str::lengthInUTF8CodePoints(stringVal);

    uassert(34472,
            "string length could not be represented as an int.",
            strLen <= std::numeric_limits<int>::max());

    return Value(static_cast<int>(strLen));
}

Value evaluate(const ExpressionToLower& expr, const Document& root, Variables* variables) {
    Value pString(expr.getChildren()[0]->evaluate(root, variables));
    std::string str = pString.coerceToString();
    boost::to_lower(str);
    return Value(str);
}

Value evaluate(const ExpressionToUpper& expr, const Document& root, Variables* variables) {
    Value pString(expr.getChildren()[0]->evaluate(root, variables));
    std::string str(pString.coerceToString());
    boost::to_upper(str);
    return Value(str);
}

Value evaluate(const ExpressionTrim& expr, const Document& root, Variables* variables) {
    auto unvalidatedInput = expr.getInput()->evaluate(root, variables);
    if (unvalidatedInput.nullish()) {
        return Value(BSONNULL);
    }
    uassert(50699,
            str::stream() << expr.getName() << " requires its input to be a string, got "
                          << unvalidatedInput.toString() << " (of type "
                          << typeName(unvalidatedInput.getType()) << ") instead.",
            unvalidatedInput.getType() == BSONType::string);
    const StringData input(unvalidatedInput.getStringData());

    auto trimType = expr.getTrimType();
    if (!expr.getCharacters()) {
        return Value(str_trim_utils::doTrim(input,
                                            str_trim_utils::kDefaultTrimWhitespaceChars,
                                            trimType == ExpressionTrim::TrimType::kBoth ||
                                                trimType == ExpressionTrim::TrimType::kLeft,
                                            trimType == ExpressionTrim::TrimType::kBoth ||
                                                trimType == ExpressionTrim::TrimType::kRight));
    }
    auto unvalidatedUserChars = expr.getCharacters()->evaluate(root, variables);
    if (unvalidatedUserChars.nullish()) {
        return Value(BSONNULL);
    }
    uassert(50700,
            str::stream() << expr.getName() << " requires 'chars' to be a string, got "
                          << unvalidatedUserChars.toString() << " (of type "
                          << typeName(unvalidatedUserChars.getType()) << ") instead.",
            unvalidatedUserChars.getType() == BSONType::string);

    return Value(str_trim_utils::doTrim(
        input,
        str_trim_utils::extractCodePointsFromChars(unvalidatedUserChars.getStringData()),
        trimType == ExpressionTrim::TrimType::kBoth || trimType == ExpressionTrim::TrimType::kLeft,
        trimType == ExpressionTrim::TrimType::kBoth ||
            trimType == ExpressionTrim::TrimType::kRight));
}

namespace {

bool stringHasTokenAtIndex(size_t index, const std::string& input, const std::string& token) {
    if (token.size() + index > input.size()) {
        return false;
    }
    return input.compare(index, token.size(), token) == 0;
}

void uassertIfNotIntegralAndNonNegative(Value val,
                                        StringData expressionName,
                                        StringData argumentName) {
    uassert(40096,
            str::stream() << expressionName << "requires an integral " << argumentName
                          << ", found a value of type: " << typeName(val.getType())
                          << ", with value: " << val.toString(),
            val.integral());
    uassert(40097,
            str::stream() << expressionName << " requires a nonnegative " << argumentName
                          << ", found: " << val.toString(),
            val.coerceToInt() >= 0);
}

}  // namespace

Value evaluate(const ExpressionIndexOfBytes& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value stringArg = children[0]->evaluate(root, variables);

    if (stringArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40091,
            str::stream() << "$indexOfBytes requires a string as the first argument, found: "
                          << typeName(stringArg.getType()),
            stringArg.getType() == BSONType::string);
    const std::string& input = stringArg.getString();

    Value tokenArg = children[1]->evaluate(root, variables);
    uassert(40092,
            str::stream() << "$indexOfBytes requires a string as the second argument, found: "
                          << typeName(tokenArg.getType()),
            tokenArg.getType() == BSONType::string);
    const std::string& token = tokenArg.getString();

    size_t startIndex = 0;
    if (children.size() > 2) {
        Value startIndexArg = children[2]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(startIndexArg, expr.getOpName(), "starting index");
        startIndex = static_cast<size_t>(startIndexArg.coerceToInt());
    }

    size_t endIndex = input.size();
    if (children.size() > 3) {
        Value endIndexArg = children[3]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(endIndexArg, expr.getOpName(), "ending index");
        // Don't let 'endIndex' exceed the length of the string.
        endIndex = std::min(input.size(), static_cast<size_t>(endIndexArg.coerceToInt()));
    }

    if (startIndex > input.length() || endIndex < startIndex) {
        return Value(-1);
    }

    size_t position = input.substr(0, endIndex).find(token, startIndex);
    if (position == std::string::npos) {
        return Value(-1);
    }

    return Value(static_cast<int>(position));
}

Value evaluate(const ExpressionIndexOfCP& expr, const Document& root, Variables* variables) {
    auto& children = expr.getChildren();
    Value stringArg = children[0]->evaluate(root, variables);

    if (stringArg.nullish()) {
        return Value(BSONNULL);
    }

    uassert(40093,
            str::stream() << "$indexOfCP requires a string as the first argument, found: "
                          << typeName(stringArg.getType()),
            stringArg.getType() == BSONType::string);
    const std::string& input = stringArg.getString();

    Value tokenArg = children[1]->evaluate(root, variables);
    uassert(40094,
            str::stream() << "$indexOfCP requires a string as the second argument, found: "
                          << typeName(tokenArg.getType()),
            tokenArg.getType() == BSONType::string);
    const std::string& token = tokenArg.getString();

    size_t startCodePointIndex = 0;
    if (children.size() > 2) {
        Value startIndexArg = children[2]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(startIndexArg, expr.getOpName(), "starting index");
        startCodePointIndex = static_cast<size_t>(startIndexArg.coerceToInt());
    }

    // Compute the length (in code points) of the input, and convert 'startCodePointIndex' to a byte
    // index.
    size_t codePointLength = 0;
    size_t startByteIndex = 0;
    for (size_t byteIx = 0; byteIx < input.size(); ++codePointLength) {
        if (codePointLength == startCodePointIndex) {
            // We have determined the byte at which our search will start.
            startByteIndex = byteIx;
        }

        uassert(40095,
                "$indexOfCP found bad UTF-8 in the input",
                !str::isUTF8ContinuationByte(input[byteIx]));
        byteIx += str::getCodePointLength(input[byteIx]);
    }

    size_t endCodePointIndex = codePointLength;
    if (children.size() > 3) {
        Value endIndexArg = children[3]->evaluate(root, variables);
        uassertIfNotIntegralAndNonNegative(endIndexArg, expr.getOpName(), "ending index");

        // Don't let 'endCodePointIndex' exceed the number of code points in the string.
        endCodePointIndex =
            std::min(codePointLength, static_cast<size_t>(endIndexArg.coerceToInt()));
    }

    // If the start index is past the end, then always return -1 since 'token' does not exist within
    // these invalid bounds.
    if (endCodePointIndex < startCodePointIndex) {
        return Value(-1);
    }

    if (startByteIndex == 0 && input.empty() && token.empty()) {
        // If we are finding the index of "" in the string "", the below loop will not loop, so we
        // need a special case for this.
        return Value(0);
    }

    // We must keep track of which byte, and which code point, we are examining, being careful not
    // to overflow either the length of the string or the ending code point.

    size_t currentCodePointIndex = startCodePointIndex;
    for (size_t byteIx = startByteIndex; currentCodePointIndex < endCodePointIndex;
         ++currentCodePointIndex) {
        if (stringHasTokenAtIndex(byteIx, input, token)) {
            return Value(static_cast<int>(currentCodePointIndex));
        }

        byteIx += str::getCodePointLength(input[byteIx]);
    }

    return Value(-1);
}

}  // namespace exec::expression
}  // namespace mongo
