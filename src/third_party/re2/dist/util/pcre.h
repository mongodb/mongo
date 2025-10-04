// Copyright 2003-2010 Google Inc.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#ifndef UTIL_PCRE_H_
#define UTIL_PCRE_H_

// This is a variant of PCRE's pcrecpp.h, originally written at Google.
// The main changes are the addition of the HitLimit method and
// compilation as PCRE in namespace re2.

// C++ interface to the pcre regular-expression library.  PCRE supports
// Perl-style regular expressions (with extensions like \d, \w, \s,
// ...).
//
// -----------------------------------------------------------------------
// REGEXP SYNTAX:
//
// This module uses the pcre library and hence supports its syntax
// for regular expressions:
//
//      http://www.google.com/search?q=pcre
//
// The syntax is pretty similar to Perl's.  For those not familiar
// with Perl's regular expressions, here are some examples of the most
// commonly used extensions:
//
//   "hello (\\w+) world"  -- \w matches a "word" character
//   "version (\\d+)"      -- \d matches a digit
//   "hello\\s+world"      -- \s matches any whitespace character
//   "\\b(\\w+)\\b"        -- \b matches empty string at a word boundary
//   "(?i)hello"           -- (?i) turns on case-insensitive matching
//   "/\\*(.*?)\\*/"       -- .*? matches . minimum no. of times possible
//
// -----------------------------------------------------------------------
// MATCHING INTERFACE:
//
// The "FullMatch" operation checks that supplied text matches a
// supplied pattern exactly.
//
// Example: successful match
//    ABSL_CHECK(PCRE::FullMatch("hello", "h.*o"));
//
// Example: unsuccessful match (requires full match):
//    ABSL_CHECK(!PCRE::FullMatch("hello", "e"));
//
// -----------------------------------------------------------------------
// UTF-8 AND THE MATCHING INTERFACE:
//
// By default, pattern and text are plain text, one byte per character.
// The UTF8 flag, passed to the constructor, causes both pattern
// and string to be treated as UTF-8 text, still a byte stream but
// potentially multiple bytes per character. In practice, the text
// is likelier to be UTF-8 than the pattern, but the match returned
// may depend on the UTF8 flag, so always use it when matching
// UTF8 text.  E.g., "." will match one byte normally but with UTF8
// set may match up to three bytes of a multi-byte character.
//
// Example:
//    PCRE re(utf8_pattern, PCRE::UTF8);
//    ABSL_CHECK(PCRE::FullMatch(utf8_string, re));
//
// -----------------------------------------------------------------------
// MATCHING WITH SUBSTRING EXTRACTION:
//
// You can supply extra pointer arguments to extract matched substrings.
//
// Example: extracts "ruby" into "s" and 1234 into "i"
//    int i;
//    std::string s;
//    ABSL_CHECK(PCRE::FullMatch("ruby:1234", "(\\w+):(\\d+)", &s, &i));
//
// Example: fails because string cannot be stored in integer
//    ABSL_CHECK(!PCRE::FullMatch("ruby", "(.*)", &i));
//
// Example: fails because there aren't enough sub-patterns:
//    ABSL_CHECK(!PCRE::FullMatch("ruby:1234", "\\w+:\\d+", &s));
//
// Example: does not try to extract any extra sub-patterns
//    ABSL_CHECK(PCRE::FullMatch("ruby:1234", "(\\w+):(\\d+)", &s));
//
// Example: does not try to extract into NULL
//    ABSL_CHECK(PCRE::FullMatch("ruby:1234", "(\\w+):(\\d+)", NULL, &i));
//
// Example: integer overflow causes failure
//    ABSL_CHECK(!PCRE::FullMatch("ruby:1234567891234", "\\w+:(\\d+)", &i));
//
// -----------------------------------------------------------------------
// PARTIAL MATCHES
//
// You can use the "PartialMatch" operation when you want the pattern
// to match any substring of the text.
//
// Example: simple search for a string:
//      ABSL_CHECK(PCRE::PartialMatch("hello", "ell"));
//
// Example: find first number in a string
//      int number;
//      ABSL_CHECK(PCRE::PartialMatch("x*100 + 20", "(\\d+)", &number));
//      ABSL_CHECK_EQ(number, 100);
//
// -----------------------------------------------------------------------
// PPCRE-COMPILED PCREGULAR EXPPCRESSIONS
//
// PCRE makes it easy to use any string as a regular expression, without
// requiring a separate compilation step.
//
// If speed is of the essence, you can create a pre-compiled "PCRE"
// object from the pattern and use it multiple times.  If you do so,
// you can typically parse text faster than with sscanf.
//
// Example: precompile pattern for faster matching:
//    PCRE pattern("h.*o");
//    while (ReadLine(&str)) {
//      if (PCRE::FullMatch(str, pattern)) ...;
//    }
//
// -----------------------------------------------------------------------
// SCANNING TEXT INCPCREMENTALLY
//
// The "Consume" operation may be useful if you want to repeatedly
// match regular expressions at the front of a string and skip over
// them as they match.  This requires use of the string_view type,
// which represents a sub-range of a real string.
//
// Example: read lines of the form "var = value" from a string.
//      std::string contents = ...;         // Fill string somehow
//      absl::string_view input(contents);  // Wrap a string_view around it
//
//      std::string var;
//      int value;
//      while (PCRE::Consume(&input, "(\\w+) = (\\d+)\n", &var, &value)) {
//        ...;
//      }
//
// Each successful call to "Consume" will set "var/value", and also
// advance "input" so it points past the matched text.  Note that if the
// regular expression matches an empty string, input will advance
// by 0 bytes.  If the regular expression being used might match
// an empty string, the loop body must check for this case and either
// advance the string or break out of the loop.
//
// The "FindAndConsume" operation is similar to "Consume" but does not
// anchor your match at the beginning of the string.  For example, you
// could extract all words from a string by repeatedly calling
//     PCRE::FindAndConsume(&input, "(\\w+)", &word)
//
// -----------------------------------------------------------------------
// PARSING HEX/OCTAL/C-RADIX NUMBERS
//
// By default, if you pass a pointer to a numeric value, the
// corresponding text is interpreted as a base-10 number.  You can
// instead wrap the pointer with a call to one of the operators Hex(),
// Octal(), or CRadix() to interpret the text in another base.  The
// CRadix operator interprets C-style "0" (base-8) and "0x" (base-16)
// prefixes, but defaults to base-10.
//
// Example:
//   int a, b, c, d;
//   ABSL_CHECK(PCRE::FullMatch("100 40 0100 0x40", "(.*) (.*) (.*) (.*)",
//         Octal(&a), Hex(&b), CRadix(&c), CRadix(&d));
// will leave 64 in a, b, c, and d.

#include "absl/strings/string_view.h"

#ifdef USEPCRE
#include <pcre.h>
namespace re2 {
const bool UsingPCRE = true;
}  // namespace re2
#else
struct pcre;  // opaque
namespace re2 {
const bool UsingPCRE = false;
}  // namespace re2
#endif

// To produce a DLL, CMake can automatically export code symbols,
// but not data symbols, so we have to annotate those manually...
#if defined(RE2_BUILD_TESTING_DLL)
#define RE2_TESTING_DLL __declspec(dllexport)
#elif defined(RE2_CONSUME_TESTING_DLL)
#define RE2_TESTING_DLL __declspec(dllimport)
#else
#define RE2_TESTING_DLL
#endif

namespace re2 {

class PCRE_Options;

// Interface for regular expression matching.  Also corresponds to a
// pre-compiled regular expression.  An "PCRE" object is safe for
// concurrent use by multiple threads.
class PCRE {
 public:
  // We convert user-passed pointers into special Arg objects
  class Arg;

  // Marks end of arg list.
  // ONLY USE IN OPTIONAL ARG DEFAULTS.
  // DO NOT PASS EXPLICITLY.
  RE2_TESTING_DLL static Arg no_more_args;

  // Options are same value as those in pcre.  We provide them here
  // to avoid users needing to include pcre.h and also to isolate
  // users from pcre should we change the underlying library.
  // Only those needed by Google programs are exposed here to
  // avoid collision with options employed internally by regexp.cc
  // Note that some options have equivalents that can be specified in
  // the regexp itself.  For example, prefixing your regexp with
  // "(?s)" has the same effect as the PCRE_DOTALL option.
  enum Option {
    None = 0x0000,
    UTF8 = 0x0800,  // == PCRE_UTF8
    EnabledCompileOptions = UTF8,
    EnabledExecOptions = 0x0000,  // TODO: use to replace anchor flag
  };

  // We provide implicit conversions from strings so that users can
  // pass in a string or a "const char*" wherever an "PCRE" is expected.
  PCRE(const char* pattern);
  PCRE(const char* pattern, Option option);
  PCRE(const std::string& pattern);
  PCRE(const std::string& pattern, Option option);
  PCRE(const char *pattern, const PCRE_Options& re_option);
  PCRE(const std::string& pattern, const PCRE_Options& re_option);

  ~PCRE();

  // The string specification for this PCRE.  E.g.
  //   PCRE re("ab*c?d+");
  //   re.pattern();    // "ab*c?d+"
  const std::string& pattern() const { return pattern_; }

  // If PCRE could not be created properly, returns an error string.
  // Else returns the empty string.
  const std::string& error() const { return *error_; }

  // Whether the PCRE has hit a match limit during execution.
  // Not thread safe.  Intended only for testing.
  // If hitting match limits is a problem,
  // you should be using PCRE2 (re2/re2.h)
  // instead of checking this flag.
  bool HitLimit();
  void ClearHitLimit();

  /***** The useful part: the matching interface *****/

  // Matches "text" against "pattern".  If pointer arguments are
  // supplied, copies matched sub-patterns into them.
  //
  // You can pass in a "const char*" or a "std::string" for "text".
  // You can pass in a "const char*" or a "std::string" or a "PCRE" for "pattern".
  //
  // The provided pointer arguments can be pointers to any scalar numeric
  // type, or one of:
  //    std::string        (matched piece is copied to string)
  //    absl::string_view  (string_view is mutated to point to matched piece)
  //    T                  ("bool T::ParseFrom(const char*, size_t)" must exist)
  //    (void*)NULL        (the corresponding matched sub-pattern is not copied)
  //
  // Returns true iff all of the following conditions are satisfied:
  //   a. "text" matches "pattern" exactly
  //   b. The number of matched sub-patterns is >= number of supplied pointers
  //   c. The "i"th argument has a suitable type for holding the
  //      string captured as the "i"th sub-pattern.  If you pass in
  //      NULL for the "i"th argument, or pass fewer arguments than
  //      number of sub-patterns, "i"th captured sub-pattern is
  //      ignored.
  //
  // CAVEAT: An optional sub-pattern that does not exist in the
  // matched string is assigned the empty string.  Therefore, the
  // following will return false (because the empty string is not a
  // valid number):
  //    int number;
  //    PCRE::FullMatch("abc", "[a-z]+(\\d+)?", &number);
  struct FullMatchFunctor {
    bool operator ()(absl::string_view text, const PCRE& re,  // 3..16 args
                     const Arg& ptr1 = no_more_args,
                     const Arg& ptr2 = no_more_args,
                     const Arg& ptr3 = no_more_args,
                     const Arg& ptr4 = no_more_args,
                     const Arg& ptr5 = no_more_args,
                     const Arg& ptr6 = no_more_args,
                     const Arg& ptr7 = no_more_args,
                     const Arg& ptr8 = no_more_args,
                     const Arg& ptr9 = no_more_args,
                     const Arg& ptr10 = no_more_args,
                     const Arg& ptr11 = no_more_args,
                     const Arg& ptr12 = no_more_args,
                     const Arg& ptr13 = no_more_args,
                     const Arg& ptr14 = no_more_args,
                     const Arg& ptr15 = no_more_args,
                     const Arg& ptr16 = no_more_args) const;
  };

  RE2_TESTING_DLL static const FullMatchFunctor FullMatch;

  // Exactly like FullMatch(), except that "pattern" is allowed to match
  // a substring of "text".
  struct PartialMatchFunctor {
    bool operator ()(absl::string_view text, const PCRE& re,  // 3..16 args
                     const Arg& ptr1 = no_more_args,
                     const Arg& ptr2 = no_more_args,
                     const Arg& ptr3 = no_more_args,
                     const Arg& ptr4 = no_more_args,
                     const Arg& ptr5 = no_more_args,
                     const Arg& ptr6 = no_more_args,
                     const Arg& ptr7 = no_more_args,
                     const Arg& ptr8 = no_more_args,
                     const Arg& ptr9 = no_more_args,
                     const Arg& ptr10 = no_more_args,
                     const Arg& ptr11 = no_more_args,
                     const Arg& ptr12 = no_more_args,
                     const Arg& ptr13 = no_more_args,
                     const Arg& ptr14 = no_more_args,
                     const Arg& ptr15 = no_more_args,
                     const Arg& ptr16 = no_more_args) const;
  };

  RE2_TESTING_DLL static const PartialMatchFunctor PartialMatch;

  // Like FullMatch() and PartialMatch(), except that pattern has to
  // match a prefix of "text", and "input" is advanced past the matched
  // text.  Note: "input" is modified iff this routine returns true.
  struct ConsumeFunctor {
    bool operator ()(absl::string_view* input, const PCRE& pattern,  // 3..16 args
                     const Arg& ptr1 = no_more_args,
                     const Arg& ptr2 = no_more_args,
                     const Arg& ptr3 = no_more_args,
                     const Arg& ptr4 = no_more_args,
                     const Arg& ptr5 = no_more_args,
                     const Arg& ptr6 = no_more_args,
                     const Arg& ptr7 = no_more_args,
                     const Arg& ptr8 = no_more_args,
                     const Arg& ptr9 = no_more_args,
                     const Arg& ptr10 = no_more_args,
                     const Arg& ptr11 = no_more_args,
                     const Arg& ptr12 = no_more_args,
                     const Arg& ptr13 = no_more_args,
                     const Arg& ptr14 = no_more_args,
                     const Arg& ptr15 = no_more_args,
                     const Arg& ptr16 = no_more_args) const;
  };

  RE2_TESTING_DLL static const ConsumeFunctor Consume;

  // Like Consume(..), but does not anchor the match at the beginning of the
  // string.  That is, "pattern" need not start its match at the beginning of
  // "input".  For example, "FindAndConsume(s, "(\\w+)", &word)" finds the next
  // word in "s" and stores it in "word".
  struct FindAndConsumeFunctor {
    bool operator ()(absl::string_view* input, const PCRE& pattern,  // 3..16 args
                     const Arg& ptr1 = no_more_args,
                     const Arg& ptr2 = no_more_args,
                     const Arg& ptr3 = no_more_args,
                     const Arg& ptr4 = no_more_args,
                     const Arg& ptr5 = no_more_args,
                     const Arg& ptr6 = no_more_args,
                     const Arg& ptr7 = no_more_args,
                     const Arg& ptr8 = no_more_args,
                     const Arg& ptr9 = no_more_args,
                     const Arg& ptr10 = no_more_args,
                     const Arg& ptr11 = no_more_args,
                     const Arg& ptr12 = no_more_args,
                     const Arg& ptr13 = no_more_args,
                     const Arg& ptr14 = no_more_args,
                     const Arg& ptr15 = no_more_args,
                     const Arg& ptr16 = no_more_args) const;
  };

  RE2_TESTING_DLL static const FindAndConsumeFunctor FindAndConsume;

  // Replace the first match of "pattern" in "str" with "rewrite".
  // Within "rewrite", backslash-escaped digits (\1 to \9) can be
  // used to insert text matching corresponding parenthesized group
  // from the pattern.  \0 in "rewrite" refers to the entire matching
  // text.  E.g.,
  //
  //   std::string s = "yabba dabba doo";
  //   ABSL_CHECK(PCRE::Replace(&s, "b+", "d"));
  //
  // will leave "s" containing "yada dabba doo"
  //
  // Returns true if the pattern matches and a replacement occurs,
  // false otherwise.
  static bool Replace(std::string* str, const PCRE& pattern,
                      absl::string_view rewrite);

  // Like Replace(), except replaces all occurrences of the pattern in
  // the string with the rewrite.  Replacements are not subject to
  // re-matching.  E.g.,
  //
  //   std::string s = "yabba dabba doo";
  //   ABSL_CHECK(PCRE::GlobalReplace(&s, "b+", "d"));
  //
  // will leave "s" containing "yada dada doo"
  //
  // Returns the number of replacements made.
  static int GlobalReplace(std::string* str, const PCRE& pattern,
                           absl::string_view rewrite);

  // Like Replace, except that if the pattern matches, "rewrite"
  // is copied into "out" with substitutions.  The non-matching
  // portions of "text" are ignored.
  //
  // Returns true iff a match occurred and the extraction happened
  // successfully;  if no match occurs, the string is left unaffected.
  static bool Extract(absl::string_view text, const PCRE& pattern,
                      absl::string_view rewrite, std::string* out);

  // Check that the given @p rewrite string is suitable for use with
  // this PCRE.  It checks that:
  //   * The PCRE has enough parenthesized subexpressions to satisfy all
  //       of the \N tokens in @p rewrite, and
  //   * The @p rewrite string doesn't have any syntax errors
  //       ('\' followed by anything besides [0-9] and '\').
  // Making this test will guarantee that "replace" and "extract"
  // operations won't ABSL_LOG(ERROR) or fail because of a bad rewrite
  // string.
  // @param rewrite The proposed rewrite string.
  // @param error An error message is recorded here, iff we return false.
  //              Otherwise, it is unchanged.
  // @return true, iff @p rewrite is suitable for use with the PCRE.
  bool CheckRewriteString(absl::string_view rewrite, std::string* error) const;

  // Returns a copy of 'unquoted' with all potentially meaningful
  // regexp characters backslash-escaped.  The returned string, used
  // as a regular expression, will exactly match the original string.
  // For example,
  //           1.5-2.0?
  //  becomes:
  //           1\.5\-2\.0\?
  static std::string QuoteMeta(absl::string_view unquoted);

  /***** Generic matching interface (not so nice to use) *****/

  // Type of match (TODO: Should be restructured as an Option)
  enum Anchor {
    UNANCHORED,         // No anchoring
    ANCHOR_START,       // Anchor at start only
    ANCHOR_BOTH,        // Anchor at start and end
  };

  // General matching routine.  Stores the length of the match in
  // "*consumed" if successful.
  bool DoMatch(absl::string_view text, Anchor anchor, size_t* consumed,
               const Arg* const* args, int n) const;

  // Return the number of capturing subpatterns, or -1 if the
  // regexp wasn't valid on construction.
  int NumberOfCapturingGroups() const;

 private:
  void Init(const char* pattern, Option option, int match_limit,
            int stack_limit, bool report_errors);

  // Match against "text", filling in "vec" (up to "vecsize" * 2/3) with
  // pairs of integers for the beginning and end positions of matched
  // text.  The first pair corresponds to the entire matched text;
  // subsequent pairs correspond, in order, to parentheses-captured
  // matches.  Returns the number of pairs (one more than the number of
  // the last subpattern with a match) if matching was successful
  // and zero if the match failed.
  // I.e. for PCRE("(foo)|(bar)|(baz)") it will return 2, 3, and 4 when matching
  // against "foo", "bar", and "baz" respectively.
  // When matching PCRE("(foo)|hello") against "hello", it will return 1.
  // But the values for all subpattern are filled in into "vec".
  int TryMatch(absl::string_view text, size_t startpos, Anchor anchor,
               bool empty_ok, int* vec, int vecsize) const;

  // Append the "rewrite" string, with backslash substitutions from "text"
  // and "vec", to string "out".
  bool Rewrite(std::string* out, absl::string_view rewrite,
               absl::string_view text, int* vec, int veclen) const;

  // internal implementation for DoMatch
  bool DoMatchImpl(absl::string_view text, Anchor anchor, size_t* consumed,
                   const Arg* const args[], int n, int* vec, int vecsize) const;

  // Compile the regexp for the specified anchoring mode
  pcre* Compile(Anchor anchor);

  std::string         pattern_;
  Option              options_;
  pcre*               re_full_;        // For full matches
  pcre*               re_partial_;     // For partial matches
  const std::string*  error_;          // Error indicator (or empty string)
  bool                report_errors_;  // Silences error logging if false
  int                 match_limit_;    // Limit on execution resources
  int                 stack_limit_;    // Limit on stack resources (bytes)
  mutable int         hit_limit_;      // Hit limit during execution (bool)

  PCRE(const PCRE&) = delete;
  PCRE& operator=(const PCRE&) = delete;
};

// PCRE_Options allow you to set the PCRE::Options, plus any pcre
// "extra" options.  The only extras are match_limit, which limits
// the CPU time of a match, and stack_limit, which limits the
// stack usage.  Setting a limit to <= 0 lets PCRE pick a sensible default
// that should not cause too many problems in production code.
// If PCRE hits a limit during a match, it may return a false negative,
// but (hopefully) it won't crash.
//
// NOTE: If you are handling regular expressions specified by
// (external or internal) users, rather than hard-coded ones,
// you should be using PCRE2, which uses an alternate implementation
// that avoids these issues.  See http://go/re2quick.
class PCRE_Options {
 public:
  // constructor
  PCRE_Options() : option_(PCRE::None), match_limit_(0), stack_limit_(0), report_errors_(true) {}
  // accessors
  PCRE::Option option() const { return option_; }
  void set_option(PCRE::Option option) {
    option_ = option;
  }
  int match_limit() const { return match_limit_; }
  void set_match_limit(int match_limit) {
    match_limit_ = match_limit;
  }
  int stack_limit() const { return stack_limit_; }
  void set_stack_limit(int stack_limit) {
    stack_limit_ = stack_limit;
  }

  // If the regular expression is malformed, an error message will be printed
  // iff report_errors() is true.  Default: true.
  bool report_errors() const { return report_errors_; }
  void set_report_errors(bool report_errors) {
    report_errors_ = report_errors;
  }
 private:
  PCRE::Option option_;
  int match_limit_;
  int stack_limit_;
  bool report_errors_;
};


/***** Implementation details *****/

// Hex/Octal/Binary?

// Special class for parsing into objects that define a ParseFrom() method
template <typename T>
class _PCRE_MatchObject {
 public:
  static inline bool Parse(const char* str, size_t n, void* dest) {
    if (dest == NULL) return true;
    T* object = reinterpret_cast<T*>(dest);
    return object->ParseFrom(str, n);
  }
};

class PCRE::Arg {
 public:
  // Empty constructor so we can declare arrays of PCRE::Arg
  Arg();

  // Constructor specially designed for NULL arguments
  Arg(void*);

  typedef bool (*Parser)(const char* str, size_t n, void* dest);

// Type-specific parsers
#define MAKE_PARSER(type, name)            \
  Arg(type* p) : arg_(p), parser_(name) {} \
  Arg(type* p, Parser parser) : arg_(p), parser_(parser) {}

  MAKE_PARSER(char,               parse_char);
  MAKE_PARSER(signed char,        parse_schar);
  MAKE_PARSER(unsigned char,      parse_uchar);
  MAKE_PARSER(float,              parse_float);
  MAKE_PARSER(double,             parse_double);
  MAKE_PARSER(std::string,        parse_string);
  MAKE_PARSER(absl::string_view,  parse_string_view);

  MAKE_PARSER(short,              parse_short);
  MAKE_PARSER(unsigned short,     parse_ushort);
  MAKE_PARSER(int,                parse_int);
  MAKE_PARSER(unsigned int,       parse_uint);
  MAKE_PARSER(long,               parse_long);
  MAKE_PARSER(unsigned long,      parse_ulong);
  MAKE_PARSER(long long,          parse_longlong);
  MAKE_PARSER(unsigned long long, parse_ulonglong);

#undef MAKE_PARSER

  // Generic constructor
  template <typename T> Arg(T*, Parser parser);
  // Generic constructor template
  template <typename T> Arg(T* p)
    : arg_(p), parser_(_PCRE_MatchObject<T>::Parse) {
  }

  // Parse the data
  bool Parse(const char* str, size_t n) const;

 private:
  void*         arg_;
  Parser        parser_;

  static bool parse_null        (const char* str, size_t n, void* dest);
  static bool parse_char        (const char* str, size_t n, void* dest);
  static bool parse_schar       (const char* str, size_t n, void* dest);
  static bool parse_uchar       (const char* str, size_t n, void* dest);
  static bool parse_float       (const char* str, size_t n, void* dest);
  static bool parse_double      (const char* str, size_t n, void* dest);
  static bool parse_string      (const char* str, size_t n, void* dest);
  static bool parse_string_view (const char* str, size_t n, void* dest);

#define DECLARE_INTEGER_PARSER(name)                                       \
 private:                                                                  \
  static bool parse_##name(const char* str, size_t n, void* dest);         \
  static bool parse_##name##_radix(const char* str, size_t n, void* dest,  \
                                   int radix);                             \
                                                                           \
 public:                                                                   \
  static bool parse_##name##_hex(const char* str, size_t n, void* dest);   \
  static bool parse_##name##_octal(const char* str, size_t n, void* dest); \
  static bool parse_##name##_cradix(const char* str, size_t n, void* dest)

  DECLARE_INTEGER_PARSER(short);
  DECLARE_INTEGER_PARSER(ushort);
  DECLARE_INTEGER_PARSER(int);
  DECLARE_INTEGER_PARSER(uint);
  DECLARE_INTEGER_PARSER(long);
  DECLARE_INTEGER_PARSER(ulong);
  DECLARE_INTEGER_PARSER(longlong);
  DECLARE_INTEGER_PARSER(ulonglong);

#undef DECLARE_INTEGER_PARSER

};

inline PCRE::Arg::Arg() : arg_(NULL), parser_(parse_null) { }
inline PCRE::Arg::Arg(void* p) : arg_(p), parser_(parse_null) { }

inline bool PCRE::Arg::Parse(const char* str, size_t n) const {
  return (*parser_)(str, n, arg_);
}

// This part of the parser, appropriate only for ints, deals with bases
#define MAKE_INTEGER_PARSER(type, name)                      \
  inline PCRE::Arg Hex(type* ptr) {                          \
    return PCRE::Arg(ptr, PCRE::Arg::parse_##name##_hex);    \
  }                                                          \
  inline PCRE::Arg Octal(type* ptr) {                        \
    return PCRE::Arg(ptr, PCRE::Arg::parse_##name##_octal);  \
  }                                                          \
  inline PCRE::Arg CRadix(type* ptr) {                       \
    return PCRE::Arg(ptr, PCRE::Arg::parse_##name##_cradix); \
  }

MAKE_INTEGER_PARSER(short,              short);
MAKE_INTEGER_PARSER(unsigned short,     ushort);
MAKE_INTEGER_PARSER(int,                int);
MAKE_INTEGER_PARSER(unsigned int,       uint);
MAKE_INTEGER_PARSER(long,               long);
MAKE_INTEGER_PARSER(unsigned long,      ulong);
MAKE_INTEGER_PARSER(long long,          longlong);
MAKE_INTEGER_PARSER(unsigned long long, ulonglong);

#undef MAKE_INTEGER_PARSER

}  // namespace re2

#endif  // UTIL_PCRE_H_
