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

#include <boost/algorithm/string/case_conv.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"

namespace mongo {

namespace exec::expression {

namespace {

/**
 * Object to hold data that is required when evaluating Regex expressions.
 */
struct RegexExecutionState {
    /**
     * The regex pattern, options, and captures buffer for the current execution context.
     */
    boost::optional<std::string> pattern;
    boost::optional<std::string> options;
    std::vector<int> capturesBuffer;
    int numCaptures = 0;

    /**
     * If 'regex' is constant, 'pcrePtr' will be shared between the active RegexExecutionState
     * and '_precompiledRegex'. If not, then the active RegexExecutionState is
     * the sole owner.
     */
    std::shared_ptr<pcre::Regex> pcrePtr;

    /**
     * The input text and starting position for the current execution context.
     */
    boost::optional<std::string> input;
    int startCodePointPos = 0;
    int startBytePos = 0;

    /**
     * If either the text input or regex pattern is nullish, then we consider the operation as a
     * whole nullish.
     */
    bool nullish() {
        return !input || !pattern;
    }
};

void extractInputField(const Value& textInput,
                       const std::string& opName,
                       boost::optional<std::string>& extractedInput) {
    uassert(51104,
            str::stream() << opName << " needs 'input' to be of type string",
            textInput.nullish() || textInput.getType() == BSONType::String);
    if (textInput.getType() == BSONType::String) {
        extractedInput = textInput.getString();
    }
}

void extractRegexAndOptions(const Value& regexPattern,
                            const Value& regexOptions,
                            const std::string& opName,
                            boost::optional<std::string>& extractedPattern,
                            boost::optional<std::string>& extractedOptions) {
    uassert(51105,
            str::stream() << opName << " needs 'regex' to be of type string or regex",
            regexPattern.nullish() || regexPattern.getType() == BSONType::String ||
                regexPattern.getType() == BSONType::RegEx);
    uassert(51106,
            str::stream() << opName << " needs 'options' to be of type string",
            regexOptions.nullish() || regexOptions.getType() == BSONType::String);

    // The 'regex' field can be a RegEx object and may have its own options...
    if (regexPattern.getType() == BSONType::RegEx) {
        StringData regexFlags = regexPattern.getRegexFlags();
        extractedPattern = regexPattern.getRegex();
        uassert(51107,
                str::stream()
                    << opName
                    << ": found regex option(s) specified in both 'regex' and 'option' fields",
                regexOptions.nullish() || regexFlags.empty());
        if (!regexFlags.empty()) {
            extractedOptions = regexFlags.toString();
        }
    } else if (regexPattern.getType() == BSONType::String) {
        // ...or it can be a string field with options specified separately.
        extractedPattern = regexPattern.getString();
    }

    // If 'options' is non-null, we must validate its contents even if 'regexPattern' is nullish.
    if (!regexOptions.nullish()) {
        extractedOptions = regexOptions.getString();
    }
    uassert(51109,
            str::stream() << opName << ": regular expression cannot contain an embedded null byte",
            !extractedPattern || extractedPattern->find('\0', 0) == std::string::npos);

    uassert(51110,
            str::stream() << opName
                          << ": regular expression options cannot contain an embedded null byte",
            !extractedOptions || extractedOptions->find('\0', 0) == std::string::npos);
}

void compile(const boost::optional<std::string>& pattern,
             const boost::optional<std::string>& options,
             const std::string& opName,
             std::shared_ptr<pcre::Regex>& compiledPcrePtr,
             int& numCaptures) {
    if (!pattern) {
        return;
    }

    auto re = std::make_shared<pcre::Regex>(
        *pattern, pcre_util::flagsToOptions(options.value_or(""), opName));
    uassert(51111,
            str::stream() << "Invalid Regex in " << opName << ": " << errorMessage(re->error()),
            *re);
    compiledPcrePtr = std::move(re);

    // Calculate the number of capture groups present in 'pattern' and store in 'numCaptures'.
    numCaptures = compiledPcrePtr->captureCount();
}

/**
 * Validates the structure of input passed in 'expr'. If valid, generates an initial
 * execution state. This returned object can later be used for calling execute() or nextMatch().
 */
template <typename RegexExpression>
RegexExecutionState buildInitialState(const RegexExpression& expr,
                                      const Document& root,
                                      Variables* variables) {
    Value textInput = expr.getInput()->evaluate(root, variables);
    Value regexPattern = expr.getRegex()->evaluate(root, variables);
    Value regexOptions =
        expr.getOptions() ? expr.getOptions()->evaluate(root, variables) : Value(BSONNULL);

    RegexExecutionState executionState;

    if (expr.hasConstantRegex()) {
        // If we have a prebuilt execution state, then the 'regex' and 'options' fields are constant
        // values, and we do not need to revalidate and re-compile them.
        auto& compiledRegex = expr.getPreCompiledRegex();
        executionState.pcrePtr = compiledRegex->pcrePtr;
        executionState.pattern = compiledRegex->pattern;
        executionState.options = compiledRegex->options;
        executionState.numCaptures = compiledRegex->numCaptures;
    } else {
        extractRegexAndOptions(regexPattern,
                               regexOptions,
                               expr.getOpName(),
                               executionState.pattern,
                               executionState.options);
        compile(executionState.pattern,
                executionState.options,
                expr.getOpName(),
                executionState.pcrePtr,
                executionState.numCaptures);
    }

    // The 'input' parameter can be a variable and needs to be extracted from the expression
    // document even when hasConstantRegex() returns true.
    extractInputField(textInput, expr.getOpName(), executionState.input);

    return executionState;
}

/**
 * Checks if there is a match for the input, options, and pattern of 'regexState'.
 * Returns the pcre::MatchData yielded by that match operation.
 * Will uassert for any errors other than `pcre::Errc::ERROR_NOMATCH`.
 */
pcre::MatchData execute(RegexExecutionState* regexState, const std::string& opName) {
    invariant(regexState);
    invariant(!regexState->nullish());
    invariant(regexState->pcrePtr);

    StringData in = *regexState->input;
    auto m = regexState->pcrePtr->matchView(in, {}, regexState->startBytePos);
    uassert(51156,
            str::stream() << "Error occurred while executing the regular expression in " << opName
                          << ". Result code: " << errorMessage(m.error()),
            m || m.error() == pcre::Errc::ERROR_NOMATCH);
    return m;
}


/**
 * Finds the next possible match for the given input and pattern that are part of
 * 'regexState'. If there is a match, the function will return a 'Value' object
 * encapsulating the matched string, the code point index of the matched string and a vector
 * representing all the captured substrings. The function will also update the parameters
 * 'startBytePos' and 'startCodePointPos' to the corresponding new indices. If there is no
 * match, the function will return null 'Value' object.
 */
Value nextMatch(RegexExecutionState* regexState, const std::string& opName) {
    auto m = execute(regexState, opName);
    if (!m) {
        // No match.
        return Value(BSONNULL);
    }

    auto afterStart = m.input().substr(m.startPos());
    auto beforeMatch = afterStart.substr(0, m[0].data() - afterStart.data());
    regexState->startCodePointPos += str::lengthInUTF8CodePoints(beforeMatch);

    // Set the start index for match to the new one.
    regexState->startBytePos = m[0].data() - m.input().data();

    std::vector<Value> captures;
    captures.reserve(m.captureCount());

    for (size_t i = 1; i < m.captureCount() + 1; ++i) {
        if (StringData cap = m[i]; !cap.rawData()) {
            // Use BSONNULL placeholder for unmatched capture groups.
            captures.push_back(Value(BSONNULL));
        } else {
            captures.push_back(Value(cap));
        }
    }

    MutableDocument match;
    match.addField("match", Value(m[0]));
    match.addField("idx", Value(regexState->startCodePointPos));
    match.addField("captures", Value(std::move(captures)));
    return match.freezeToValue();
}

}  // namespace

ExpressionRegex::PrecompiledRegex precompileRegex(const Value& regex,
                                                  const Value& options,
                                                  const std::string& opName) {
    ExpressionRegex::PrecompiledRegex precompiledRegex;
    extractRegexAndOptions(
        regex, options, opName, precompiledRegex.pattern, precompiledRegex.options);
    compile(precompiledRegex.pattern,
            precompiledRegex.options,
            opName,
            precompiledRegex.pcrePtr,
            precompiledRegex.numCaptures);
    return precompiledRegex;
}

Value evaluate(const ExpressionRegexFind& expr, const Document& root, Variables* variables) {
    auto executionState = buildInitialState(expr, root, variables);
    if (executionState.nullish()) {
        return Value(BSONNULL);
    }
    return nextMatch(&executionState, expr.getOpName());
}

Value evaluate(const ExpressionRegexFindAll& expr, const Document& root, Variables* variables) {
    std::vector<Value> output;
    auto executionState = buildInitialState(expr, root, variables);
    if (executionState.nullish()) {
        return Value(std::move(output));
    }
    StringData input = *(executionState.input);
    size_t totalDocSize = 0;

    // Using do...while loop because, when input is an empty string, we still want to see if there
    // is a match.
    do {
        auto matchObj = nextMatch(&executionState, expr.getOpName());
        if (matchObj.getType() == BSONType::jstNULL) {
            break;
        }
        totalDocSize += matchObj.getApproximateSize();
        uassert(51151,
                str::stream() << expr.getOpName()
                              << ": the size of buffer to store output exceeded the 64MB limit",
                totalDocSize <= mongo::BufferMaxSize);

        output.push_back(matchObj);
        std::string matchStr = matchObj.getDocument().getField("match").getString();
        if (matchStr.empty()) {
            // This would only happen if the regex matched an empty string. In this case, even if
            // the character at startByteIndex matches the regex, we cannot return it since we are
            // already returing an empty string starting at this index. So we move on to the next
            // byte index.
            if (static_cast<size_t>(executionState.startBytePos) >= input.size()) {
                continue;  // input already exhausted
            }
            executionState.startBytePos +=
                str::getCodePointLength(input[executionState.startBytePos]);
            ++executionState.startCodePointPos;
            continue;
        }

        // We don't want any overlapping sub-strings. So we move 'startBytePos' to point to the
        // byte after 'matchStr'. We move the code point index also correspondingly.
        executionState.startBytePos += matchStr.size();
        for (size_t byteIx = 0; byteIx < matchStr.size(); ++executionState.startCodePointPos) {
            byteIx += str::getCodePointLength(matchStr[byteIx]);
        }

        invariant(executionState.startBytePos > 0);
        invariant(executionState.startCodePointPos > 0);
        invariant(executionState.startCodePointPos <= executionState.startBytePos);
    } while (static_cast<size_t>(executionState.startBytePos) < input.size());
    return Value(std::move(output));
}

Value evaluate(const ExpressionRegexMatch& expr, const Document& root, Variables* variables) {
    auto state = buildInitialState(expr, root, variables);
    if (state.nullish()) {
        return Value(false);
    }
    pcre::MatchData m = execute(&state, expr.getOpName());
    return Value(!!m);
}

}  // namespace exec::expression
}  // namespace mongo
