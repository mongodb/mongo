/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "mongo/base/string_data.h"

namespace mongo::pcre {

/*
 * Mongo's C++ wrapper for the PCRE2 library. Applies mongo-isms like
 * StringData.
 *
 * This wrapper is deliberately low-level and intended to be ignorant of mongo
 * server code's app-level preferences. It provides only a general-purpose PCRE2
 * wrapper.
 *
 * Care is taken to make this library self-contained through strict
 * encapsulation. No code depending on this library needs to include the pcre2
 * header or take on a dependency on the third_party/pcre2 library.
 */

/**
 * The complete list of PCRE2 errors, with the `PCRE2_` prefix stripped.
 * These are publicly available as members of the `Errc` enum class below.
 * E.g., `mongo::pcre::Errc::ERROR_BAD_SUBPATTERN_REFERENCE`.
 */
#define MONGO_PCRE_ERROR_EXPAND_TABLE_(X)        \
    X(ERROR_END_BACKSLASH)                       \
    X(ERROR_END_BACKSLASH_C)                     \
    X(ERROR_UNKNOWN_ESCAPE)                      \
    X(ERROR_QUANTIFIER_OUT_OF_ORDER)             \
    X(ERROR_QUANTIFIER_TOO_BIG)                  \
    X(ERROR_MISSING_SQUARE_BRACKET)              \
    X(ERROR_ESCAPE_INVALID_IN_CLASS)             \
    X(ERROR_CLASS_RANGE_ORDER)                   \
    X(ERROR_QUANTIFIER_INVALID)                  \
    X(ERROR_INTERNAL_UNEXPECTED_REPEAT)          \
    X(ERROR_INVALID_AFTER_PARENS_QUERY)          \
    X(ERROR_POSIX_CLASS_NOT_IN_CLASS)            \
    X(ERROR_POSIX_NO_SUPPORT_COLLATING)          \
    X(ERROR_MISSING_CLOSING_PARENTHESIS)         \
    X(ERROR_BAD_SUBPATTERN_REFERENCE)            \
    X(ERROR_NULL_PATTERN)                        \
    X(ERROR_BAD_OPTIONS)                         \
    X(ERROR_MISSING_COMMENT_CLOSING)             \
    X(ERROR_PARENTHESES_NEST_TOO_DEEP)           \
    X(ERROR_PATTERN_TOO_LARGE)                   \
    X(ERROR_HEAP_FAILED)                         \
    X(ERROR_UNMATCHED_CLOSING_PARENTHESIS)       \
    X(ERROR_INTERNAL_CODE_OVERFLOW)              \
    X(ERROR_MISSING_CONDITION_CLOSING)           \
    X(ERROR_LOOKBEHIND_NOT_FIXED_LENGTH)         \
    X(ERROR_ZERO_RELATIVE_REFERENCE)             \
    X(ERROR_TOO_MANY_CONDITION_BRANCHES)         \
    X(ERROR_CONDITION_ASSERTION_EXPECTED)        \
    X(ERROR_BAD_RELATIVE_REFERENCE)              \
    X(ERROR_UNKNOWN_POSIX_CLASS)                 \
    X(ERROR_INTERNAL_STUDY_ERROR)                \
    X(ERROR_UNICODE_NOT_SUPPORTED)               \
    X(ERROR_PARENTHESES_STACK_CHECK)             \
    X(ERROR_CODE_POINT_TOO_BIG)                  \
    X(ERROR_LOOKBEHIND_TOO_COMPLICATED)          \
    X(ERROR_LOOKBEHIND_INVALID_BACKSLASH_C)      \
    X(ERROR_UNSUPPORTED_ESCAPE_SEQUENCE)         \
    X(ERROR_CALLOUT_NUMBER_TOO_BIG)              \
    X(ERROR_MISSING_CALLOUT_CLOSING)             \
    X(ERROR_ESCAPE_INVALID_IN_VERB)              \
    X(ERROR_UNRECOGNIZED_AFTER_QUERY_P)          \
    X(ERROR_MISSING_NAME_TERMINATOR)             \
    X(ERROR_DUPLICATE_SUBPATTERN_NAME)           \
    X(ERROR_INVALID_SUBPATTERN_NAME)             \
    X(ERROR_UNICODE_PROPERTIES_UNAVAILABLE)      \
    X(ERROR_MALFORMED_UNICODE_PROPERTY)          \
    X(ERROR_UNKNOWN_UNICODE_PROPERTY)            \
    X(ERROR_SUBPATTERN_NAME_TOO_LONG)            \
    X(ERROR_TOO_MANY_NAMED_SUBPATTERNS)          \
    X(ERROR_CLASS_INVALID_RANGE)                 \
    X(ERROR_OCTAL_BYTE_TOO_BIG)                  \
    X(ERROR_INTERNAL_OVERRAN_WORKSPACE)          \
    X(ERROR_INTERNAL_MISSING_SUBPATTERN)         \
    X(ERROR_DEFINE_TOO_MANY_BRANCHES)            \
    X(ERROR_BACKSLASH_O_MISSING_BRACE)           \
    X(ERROR_INTERNAL_UNKNOWN_NEWLINE)            \
    X(ERROR_BACKSLASH_G_SYNTAX)                  \
    X(ERROR_PARENS_QUERY_R_MISSING_CLOSING)      \
    X(ERROR_VERB_ARGUMENT_NOT_ALLOWED)           \
    X(ERROR_VERB_UNKNOWN)                        \
    X(ERROR_SUBPATTERN_NUMBER_TOO_BIG)           \
    X(ERROR_SUBPATTERN_NAME_EXPECTED)            \
    X(ERROR_INTERNAL_PARSED_OVERFLOW)            \
    X(ERROR_INVALID_OCTAL)                       \
    X(ERROR_SUBPATTERN_NAMES_MISMATCH)           \
    X(ERROR_MARK_MISSING_ARGUMENT)               \
    X(ERROR_INVALID_HEXADECIMAL)                 \
    X(ERROR_BACKSLASH_C_SYNTAX)                  \
    X(ERROR_BACKSLASH_K_SYNTAX)                  \
    X(ERROR_INTERNAL_BAD_CODE_LOOKBEHINDS)       \
    X(ERROR_BACKSLASH_N_IN_CLASS)                \
    X(ERROR_CALLOUT_STRING_TOO_LONG)             \
    X(ERROR_UNICODE_DISALLOWED_CODE_POINT)       \
    X(ERROR_UTF_IS_DISABLED)                     \
    X(ERROR_UCP_IS_DISABLED)                     \
    X(ERROR_VERB_NAME_TOO_LONG)                  \
    X(ERROR_BACKSLASH_U_CODE_POINT_TOO_BIG)      \
    X(ERROR_MISSING_OCTAL_OR_HEX_DIGITS)         \
    X(ERROR_VERSION_CONDITION_SYNTAX)            \
    X(ERROR_INTERNAL_BAD_CODE_AUTO_POSSESS)      \
    X(ERROR_CALLOUT_NO_STRING_DELIMITER)         \
    X(ERROR_CALLOUT_BAD_STRING_DELIMITER)        \
    X(ERROR_BACKSLASH_C_CALLER_DISABLED)         \
    X(ERROR_QUERY_BARJX_NEST_TOO_DEEP)           \
    X(ERROR_BACKSLASH_C_LIBRARY_DISABLED)        \
    X(ERROR_PATTERN_TOO_COMPLICATED)             \
    X(ERROR_LOOKBEHIND_TOO_LONG)                 \
    X(ERROR_PATTERN_STRING_TOO_LONG)             \
    X(ERROR_INTERNAL_BAD_CODE)                   \
    X(ERROR_INTERNAL_BAD_CODE_IN_SKIP)           \
    X(ERROR_NO_SURROGATES_IN_UTF16)              \
    X(ERROR_BAD_LITERAL_OPTIONS)                 \
    X(ERROR_SUPPORTED_ONLY_IN_UNICODE)           \
    X(ERROR_INVALID_HYPHEN_IN_OPTIONS)           \
    X(ERROR_ALPHA_ASSERTION_UNKNOWN)             \
    X(ERROR_SCRIPT_RUN_NOT_AVAILABLE)            \
    X(ERROR_TOO_MANY_CAPTURES)                   \
    X(ERROR_CONDITION_ATOMIC_ASSERTION_EXPECTED) \
    X(ERROR_BACKSLASH_K_IN_LOOKAROUND)           \
    X(ERROR_NOMATCH)                             \
    X(ERROR_PARTIAL)                             \
    X(ERROR_UTF8_ERR1)                           \
    X(ERROR_UTF8_ERR2)                           \
    X(ERROR_UTF8_ERR3)                           \
    X(ERROR_UTF8_ERR4)                           \
    X(ERROR_UTF8_ERR5)                           \
    X(ERROR_UTF8_ERR6)                           \
    X(ERROR_UTF8_ERR7)                           \
    X(ERROR_UTF8_ERR8)                           \
    X(ERROR_UTF8_ERR9)                           \
    X(ERROR_UTF8_ERR10)                          \
    X(ERROR_UTF8_ERR11)                          \
    X(ERROR_UTF8_ERR12)                          \
    X(ERROR_UTF8_ERR13)                          \
    X(ERROR_UTF8_ERR14)                          \
    X(ERROR_UTF8_ERR15)                          \
    X(ERROR_UTF8_ERR16)                          \
    X(ERROR_UTF8_ERR17)                          \
    X(ERROR_UTF8_ERR18)                          \
    X(ERROR_UTF8_ERR19)                          \
    X(ERROR_UTF8_ERR20)                          \
    X(ERROR_UTF8_ERR21)                          \
    X(ERROR_UTF16_ERR1)                          \
    X(ERROR_UTF16_ERR2)                          \
    X(ERROR_UTF16_ERR3)                          \
    X(ERROR_UTF32_ERR1)                          \
    X(ERROR_UTF32_ERR2)                          \
    X(ERROR_BADDATA)                             \
    X(ERROR_MIXEDTABLES)                         \
    X(ERROR_BADMAGIC)                            \
    X(ERROR_BADMODE)                             \
    X(ERROR_BADOFFSET)                           \
    X(ERROR_BADOPTION)                           \
    X(ERROR_BADREPLACEMENT)                      \
    X(ERROR_BADUTFOFFSET)                        \
    X(ERROR_CALLOUT)                             \
    X(ERROR_DFA_BADRESTART)                      \
    X(ERROR_DFA_RECURSE)                         \
    X(ERROR_DFA_UCOND)                           \
    X(ERROR_DFA_UFUNC)                           \
    X(ERROR_DFA_UITEM)                           \
    X(ERROR_DFA_WSSIZE)                          \
    X(ERROR_INTERNAL)                            \
    X(ERROR_JIT_BADOPTION)                       \
    X(ERROR_JIT_STACKLIMIT)                      \
    X(ERROR_MATCHLIMIT)                          \
    X(ERROR_NOMEMORY)                            \
    X(ERROR_NOSUBSTRING)                         \
    X(ERROR_NOUNIQUESUBSTRING)                   \
    X(ERROR_NULL)                                \
    X(ERROR_RECURSELOOP)                         \
    X(ERROR_DEPTHLIMIT)                          \
    X(ERROR_RECURSIONLIMIT)                      \
    X(ERROR_UNAVAILABLE)                         \
    X(ERROR_UNSET)                               \
    X(ERROR_BADOFFSETLIMIT)                      \
    X(ERROR_BADREPESCAPE)                        \
    X(ERROR_REPMISSINGBRACE)                     \
    X(ERROR_BADSUBSTITUTION)                     \
    X(ERROR_BADSUBSPATTERN)                      \
    X(ERROR_TOOMANYREPLACE)                      \
    X(ERROR_BADSERIALIZEDDATA)                   \
    X(ERROR_HEAPLIMIT)                           \
    X(ERROR_CONVERT_SYNTAX)                      \
    X(ERROR_INTERNAL_DUPMATCH)                   \
    X(ERROR_DFA_UINVALID_UTF)                    \
    /**/

/**
 * These values are usable as `CompileOptions` OR `MatchOptions`.
 * See `CompileAndMatchOptions` below.
 */
#define MONGO_PCRE_OPTION_EXPAND_TABLE_COMPILE_AND_MATCH_(X) \
    X(ANCHORED)                                              \
    X(NO_UTF_CHECK)                                          \
    X(ENDANCHORED)                                           \
    /**/

/** Options for the `Regex` constructor. See `CompileOptions`. */
#define MONGO_PCRE_OPTION_EXPAND_TABLE_COMPILE_(X) \
    X(ALLOW_EMPTY_CLASS)                           \
    X(ALT_BSUX)                                    \
    X(AUTO_CALLOUT)                                \
    X(CASELESS)                                    \
    X(DOLLAR_ENDONLY)                              \
    X(DOTALL)                                      \
    X(DUPNAMES)                                    \
    X(EXTENDED)                                    \
    X(FIRSTLINE)                                   \
    X(MATCH_UNSET_BACKREF)                         \
    X(MULTILINE)                                   \
    X(NEVER_UCP)                                   \
    X(NEVER_UTF)                                   \
    X(NO_AUTO_CAPTURE)                             \
    X(NO_AUTO_POSSESS)                             \
    X(NO_DOTSTAR_ANCHOR)                           \
    X(NO_START_OPTIMIZE)                           \
    X(UCP)                                         \
    X(UNGREEDY)                                    \
    X(UTF)                                         \
    X(NEVER_BACKSLASH_C)                           \
    X(ALT_CIRCUMFLEX)                              \
    X(ALT_VERBNAMES)                               \
    X(USE_OFFSET_LIMIT)                            \
    X(EXTENDED_MORE)                               \
    X(LITERAL)                                     \
    X(MATCH_INVALID_UTF)                           \
    /**/

/** Options for match and/or substitute calls. See `MatchOptions`. */
#define MONGO_PCRE_OPTION_EXPAND_TABLE_MATCH_(X) \
    X(NOTBOL)                                    \
    X(NOTEOL)                                    \
    X(NOTEMPTY)                                  \
    X(NOTEMPTY_ATSTART)                          \
    X(PARTIAL_SOFT)                              \
    X(PARTIAL_HARD)                              \
    X(DFA_RESTART)                               \
    X(DFA_SHORTEST)                              \
    X(SUBSTITUTE_GLOBAL)                         \
    X(SUBSTITUTE_EXTENDED)                       \
    X(SUBSTITUTE_UNSET_EMPTY)                    \
    X(SUBSTITUTE_UNKNOWN_UNSET)                  \
    X(SUBSTITUTE_OVERFLOW_LENGTH)                \
    X(NO_JIT)                                    \
    X(COPY_MATCHED_SUBJECT)                      \
    X(SUBSTITUTE_LITERAL)                        \
    X(SUBSTITUTE_MATCHED)                        \
    X(SUBSTITUTE_REPLACEMENT_ONLY)               \
    /**/

#if 0
/** Extended compile options for the compile context. Not yet used. */
#define MONGO_PCRE_OPTION_EXPAND_TABLE_EXTRA_(X) \
    X(EXTRA_ALLOW_SURROGATE_ESCAPES)             \
    X(EXTRA_BAD_ESCAPE_IS_LITERAL)               \
    X(EXTRA_MATCH_WORD)                          \
    X(EXTRA_MATCH_LINE)                          \
    X(EXTRA_ESCAPED_CR_IS_LF)                    \
    X(EXTRA_ALT_BSUX)                            \
    X(EXTRA_ALLOW_LOOKAROUND_BSK)                \
    /**/
#endif  // 0

/**
 * The `std::error_code` enum type for the PCRE2 error number space.
 * `std::is_error_code_enum` is specialized for this enum type, which enables
 * `Errc` to be implicitly convertible to `std::error_code`.
 */
enum class Errc : int {
    OK = 0,
#define X_(name) name,
    MONGO_PCRE_ERROR_EXPAND_TABLE_(X_)
#undef X_
};

/** Category for a pcre2 API error code. */
const std::error_category& pcreCategory() noexcept;

/** Wrap a pcre2 API error code in a std::error_code{e,errorCategory()}. */
inline std::error_code pcreError(int e) noexcept {
    return std::error_code(e, pcreCategory());
}

/**
 * Creates a `std::error_code` from an `Errc`.
 * An implicit `std::error_code` constructor finds this function by ADL.
 */
inline std::error_code make_error_code(Errc e) noexcept {
    return pcreError(static_cast<std::underlying_type_t<Errc>>(e));
}

namespace detail {
/**
 * A typesafe wrapper around `uint32_t`, representing the bitfields of PCRE2
 * options. A CRTP base class for the `Options` types.
 */
template <typename D>
class Options {
public:
    constexpr Options() noexcept = default;
    constexpr explicit Options(uint32_t v) noexcept : _v{v} {}
    constexpr explicit operator uint32_t() const noexcept {
        return _v;
    }
    constexpr explicit operator bool() const noexcept {
        return _v;
    }
    constexpr friend D& operator|=(D& a, D b) noexcept {
        return a = D{a._v | b._v};
    }
    constexpr friend D& operator&=(D& a, D b) noexcept {
        return a = D{a._v & b._v};
    }
    constexpr friend D operator~(D a) noexcept {
        return D{~a._v};
    }
    constexpr friend D operator|(D a, D b) noexcept {
        return a |= b;
    }
    constexpr friend D operator&(D a, D b) noexcept {
        return a &= b;
    }

private:
    uint32_t _v = 0;
};
}  // namespace detail

/** The bitfield of Regex compile (constructor) options. */
class CompileOptions : public detail::Options<CompileOptions> {
public:
    using detail::Options<CompileOptions>::Options;
};

/** The bitfield of `Regex` match options. */
class MatchOptions : public detail::Options<MatchOptions> {
public:
    using detail::Options<MatchOptions>::Options;
};

/**
 * A few of the PCRE2 options' bit positions are usable as compile options OR
 * match options. We model this by making this type implicitly convertible to
 * both `CompileOptions` and `MatchOptions`.
 */
class CompileAndMatchOptions : public detail::Options<CompileAndMatchOptions> {
public:
    using detail::Options<CompileAndMatchOptions>::Options;
    constexpr operator CompileOptions() const noexcept {
        return CompileOptions{uint32_t{*this}};
    }
    constexpr operator MatchOptions() const noexcept {
        return MatchOptions{uint32_t{*this}};
    }
};

/**
 * @{
 * The `CompileOptions`, `MatchOptions`, and `CompileAndMatchOptions` values
 * are declared as `extern const` and not as more basic enum or constexpr values.
 * This arrangement allows the variables to be given definitions in the
 * pcre.cpp file, where the pcre2 library's corresponding macros are available
 * to their initializers.
 *
 * It can be assumed that these variables have static constant initialization.
 * That is, they are available for use in static initializers.
 *
 * The options values are given an inilne namespace so they can be brought into
 * a local scope with a `using namespace pcre::options;` directive.
 */
inline namespace options {
#define X_(name) extern const CompileOptions name;
MONGO_PCRE_OPTION_EXPAND_TABLE_COMPILE_(X_)
#undef X_

#define X_(name) extern const MatchOptions name;
MONGO_PCRE_OPTION_EXPAND_TABLE_MATCH_(X_)
#undef X_

#define X_(name) extern const CompileAndMatchOptions name;
MONGO_PCRE_OPTION_EXPAND_TABLE_COMPILE_AND_MATCH_(X_)
#undef X_
}  // namespace options
/** @} */

class MatchData;

namespace detail {
class RegexImpl;
class MatchDataImpl;
}  // namespace detail

/**
 * Wrapper class encapsulating the PCRE2 regular expression library.
 * See https://www.pcre.org/current/doc/html/
 */
class Regex {
public:
    Regex(std::string pattern, CompileOptions options);

    explicit Regex(std::string pattern) : Regex{std::move(pattern), CompileOptions{}} {}

    ~Regex();

    Regex(const Regex&);

    Regex& operator=(const Regex&);

    Regex(Regex&&) noexcept;

    Regex& operator=(Regex&&) noexcept;

    /** The pattern string from the constructor. */
    const std::string& pattern() const;

    /** The Options from the constructor. */
    CompileOptions options() const;

    /** True if this Regex was created without error. */
    explicit operator bool() const;

    /** The error saved from the compile of this Regex. */
    std::error_code error() const;

    /** Position in the pattern at which the compile `error` occurred. */
    size_t errorPosition() const;

    /** Count of subpattern captures in this pattern. */
    size_t captureCount() const;

    /** The size of the compiled regex. */
    size_t codeSize() const;

    /**
     * Creates a MatchData by applying this regex to an `input` string.
     *
     * Options supplied at `match` time cannot be optimized as well as behaviors
     * like '^', '$' built into the Regex directly.
     */
    MatchData match(std::string input, MatchOptions options, size_t startPos) const;
    MatchData match(std::string input, MatchOptions options) const;
    MatchData match(std::string input) const;

    /** Can avoid a string copy when input will outlive the returned MatchData. */
    MatchData matchView(StringData input, MatchOptions options, size_t startPos) const;
    MatchData matchView(StringData input, MatchOptions options) const;
    MatchData matchView(StringData input) const;

    /**
     * Replaces occurrences in `str` of this pattern with `replacement`.
     * Additional substitute `options` can change behavior. Important ones:
     *
     *  - SUBSTITUTE_GLOBAL: Replace all occurrences
     *  - SUBSTITUTE_LITERAL: No $ variable expansions in replacement
     *  - SUBSTITUTE_EXTENDED: Better escapes, bash-like substitutions
     *
     * See https://www.pcre.org/current/doc/html/pcre2api.html#SEC36
     */
    int substitute(StringData replacement,
                   std::string* str,
                   MatchOptions options = {},
                   size_t startPos = 0) const;

private:
    std::unique_ptr<detail::RegexImpl> _impl;
};

/**
 * Represents the result of a Regex match (or matchView operation).
 * The MatchData refers to the Regex that produced it, so the Regex
 * must outlive any MatchData it produces.
 */
class MatchData {
public:
    /** Implementation detail used by Regex to create these objects. */
    explicit MatchData(std::unique_ptr<detail::MatchDataImpl> impl);

    ~MatchData();

    MatchData(MatchData&&) noexcept;
    MatchData& operator=(MatchData&&) noexcept;

    /** True if the match succeeded. */
    explicit operator bool() const;

    /**
     * Returns the number of subpatterns captured by this match,
     * Does not count the `m[0]` element: only subpattern captures.
     */
    size_t captureCount() const;

    /**
     * @{
     * Returns a match group by index or by name. Element zero (by index) is
     * the full matched substring, followed by captures.
     *
     * An empty and null capture are slightly different and can be
     * distinguished by their rawData pointer. The difference doesn't
     * often matter though. E.g.,
     *    Regex{"(A|B(C))"}.match("A")[2].rawData() == nullptr
     * because capture group 2 (the `C`) was on the inactive `B` branch.
     * Throws `ExceptionFor<NoSuchKey>` if capture not found.
     * Requires `i <= captureCount()`.
     */
    StringData operator[](size_t i) const;
    StringData operator[](const std::string& name) const;
    /** @} */

    /**
     * All capture groups. For MatchData `m`:
     *     {m[1]... m[captureCount()]};
     */
    std::vector<StringData> getCaptures() const;

    /** Same as `getCaptures`, but as `std::vector<std::string>`. */
    std::vector<std::string> getCapturesStrings() const {
        return _strVec(getCaptures());
    }

    /**
     * The matched substring, followed by the `getCaptures()` list.
     * For MatchData `m`:
     *     {m[0], m[1]... m[m.captureCount()]};
     */
    std::vector<StringData> getMatchList() const;

    /** Same as `getMatchList`, but as `std::vector<std::string>`. */
    std::vector<std::string> getMatchListStrings() const {
        return _strVec(getMatchList());
    }

    /** Error saved from the match that created this object. */
    std::error_code error() const;

    /**
     * The input to the match that created this object. If this MatchData was
     * created by a `match` call, the `input` refers to a string owned by this
     * object. If this MatchData was created by a `matchView` call, then this
     * `input` result refers to the StringData provided to it.
     */
    StringData input() const;

    size_t startPos() const;

private:
    static std::vector<std::string> _strVec(const std::vector<StringData>& v) {
        std::vector<std::string> r;
        r.reserve(v.size());
        for (StringData s : v)
            r.push_back(std::string{s});
        return r;
    }

    std::unique_ptr<detail::MatchDataImpl> _impl;
};

inline MatchData Regex::match(std::string input, MatchOptions options) const {
    return match(std::move(input), options, 0);
}
inline MatchData Regex::match(std::string input) const {
    return match(std::move(input), MatchOptions{}, 0);
}
inline MatchData Regex::matchView(StringData input, MatchOptions options) const {
    return matchView(input, options, 0);
}
inline MatchData Regex::matchView(StringData input) const {
    return matchView(input, MatchOptions{}, 0);
}

}  // namespace mongo::pcre

namespace std {
template <>
struct is_error_code_enum<mongo::pcre::Errc> : std::true_type {};
}  // namespace std
