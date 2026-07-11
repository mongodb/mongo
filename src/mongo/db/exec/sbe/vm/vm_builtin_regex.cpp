// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/util/pcre.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#include <string_view>

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
value::TagValueMaybeOwned pcreNextMatch(pcre::Regex* pcre,
                                        std::string_view inputString,
                                        uint32_t& startBytePos,
                                        uint32_t& codePointPos,
                                        bool isMatch) {
    pcre::MatchData m = pcre->matchView(inputString, {}, startBytePos);
    if (!m && m.error() != pcre::Errc::ERROR_NOMATCH) {
        LOGV2_ERROR(5073414,
                    "Error occurred while executing regular expression.",
                    "execResult"_attr = redact(errorMessage(m.error())));
        return value::TagValueMaybeOwned::nothing();
    }

    if (isMatch) {
        // $regexMatch returns true or false.
        return value::TagValueMaybeOwned::boolean(!!m);
    }
    // $regexFind and $regexFindAll build result object or return null.
    if (!m) {
        return value::TagValueMaybeOwned::null();
    }

    // Create the result object {"match" : .., "idx" : ..., "captures" : ...}
    // from the pcre::MatchData.
    auto matched = value::TagValueOwned::fromRaw(value::makeNewString(m[0]));

    std::string_view precedesMatch = m.input().substr(m.startPos());
    precedesMatch = precedesMatch.substr(0, m[0].data() - precedesMatch.data());
    codePointPos += str::lengthInUTF8CodePoints(precedesMatch);
    startBytePos += precedesMatch.size();

    auto arr = value::TagValueOwned::fromRaw(value::makeNewArray());
    auto arrayView = value::getArrayView(arr.value());
    arrayView->reserve(m.captureCount());
    for (size_t i = 0; i < m.captureCount(); ++i) {
        std::string_view cap = m[i + 1];
        if (!cap.data()) {
            arrayView->push_back_raw(value::TypeTags::Null, 0);
        } else {
            auto [tag, val] = value::makeNewString(cap);
            arrayView->push_back_raw(tag, val);
        }
    }

    auto res = value::TagValueOwned::fromRaw(value::makeNewObject());
    auto resObjectView = value::getObjectView(res.value());
    resObjectView->reserve(3);
    auto [matchedTag, matchedVal] = matched.releaseToRaw();
    resObjectView->push_back_raw("match", matchedTag, matchedVal);
    resObjectView->push_back_raw(
        "idx", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(codePointPos));
    auto [arrTag, arrVal] = arr.releaseToRaw();
    resObjectView->push_back_raw("captures", arrTag, arrVal);
    return std::move(res);
}

/**
 * A helper function with common logic for $regexMatch and $regexFind functions. Both extract only
 * the first match to a regular expression, but return different result objects.
 */
value::TagValueMaybeOwned genericPcreRegexSingleMatch(value::TypeTags typeTagPcreRegex,
                                                      value::Value valuePcreRegex,
                                                      value::TypeTags typeTagInputStr,
                                                      value::Value valueInputStr,
                                                      bool isMatch) {
    if (!value::isStringOrSymbol(typeTagInputStr) || !value::isPcreRegex(typeTagPcreRegex)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto inputString = value::getStringOrSymbolView(typeTagInputStr, valueInputStr);
    auto pcreRegex = value::getPcreRegexView(valuePcreRegex);

    uint32_t startBytePos = 0;
    uint32_t codePointPos = 0;
    return pcreNextMatch(pcreRegex, inputString, startBytePos, codePointPos, isMatch);
}

}  // namespace

value::TagValueMaybeOwned ByteCode::builtinRegexCompile(ArityType arity) {
    tassert(11080022, "Unexpected arity value", arity == 2);

    auto patternView = viewFromStack(0);
    auto optionsView = viewFromStack(1);

    if (!value::isString(patternView.tag) || !value::isString(optionsView.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto pattern = value::getStringView(patternView.tag, patternView.value);
    auto options = value::getStringView(optionsView.tag, optionsView.value);

    if (pattern.find('\0', 0) != std::string::npos || options.find('\0', 0) != std::string::npos) {
        return value::TagValueMaybeOwned::nothing();
    }

    return makeNewPcreRegex(pattern, options);
}

value::TagValueMaybeOwned ByteCode::builtinRegexMatch(ArityType arity) {
    tassert(11080021, "Unexpected arity value", arity == 2);
    auto pcreRegexView = viewFromStack(0);
    auto inputStrView = viewFromStack(1);

    if (value::isArray(pcreRegexView.tag)) {
        for (value::ArrayEnumerator ae(pcreRegexView.tag, pcreRegexView.value); !ae.atEnd();
             ae.advance()) {
            auto [elemTag, elemVal] = ae.getViewOfValue();
            auto result = genericPcreRegexSingleMatch(
                elemTag, elemVal, inputStrView.tag, inputStrView.value, true);

            if (result.tag() == value::TypeTags::Boolean &&
                value::bitcastTo<bool>(result.value())) {
                return result;
            }
        }

        return value::TagValueMaybeOwned::boolean(false);
    }

    return genericPcreRegexSingleMatch(
        pcreRegexView.tag, pcreRegexView.value, inputStrView.tag, inputStrView.value, true);
}

value::TagValueMaybeOwned ByteCode::builtinRegexFind(ArityType arity) {
    tassert(11080020, "Unexpected arity value", arity == 2);
    auto pcreRegexView = viewFromStack(0);
    auto inputStrView = viewFromStack(1);

    return genericPcreRegexSingleMatch(
        pcreRegexView.tag, pcreRegexView.value, inputStrView.tag, inputStrView.value, false);
}

value::TagValueMaybeOwned ByteCode::builtinRegexFindAll(ArityType arity) {
    tassert(11080019, "Unexpected arity value", arity == 2);
    auto pcreRegexView = viewFromStack(0);
    auto inputStrView = viewFromStack(1);

    if (!value::isString(inputStrView.tag) || pcreRegexView.tag != value::TypeTags::pcreRegex) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto inputString = value::getStringView(inputStrView.tag, inputStrView.value);
    auto pcre = value::getPcreRegexView(pcreRegexView.value);

    uint32_t startBytePos = 0;
    uint32_t codePointPos = 0;

    // Prepare the result array of matching objects.
    value::TagValueOwned arr{value::makeNewArray()};
    auto arrayView = value::getArrayView(arr.value());

    int resultSize = 0;
    do {
        auto match = pcreNextMatch(pcre, inputString, startBytePos, codePointPos, false);

        if (match.tag() == value::TypeTags::Null) {
            break;
        }
        if (match.tag() != value::TypeTags::Object) {
            return value::TagValueMaybeOwned::nothing();
        }

        resultSize += getApproximateSize(match.tag(), match.value());
        uassert(5126606,
                "$regexFindAll: the size of buffer to store output exceeded the 64MB limit",
                resultSize <= mongo::BufferMaxSize);

        auto [matchTag, matchVal] = match.raw();
        match.disown();
        arrayView->push_back_raw(matchTag, matchVal);

        // Move indexes after the current matched string to prepare for the next search.
        auto [mstrTag, mstrVal] = value::getObjectView(matchVal)->getField("match");
        auto matchString = value::getStringView(mstrTag, mstrVal);
        if (matchString.empty()) {
            // The regex matched an empty string. If the empty match landed at the end of the
            // input (e.g. pattern "$" or "a*" against ""), 'startBytePos' is already at
            // 'inputString.size()' and there is no byte to advance over. Break out so we do not
            // read past the end of the input.
            if (startBytePos >= inputString.size()) {
                break;
            }
            startBytePos += str::getCodePointLength(inputString[startBytePos]);
            ++codePointPos;
        } else {
            startBytePos += matchString.size();
            for (size_t byteIdx = 0; byteIdx < matchString.size(); ++codePointPos) {
                byteIdx += str::getCodePointLength(matchString[byteIdx]);
            }
        }
    } while (startBytePos < inputString.size());

    return std::move(arr);
}

value::TagValueMaybeOwned ByteCode::builtinGetRegexPattern(ArityType arity) {
    tassert(11080018, "Unexpected arity value", arity == 1);
    auto regexView = viewFromStack(0);

    if (regexView.tag != value::TypeTags::bsonRegex) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto regex = value::getBsonRegexView(regexView.value);
    auto [strType, strValue] = value::makeNewString(regex.pattern);

    return {true, strType, strValue};
}

value::TagValueMaybeOwned ByteCode::builtinGetRegexFlags(ArityType arity) {
    tassert(11080017, "Unexpected arity value", arity == 1);
    auto regexView = viewFromStack(0);

    if (regexView.tag != value::TypeTags::bsonRegex) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto regex = value::getBsonRegexView(regexView.value);
    auto [strType, strValue] = value::makeNewString(regex.flags);

    return {true, strType, strValue};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
