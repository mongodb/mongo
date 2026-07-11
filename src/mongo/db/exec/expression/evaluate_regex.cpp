// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/expression/evaluate.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/util/pcre.h"
#include "mongo/util/pcre_util.h"

#include <string_view>


namespace mongo {

namespace exec::expression {

namespace {
using namespace std::literals::string_view_literals;

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
     * The starting position in the input text that has not yet been split using match. This index
     * can be different from startBytePos.
     */
    int beforeMatchStrStart = 0;

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
            textInput.nullish() || textInput.getType() == BSONType::string);
    if (textInput.getType() == BSONType::string) {
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
            regexPattern.nullish() || regexPattern.getType() == BSONType::string ||
                regexPattern.getType() == BSONType::regEx);
    uassert(51106,
            str::stream() << opName << " needs 'options' to be of type string",
            regexOptions.nullish() || regexOptions.getType() == BSONType::string);

    // The 'regex' field can be a RegEx object and may have its own options...
    if (regexPattern.getType() == BSONType::regEx) {
        std::string_view regexFlags = regexPattern.getRegexFlags();
        extractedPattern = regexPattern.getRegex();
        uassert(51107,
                str::stream()
                    << opName
                    << ": found regex option(s) specified in both 'regex' and 'option' fields",
                regexOptions.nullish() || regexFlags.empty());
        if (!regexFlags.empty()) {
            extractedOptions = std::string{regexFlags};
        }
    } else if (regexPattern.getType() == BSONType::string) {
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
        *pattern,
        pcre_util::flagsToOptions(options.value_or(""), opName),
        pcre::Limits{
            .heapLimitKB = static_cast<uint32_t>(internalQueryRegexHeapLimitKB.loadRelaxed()),
            .matchLimit = static_cast<uint32_t>(internalQueryRegexMatchLimit.loadRelaxed())});
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
                                      Variables* variables,
                                      const EvaluationContext& ctx) {
    Value textInput = expr.getInput()->evaluate(root, variables, ctx);
    Value regexPattern = expr.getRegex()->evaluate(root, variables, ctx);
    Value regexOptions =
        expr.getOptions() ? expr.getOptions()->evaluate(root, variables, ctx) : Value(BSONNULL);

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
 * Creates an initial execution state from an input string value and regular expression pattern
 * value. This returned object can later be used for calling execute() or nextMatch().
 */
RegexExecutionState buildInitialState(const std::string& opName,
                                      const Value& textInput,
                                      const Value& regexPattern) {
    RegexExecutionState executionState;

    extractRegexAndOptions(
        regexPattern, Value(BSONNULL), opName, executionState.pattern, executionState.options);
    compile(executionState.pattern,
            executionState.options,
            opName,
            executionState.pcrePtr,
            executionState.numCaptures);
    extractInputField(textInput, opName, executionState.input);

    return executionState;
}

/**
 * Checks if there is a match for the input, options, and pattern of 'regexState'.
 * Returns the pcre::MatchData yielded by that match operation.
 * Will uassert for any errors other than `pcre::Errc::ERROR_NOMATCH`.
 */
pcre::MatchData execute(RegexExecutionState* regexState, const std::string& opName) {
    tassert(11103508, "Expected non-null regexState", regexState);
    tassert(11103509, "Expected non-nullish regexState", !regexState->nullish());
    tassert(11103510, "Expected regexState to contain a valid pcrePtr", regexState->pcrePtr);

    std::string_view in = *regexState->input;
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
        if (std::string_view cap = m[i]; !cap.data()) {
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

/**
 * Finds the next possible match for the given input and pattern that are part of
 * 'regexState'. This function returns the matched MatchData object and the input substring
 * preceding the match.
 */
std::pair<pcre::MatchData, std::string_view> nextMatchAndPrecedingString(
    RegexExecutionState* regexState, const std::string& opName) {
    auto m = execute(regexState, opName);
    if (!m) {
        // No match.
        return {std::move(m), ""sv};
    }

    const int matchPos = m[0].data() - m.input().data();

    const std::string_view beforeMatch = m.input().substr(
        regexState->beforeMatchStrStart, matchPos - regexState->beforeMatchStrStart);

    // Move indices for next match.
    regexState->beforeMatchStrStart += beforeMatch.size() + m[0].size();
    if (m[0].empty()) {
        // Move the startBytePos index to the next character, so we don't capture the same empty
        // match in the next iteration. To accommodate unicode characters, we need the length of the
        // current character to move, but this can only be done if the index points to a valid
        // character (i.e. startBytePos < m.input().size()), if that's not the case we just increase
        // by one because we will exit in the next iteration.
        regexState->startBytePos = static_cast<size_t>(regexState->startBytePos) < m.input().size()
            ? matchPos + str::getCodePointLength(m.input()[regexState->startBytePos])
            : m.input().size() + 1;
    } else {
        regexState->startBytePos = regexState->beforeMatchStrStart;
    }
    return {std::move(m), beforeMatch};
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

Value evaluate(const ExpressionRegexFind& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto executionState = buildInitialState(expr, root, variables, ctx);
    if (executionState.nullish()) {
        return Value(BSONNULL);
    }
    return nextMatch(&executionState, expr.getOpName());
}

Value evaluate(const ExpressionRegexFindAll& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    std::vector<Value> output;
    auto executionState = buildInitialState(expr, root, variables, ctx);
    if (executionState.nullish()) {
        return Value(std::move(output));
    }
    std::string_view input = *(executionState.input);
    size_t totalDocSize = 0;

    // Using do...while loop because, when input is an empty string, we still want to see if there
    // is a match.
    do {
        auto matchObj = nextMatch(&executionState, expr.getOpName());
        if (matchObj.getType() == BSONType::null) {
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

Value evaluate(const ExpressionRegexMatch& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto state = buildInitialState(expr, root, variables, ctx);
    if (state.nullish()) {
        return Value(false);
    }
    pcre::MatchData m = execute(&state, expr.getOpName());
    return Value(!!m);
}

namespace {
void validateSplitArguments(const ExpressionSplit& expr,
                            const Value& inputArg,
                            const Value& separatorArg) {
    uassert(40085,
            str::stream() << "$split requires an expression that evaluates to a string as a first "
                             "argument, found: "
                          << typeName(inputArg.getType()),
            inputArg.getType() == BSONType::string);

    if (expr.getExpressionContext()->isFeatureFlagMqlJsEngineGapEnabled()) {
        uassert(40086,
                str::stream() << "$split requires an expression that evaluates to a string or "
                                 "regular expression as a second argument, found: "
                              << typeName(separatorArg.getType()),
                separatorArg.getType() == BSONType::string ||
                    separatorArg.getType() == BSONType::regEx);
    } else {
        uassert(
            10503900,
            str::stream() << "$split requires an expression that evaluates to a string as a second "
                             "argument, found: "
                          << typeName(separatorArg.getType()),
            separatorArg.getType() == BSONType::string);
    }
}
}  // namespace

Value evaluate(const ExpressionSplit& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto& children = expr.getChildren();
    Value inputArg = children[0]->evaluate(root, variables, ctx);
    Value separatorArg = children[1]->evaluate(root, variables, ctx);

    if (inputArg.nullish() || separatorArg.nullish()) {
        return Value(BSONNULL);
    }
    validateSplitArguments(expr, inputArg, separatorArg);

    std::vector<Value> output;

    if (separatorArg.getType() == BSONType::string) {
        // Split using a string delimiter.
        std::string_view input = inputArg.getStringData();
        std::string_view separator = separatorArg.getStringData();

        uassert(40087, "$split requires a non-empty separator", !separator.empty());

        const char* needle = separator.data();
        const char* const needleEnd = needle + separator.size();
        const char* remainingHaystack = input.data();
        const char* const haystackEnd = remainingHaystack + input.size();

        const char* it = remainingHaystack;
        while ((it = std::search(remainingHaystack, haystackEnd, needle, needleEnd)) !=
               haystackEnd) {
            std::string_view sd(remainingHaystack, it - remainingHaystack);
            output.push_back(Value(sd));
            remainingHaystack = it + separator.size();
        }

        std::string_view splitString(remainingHaystack,
                                     input.size() - (remainingHaystack - input.data()));
        output.push_back(Value(splitString));
        return Value(std::move(output));
    }

    tassert(10503905,
            str::stream() << "Expected separator to be a regular expression, but found: "
                          << typeName(separatorArg.getType()),
            separatorArg.getType() == BSONType::regEx);
    tassert(10503906,
            str::stream() << "featureFlagMqlJsEngineGap is not enabled, but $split accepted a "
                             "regular expression as separator.",
            expr.getExpressionContext()->isFeatureFlagMqlJsEngineGapEnabled());
    // Split using a regular expression delimiter.
    auto executionState = buildInitialState(expr.getOpName(), inputArg, separatorArg);
    if (executionState.nullish()) {
        return Value(std::move(output));
    }

    auto input = inputArg.getStringData();

    // Condition uses <= instead of < to also capture possible empty matches at the end of the
    // search string.
    while (static_cast<size_t>(executionState.startBytePos) <= input.size()) {
        auto [match, beforeMatch] = nextMatchAndPrecedingString(&executionState, expr.getOpName());
        if (!match) {
            // No match.
            break;
        }
        output.push_back(Value(beforeMatch));
        for (std::string_view subMatch : match.getCaptures()) {
            output.push_back(Value(subMatch));
        }
    }
    output.push_back(Value(input.substr(executionState.beforeMatchStrStart)));
    return Value(std::move(output));
}

namespace {
template <typename ExpressionReplace>
Value evaluateReplace(
    ExpressionReplace& expr,
    const Document& root,
    Variables* variables,
    const EvaluationContext& ctx,
    std::function<Value(std::string_view, std::string_view, std::string_view)> replaceOpStr,
    std::function<Value(std::string_view, RegexExecutionState, std::string_view)> replaceOpRegEx) {
    Value input = expr.getInput()->evaluate(root, variables, ctx);
    Value find = expr.getFind()->evaluate(root, variables, ctx);
    Value replacement = expr.getReplacement()->evaluate(root, variables, ctx);

    uassert(10503904,
            str::stream() << expr.getOpName()
                          << " requires that 'input' be a string, found: " << input.toString(),
            input.getType() == BSONType::string || input.nullish());
    if (expr.getExpressionContext()->isFeatureFlagMqlJsEngineGapEnabled()) {
        uassert(10503903,
                str::stream() << expr.getOpName()
                              << " requires that 'find' be a string or regular expression, found: "
                              << find.toString(),
                find.getType() == BSONType::string || find.getType() == BSONType::regEx ||
                    find.nullish());
    } else {
        uassert(10503901,
                str::stream() << expr.getOpName()
                              << " requires that 'find' be a string, found: " << find.toString(),
                find.getType() == BSONType::string || find.nullish());
    }
    uassert(10503902,
            str::stream() << expr.getOpName() << " requires that 'replacement' be a string, found: "
                          << replacement.toString(),
            replacement.getType() == BSONType::string || replacement.nullish());

    // Return null if any arg is nullish.
    if (input.nullish() || find.nullish() || replacement.nullish()) {
        return Value(BSONNULL);
    }

    if (find.getType() == BSONType::regEx) {
        tassert(
            10503907,
            str::stream() << "Find argument is a regular expression, but replaceOpRegEx is null.",
            replaceOpRegEx != nullptr);
        tassert(10503908,
                str::stream() << "featureFlagMqlJsEngineGap is not enabled, but "
                              << expr.getOpName() << " accepted a regular expression as separator.",
                expr.getExpressionContext()->isFeatureFlagMqlJsEngineGapEnabled());
        auto executionState = buildInitialState(expr.getOpName(), input, find);
        if (executionState.nullish()) {
            return Value(std::move(input));
        }
        return replaceOpRegEx(
            input.getStringData(), std::move(executionState), replacement.getStringData());
    }
    // find.getType() == BSONType::string
    return replaceOpStr(input.getStringData(), find.getStringData(), replacement.getStringData());
}

void trackReplaceOutput(const EvaluationContext& ctx,
                        SimpleMemoryUsageToken& memToken,
                        int64_t currentLength,
                        std::string_view opName) {
    memToken.set(currentLength);
    memToken.tracker()->assertWithinMemoryLimit(opName, ctx.stageName);
}
}  // namespace

Value evaluate(const ExpressionReplaceOne& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto replaceOneOp =
        [](std::string_view input, std::string_view find, std::string_view replacement) -> Value {
        size_t startIndex = input.find(find);
        if (startIndex == std::string::npos) {
            return Value(std::string_view(input));
        }
        // An empty string matches at every position, so replaceOne should insert the replacement
        // text at position 0. input.find correctly returns position 0 when 'find' is empty, so we
        // don't need any special case to handle this.
        size_t endIndex = startIndex + find.size();
        StringBuilder output;
        output << input.substr(0, startIndex);
        output << replacement;
        output << input.substr(endIndex);
        return Value(output.stringData());
    };
    auto replaceOneOpRegEx = [&](std::string_view input,
                                 RegexExecutionState executionState,
                                 std::string_view replacement) -> Value {
        auto [match, beforeMatch] = nextMatchAndPrecedingString(&executionState, expr.getOpName());
        if (!match) {
            // No match.
            return Value(input);
        }
        StringBuilder output;
        output << beforeMatch << replacement << input.substr(executionState.beforeMatchStrStart);
        return Value(output.stringData());
    };
    return evaluateReplace(expr, root, variables, ctx, replaceOneOp, replaceOneOpRegEx);
}

Value evaluate(const ExpressionReplaceAll& expr,
               const Document& root,
               Variables* variables,
               const EvaluationContext& ctx) {
    auto replaceAllOpStr =
        [&](std::string_view input, std::string_view find, std::string_view replacement) -> Value {
        auto& tracker = getMemoryTracker(expr, ctx);
        SimpleMemoryUsageToken memToken{0, &tracker};

        // An empty string matches at every position, so replaceAll should insert 'replacement'
        // at every position when 'find' is empty. Handling this as a special case lets us
        // assume 'find' is nonempty in the usual case.
        if (find.size() == 0) {
            const int64_t inputSize = static_cast<int64_t>(input.size());
            const int64_t replacementSize = static_cast<int64_t>(replacement.size());
            const int64_t expectedSize = (inputSize + 1) * replacementSize + inputSize;
            trackReplaceOutput(ctx, memToken, expectedSize, expr.getOpName());

            StringBuilder output;
            for (char c : input) {
                output << replacement << c;
            }
            output << replacement;
            return Value(output.stringData());
        }

        StringBuilder output;
        for (;;) {
            size_t startIndex = input.find(find);
            if (startIndex == std::string::npos) {
                output << input;
                break;
            }

            size_t endIndex = startIndex + find.size();
            output << input.substr(0, startIndex);
            output << replacement;
            trackReplaceOutput(ctx, memToken, output.len(), expr.getOpName());
            // This step assumes 'find' is nonempty. If 'find' were empty then input.find would
            // always find a match at position 0, and the input would never shrink.
            input = input.substr(endIndex);
        }
        trackReplaceOutput(ctx, memToken, output.len(), expr.getOpName());
        return Value(output.stringData());
    };
    auto replaceAllOpRegEx = [&](std::string_view input,
                                 RegexExecutionState executionState,
                                 std::string_view replacement) -> Value {
        auto& tracker = getMemoryTracker(expr, ctx);
        SimpleMemoryUsageToken memToken{0, &tracker};
        StringBuilder output;

        // Condition uses <= instead of < to also capture possible empty matches at the end of the
        // search string.
        while (static_cast<size_t>(executionState.startBytePos) <= input.size()) {
            auto [match, beforeMatch] =
                nextMatchAndPrecedingString(&executionState, expr.getOpName());
            if (!match) {
                // No match.
                break;
            }
            output << beforeMatch << replacement;
            trackReplaceOutput(ctx, memToken, output.len(), expr.getOpName());
        }
        output << input.substr(executionState.beforeMatchStrStart);
        trackReplaceOutput(ctx, memToken, output.len(), expr.getOpName());

        return Value(output.stringData());
    };
    return evaluateReplace(expr, root, variables, ctx, replaceAllOpStr, replaceAllOpRegEx);
}

}  // namespace exec::expression
}  // namespace mongo
