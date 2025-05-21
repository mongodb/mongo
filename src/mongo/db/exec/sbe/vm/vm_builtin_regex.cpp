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

#include "mongo/db/exec/sbe/util/pcre.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace sbe {
namespace vm {
namespace {
/**
 * A helper function to extract the next match in the subject string using the compiled regex
 * pattern.
 * - pcre: The wrapper object containing the compiled pcre expression
 * - inputString: The subject string.
 * - startBytePos: The position from where the search should start given in bytes.
 * - codePointPos: The same position in terms of code points.
 * - isMatch: Boolean flag to mark if the caller function is $regexMatch, in which case the result
 * returned is true/false.
 */
FastTuple<bool, value::TypeTags, value::Value> pcreNextMatch(pcre::Regex* pcre,
                                                             StringData inputString,
                                                             uint32_t& startBytePos,
                                                             uint32_t& codePointPos,
                                                             bool isMatch) {
    pcre::MatchData m = pcre->matchView(inputString, {}, startBytePos);
    if (!m && m.error() != pcre::Errc::ERROR_NOMATCH) {
        LOGV2_ERROR(5073414,
                    "Error occurred while executing regular expression.",
                    "execResult"_attr = redact(errorMessage(m.error())));
        return {false, value::TypeTags::Nothing, 0};
    }

    if (isMatch) {
        // $regexMatch returns true or false.
        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(!!m)};
    }
    // $regexFind and $regexFindAll build result object or return null.
    if (!m) {
        return {false, value::TypeTags::Null, 0};
    }

    // Create the result object {"match" : .., "idx" : ..., "captures" : ...}
    // from the pcre::MatchData.
    auto [matchedTag, matchedVal] = value::makeNewString(m[0]);
    value::ValueGuard matchedGuard{matchedTag, matchedVal};

    StringData precedesMatch = m.input().substr(m.startPos());
    precedesMatch = precedesMatch.substr(0, m[0].data() - precedesMatch.data());
    codePointPos += str::lengthInUTF8CodePoints(precedesMatch);
    startBytePos += precedesMatch.size();

    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto arrayView = value::getArrayView(arrVal);
    arrayView->reserve(m.captureCount());
    for (size_t i = 0; i < m.captureCount(); ++i) {
        StringData cap = m[i + 1];
        if (!cap.data()) {
            arrayView->push_back(value::TypeTags::Null, 0);
        } else {
            auto [tag, val] = value::makeNewString(cap);
            arrayView->push_back(tag, val);
        }
    }

    auto [resTag, resVal] = value::makeNewObject();
    value::ValueGuard resGuard{resTag, resVal};
    auto resObjectView = value::getObjectView(resVal);
    resObjectView->reserve(3);
    matchedGuard.reset();
    resObjectView->push_back("match", matchedTag, matchedVal);
    resObjectView->push_back(
        "idx", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(codePointPos));
    arrGuard.reset();
    resObjectView->push_back("captures", arrTag, arrVal);
    resGuard.reset();
    return {true, resTag, resVal};
}

/**
 * A helper function with common logic for $regexMatch and $regexFind functions. Both extract only
 * the first match to a regular expression, but return different result objects.
 */
FastTuple<bool, value::TypeTags, value::Value> genericPcreRegexSingleMatch(
    value::TypeTags typeTagPcreRegex,
    value::Value valuePcreRegex,
    value::TypeTags typeTagInputStr,
    value::Value valueInputStr,
    bool isMatch) {
    if (!value::isStringOrSymbol(typeTagInputStr) || !value::isPcreRegex(typeTagPcreRegex)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto inputString = value::getStringOrSymbolView(typeTagInputStr, valueInputStr);
    auto pcreRegex = value::getPcreRegexView(valuePcreRegex);

    uint32_t startBytePos = 0;
    uint32_t codePointPos = 0;
    return pcreNextMatch(pcreRegex, inputString, startBytePos, codePointPos, isMatch);
}

}  // namespace

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexCompile(ArityType arity) {
    invariant(arity == 2);

    auto [patternOwned, patternTypeTag, patternValue] = getFromStack(0);
    auto [optionsOwned, optionsTypeTag, optionsValue] = getFromStack(1);

    if (!value::isString(patternTypeTag) || !value::isString(optionsTypeTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto pattern = value::getStringView(patternTypeTag, patternValue);
    auto options = value::getStringView(optionsTypeTag, optionsValue);

    if (pattern.find('\0', 0) != std::string::npos || options.find('\0', 0) != std::string::npos) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto [pcreTag, pcreValue] = makeNewPcreRegex(pattern, options);
    return {true, pcreTag, pcreValue};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexMatch(ArityType arity) {
    invariant(arity == 2);
    auto [ownedPcreRegex, tagPcreRegex, valPcreRegex] = getFromStack(0);
    auto [ownedInputStr, tagInputStr, valInputStr] = getFromStack(1);

    if (value::isArray(tagPcreRegex)) {
        for (value::ArrayEnumerator ae(tagPcreRegex, valPcreRegex); !ae.atEnd(); ae.advance()) {
            auto [elemTag, elemVal] = ae.getViewOfValue();
            auto [ownedResult, tagResult, valResult] =
                genericPcreRegexSingleMatch(elemTag, elemVal, tagInputStr, valInputStr, true);

            if (tagResult == value::TypeTags::Boolean && value::bitcastTo<bool>(valResult)) {
                return {ownedResult, tagResult, valResult};
            }

            if (ownedResult) {
                value::releaseValue(tagResult, valResult);
            }
        }

        return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(false)};
    }

    return genericPcreRegexSingleMatch(tagPcreRegex, valPcreRegex, tagInputStr, valInputStr, true);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexFind(ArityType arity) {
    invariant(arity == 2);
    auto [ownedPcreRegex, typeTagPcreRegex, valuePcreRegex] = getFromStack(0);
    auto [ownedInputStr, typeTagInputStr, valueInputStr] = getFromStack(1);

    return genericPcreRegexSingleMatch(
        typeTagPcreRegex, valuePcreRegex, typeTagInputStr, valueInputStr, false);
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRegexFindAll(ArityType arity) {
    invariant(arity == 2);
    auto [ownedPcre, typeTagPcreRegex, valuePcreRegex] = getFromStack(0);
    auto [ownedStr, typeTagInputStr, valueInputStr] = getFromStack(1);

    if (!value::isString(typeTagInputStr) || typeTagPcreRegex != value::TypeTags::pcreRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto inputString = value::getStringView(typeTagInputStr, valueInputStr);
    auto pcre = value::getPcreRegexView(valuePcreRegex);

    uint32_t startBytePos = 0;
    uint32_t codePointPos = 0;

    // Prepare the result array of matching objects.
    auto [arrTag, arrVal] = value::makeNewArray();
    value::ValueGuard arrGuard{arrTag, arrVal};
    auto arrayView = value::getArrayView(arrVal);

    int resultSize = 0;
    do {
        auto [_, matchTag, matchVal] =
            pcreNextMatch(pcre, inputString, startBytePos, codePointPos, false);
        value::ValueGuard matchGuard{matchTag, matchVal};

        if (matchTag == value::TypeTags::Null) {
            break;
        }
        if (matchTag != value::TypeTags::Object) {
            return {false, value::TypeTags::Nothing, 0};
        }

        resultSize += getApproximateSize(matchTag, matchVal);
        uassert(5126606,
                "$regexFindAll: the size of buffer to store output exceeded the 64MB limit",
                resultSize <= mongo::BufferMaxSize);

        matchGuard.reset();
        arrayView->push_back(matchTag, matchVal);

        // Move indexes after the current matched string to prepare for the next search.
        auto [mstrTag, mstrVal] = value::getObjectView(matchVal)->getField("match");
        auto matchString = value::getStringView(mstrTag, mstrVal);
        if (matchString.empty()) {
            startBytePos += str::getCodePointLength(inputString[startBytePos]);
            ++codePointPos;
        } else {
            startBytePos += matchString.size();
            for (size_t byteIdx = 0; byteIdx < matchString.size(); ++codePointPos) {
                byteIdx += str::getCodePointLength(matchString[byteIdx]);
            }
        }
    } while (startBytePos < inputString.size());

    arrGuard.reset();
    return {true, arrTag, arrVal};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetRegexPattern(ArityType arity) {
    invariant(arity == 1);
    auto [regexOwned, regexType, regexValue] = getFromStack(0);

    if (regexType != value::TypeTags::bsonRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto regex = value::getBsonRegexView(regexValue);
    auto [strType, strValue] = value::makeNewString(regex.pattern);

    return {true, strType, strValue};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinGetRegexFlags(ArityType arity) {
    invariant(arity == 1);
    auto [regexOwned, regexType, regexValue] = getFromStack(0);

    if (regexType != value::TypeTags::bsonRegex) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto regex = value::getBsonRegexView(regexValue);
    auto [strType, strValue] = value::makeNewString(regex.flags);

    return {true, strType, strValue};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
