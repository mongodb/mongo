/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Structured representation of Unicode locale IDs used with Intl functions. */

#ifndef builtin_intl_LanguageTag_h
#define builtin_intl_LanguageTag_h

#include "mozilla/Assertions.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"
#include "mozilla/TypedEnumBits.h"
#include "mozilla/Variant.h"

#include <algorithm>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <utility>

#include "js/AllocPolicy.h"
#include "js/GCAPI.h"
#include "js/Result.h"
#include "js/RootingAPI.h"
#include "js/Utility.h"
#include "js/Vector.h"

struct JS_PUBLIC_API JSContext;
class JSLinearString;
class JS_PUBLIC_API JSString;
class JS_PUBLIC_API JSTracer;

namespace js {

namespace intl {

/**
 * Return true if |language| is a valid language subtag.
 */
template <typename CharT>
bool IsStructurallyValidLanguageTag(mozilla::Span<const CharT> language);

/**
 * Return true if |script| is a valid script subtag.
 */
template <typename CharT>
bool IsStructurallyValidScriptTag(mozilla::Span<const CharT> script);

/**
 * Return true if |region| is a valid region subtag.
 */
template <typename CharT>
bool IsStructurallyValidRegionTag(mozilla::Span<const CharT> region);

#ifdef DEBUG
/**
 * Return true if |variant| is a valid variant subtag.
 */
bool IsStructurallyValidVariantTag(mozilla::Span<const char> variant);

/**
 * Return true if |extension| is a valid Unicode extension subtag.
 */
bool IsStructurallyValidUnicodeExtensionTag(
    mozilla::Span<const char> extension);

/**
 * Return true if |privateUse| is a valid private-use subtag.
 */
bool IsStructurallyValidPrivateUseTag(mozilla::Span<const char> privateUse);

#endif

template <typename CharT>
char AsciiToLowerCase(CharT c) {
  MOZ_ASSERT(mozilla::IsAscii(c));
  return mozilla::IsAsciiUppercaseAlpha(c) ? (c + 0x20) : c;
}

template <typename CharT>
char AsciiToUpperCase(CharT c) {
  MOZ_ASSERT(mozilla::IsAscii(c));
  return mozilla::IsAsciiLowercaseAlpha(c) ? (c - 0x20) : c;
}

template <typename CharT>
void AsciiToLowerCase(CharT* chars, size_t length, char* dest) {
  // Tell the analysis the |std::transform| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  char (&fn)(CharT) = AsciiToLowerCase;
  std::transform(chars, chars + length, dest, fn);
}

template <typename CharT>
void AsciiToUpperCase(CharT* chars, size_t length, char* dest) {
  // Tell the analysis the |std::transform| function can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  char (&fn)(CharT) = AsciiToUpperCase;
  std::transform(chars, chars + length, dest, fn);
}

template <typename CharT>
void AsciiToTitleCase(CharT* chars, size_t length, char* dest) {
  if (length > 0) {
    AsciiToUpperCase(chars, 1, dest);
    AsciiToLowerCase(chars + 1, length - 1, dest + 1);
  }
}

// Constants for language subtag lengths.
namespace LanguageTagLimits {

// unicode_language_subtag = alpha{2,3} | alpha{5,8} ;
static constexpr size_t LanguageLength = 8;

// unicode_script_subtag = alpha{4} ;
static constexpr size_t ScriptLength = 4;

// unicode_region_subtag = (alpha{2} | digit{3}) ;
static constexpr size_t RegionLength = 3;
static constexpr size_t AlphaRegionLength = 2;
static constexpr size_t DigitRegionLength = 3;

// key = alphanum alpha ;
static constexpr size_t UnicodeKeyLength = 2;

// tkey = alpha digit ;
static constexpr size_t TransformKeyLength = 2;

}  // namespace LanguageTagLimits

// Fixed size language subtag which is stored inline in LanguageTag.
template <size_t Length>
class LanguageTagSubtag final {
  uint8_t length_ = 0;
  char chars_[Length] = {};  // zero initialize

 public:
  LanguageTagSubtag() = default;

  LanguageTagSubtag(const LanguageTagSubtag&) = delete;
  LanguageTagSubtag& operator=(const LanguageTagSubtag&) = delete;

  size_t length() const { return length_; }
  bool missing() const { return length_ == 0; }
  bool present() const { return length_ > 0; }

  mozilla::Span<const char> span() const { return {chars_, length_}; }

  template <typename CharT>
  void set(mozilla::Span<const CharT> str) {
    MOZ_ASSERT(str.size() <= Length);
    std::copy_n(str.data(), str.size(), chars_);
    length_ = str.size();
  }

  // The toXYZCase() methods are using |Length| instead of |length()|, because
  // current compilers (tested GCC and Clang) can't infer the maximum string
  // length - even when using hints like |std::min| - and instead are emitting
  // SIMD optimized code. Using a fixed sized length avoids emitting the SIMD
  // code. (Emitting SIMD code doesn't make sense here, because the SIMD code
  // only kicks in for long strings.) A fixed length will additionally ensure
  // the compiler unrolls the loop in the case conversion code.

  void toLowerCase() { AsciiToLowerCase(chars_, Length, chars_); }

  void toUpperCase() { AsciiToUpperCase(chars_, Length, chars_); }

  void toTitleCase() { AsciiToTitleCase(chars_, Length, chars_); }

  template <size_t N>
  bool equalTo(const char (&str)[N]) const {
    static_assert(N - 1 <= Length,
                  "subtag literals must not exceed the maximum subtag length");

    return length_ == N - 1 && memcmp(chars_, str, N - 1) == 0;
  }
};

using LanguageSubtag = LanguageTagSubtag<LanguageTagLimits::LanguageLength>;
using ScriptSubtag = LanguageTagSubtag<LanguageTagLimits::ScriptLength>;
using RegionSubtag = LanguageTagSubtag<LanguageTagLimits::RegionLength>;

/**
 * Object representing a language tag.
 *
 * All subtags are already in canonicalized case.
 */
class MOZ_STACK_CLASS LanguageTag final {
  LanguageSubtag language_ = {};
  ScriptSubtag script_ = {};
  RegionSubtag region_ = {};

  using VariantsVector = Vector<JS::UniqueChars, 2>;
  using ExtensionsVector = Vector<JS::UniqueChars, 2>;

  VariantsVector variants_;
  ExtensionsVector extensions_;
  JS::UniqueChars privateuse_ = nullptr;

  friend class LanguageTagParser;

  bool canonicalizeUnicodeExtension(JSContext* cx,
                                    JS::UniqueChars& unicodeExtension);

  bool canonicalizeTransformExtension(JSContext* cx,
                                      JS::UniqueChars& transformExtension);

 public:
  static bool languageMapping(LanguageSubtag& language);
  static bool complexLanguageMapping(const LanguageSubtag& language);

 private:
  static bool scriptMapping(ScriptSubtag& script);
  static bool regionMapping(RegionSubtag& region);
  static bool complexRegionMapping(const RegionSubtag& region);

  void performComplexLanguageMappings();
  void performComplexRegionMappings();
  [[nodiscard]] bool performVariantMappings(JSContext* cx);

  [[nodiscard]] bool updateLegacyMappings(JSContext* cx);

  static bool signLanguageMapping(LanguageSubtag& language,
                                  const RegionSubtag& region);

  static const char* replaceTransformExtensionType(
      mozilla::Span<const char> key, mozilla::Span<const char> type);

 public:
  /**
   * Given a Unicode key and type, return the null-terminated preferred
   * replacement for that type if there is one, or null if there is none, e.g.
   * in effect
   * |replaceUnicodeExtensionType("ca", "islamicc") == "islamic-civil"|
   * and
   * |replaceUnicodeExtensionType("ca", "islamic-civil") == nullptr|.
   */
  static const char* replaceUnicodeExtensionType(
      mozilla::Span<const char> key, mozilla::Span<const char> type);

 public:
  explicit LanguageTag(JSContext* cx) : variants_(cx), extensions_(cx) {}

  LanguageTag(const LanguageTag&) = delete;
  LanguageTag& operator=(const LanguageTag&) = delete;

  const LanguageSubtag& language() const { return language_; }
  const ScriptSubtag& script() const { return script_; }
  const RegionSubtag& region() const { return region_; }
  const auto& variants() const { return variants_; }
  const auto& extensions() const { return extensions_; }
  const char* privateuse() const { return privateuse_.get(); }

  /**
   * Return the Unicode extension subtag or nullptr if not present.
   */
  const char* unicodeExtension() const;

 private:
  ptrdiff_t unicodeExtensionIndex() const;

 public:
  /**
   * Set the language subtag. The input must be a valid language subtag.
   */
  template <size_t N>
  void setLanguage(const char (&language)[N]) {
    mozilla::Span<const char> span(language, N - 1);
    MOZ_ASSERT(IsStructurallyValidLanguageTag(span));
    language_.set(span);
  }

  /**
   * Set the language subtag. The input must be a valid language subtag.
   */
  void setLanguage(const LanguageSubtag& language) {
    MOZ_ASSERT(IsStructurallyValidLanguageTag(language.span()));
    language_.set(language.span());
  }

  /**
   * Set the script subtag. The input must be a valid script subtag.
   */
  template <size_t N>
  void setScript(const char (&script)[N]) {
    mozilla::Span<const char> span(script, N - 1);
    MOZ_ASSERT(IsStructurallyValidScriptTag(span));
    script_.set(span);
  }

  /**
   * Set the script subtag. The input must be a valid script subtag or the empty
   * string.
   */
  void setScript(const ScriptSubtag& script) {
    MOZ_ASSERT(script.missing() || IsStructurallyValidScriptTag(script.span()));
    script_.set(script.span());
  }

  /**
   * Set the region subtag. The input must be a valid region subtag.
   */
  template <size_t N>
  void setRegion(const char (&region)[N]) {
    mozilla::Span<const char> span(region, N - 1);
    MOZ_ASSERT(IsStructurallyValidRegionTag(span));
    region_.set(span);
  }

  /**
   * Set the region subtag. The input must be a valid region subtag or the empty
   * empty string.
   */
  void setRegion(const RegionSubtag& region) {
    MOZ_ASSERT(region.missing() || IsStructurallyValidRegionTag(region.span()));
    region_.set(region.span());
  }

  /**
   * Removes all variant subtags.
   */
  void clearVariants() { variants_.clearAndFree(); }

  /**
   * Set the Unicode extension subtag. The input must be a valid Unicode
   * extension subtag.
   */
  bool setUnicodeExtension(JS::UniqueChars extension);

  /**
   * Remove any Unicode extension subtag if present.
   */
  void clearUnicodeExtension();

  /**
   * Set the private-use subtag. The input must be a valid private-use subtag
   * or nullptr.
   */
  void setPrivateuse(JS::UniqueChars privateuse) {
    MOZ_ASSERT(!privateuse ||
               IsStructurallyValidPrivateUseTag(
                   {privateuse.get(), strlen(privateuse.get())}));
    privateuse_ = std::move(privateuse);
  }

  /** Canonicalize the base-name (language, script, region, variant) subtags. */
  bool canonicalizeBaseName(JSContext* cx);

  /**
   * Canonicalize all extension subtags.
   */
  bool canonicalizeExtensions(JSContext* cx);

  /**
   * Canonicalizes the given structurally valid Unicode BCP 47 locale
   * identifier, including regularized case of subtags. For example, the
   * language tag Zh-haNS-bu-variant2-Variant1-u-ca-chinese-t-Zh-laTN-x-PRIVATE,
   * where
   *
   *     Zh             ; 2*3ALPHA
   *     -haNS          ; ["-" script]
   *     -bu            ; ["-" region]
   *     -variant2      ; *("-" variant)
   *     -Variant1
   *     -u-ca-chinese  ; *("-" extension)
   *     -t-Zh-laTN
   *     -x-PRIVATE     ; ["-" privateuse]
   *
   * becomes zh-Hans-MM-variant1-variant2-t-zh-latn-u-ca-chinese-x-private
   *
   * Spec: ECMAScript Internationalization API Specification, 6.2.3.
   */
  bool canonicalize(JSContext* cx) {
    return canonicalizeBaseName(cx) && canonicalizeExtensions(cx);
  }

  /**
   * Return the string representation of this language tag.
   */
  JSString* toString(JSContext* cx) const;

  /**
   * Return the string representation of this language tag as a null-terminated
   * C-string.
   */
  JS::UniqueChars toStringZ(JSContext* cx) const;

  /**
   * Add likely-subtags to the language tag.
   *
   * Spec: <https://www.unicode.org/reports/tr35/#Likely_Subtags>
   */
  bool addLikelySubtags(JSContext* cx);

  /**
   * Remove likely-subtags from the language tag.
   *
   * Spec: <https://www.unicode.org/reports/tr35/#Likely_Subtags>
   */
  bool removeLikelySubtags(JSContext* cx);
};

/**
 * Parser for Unicode BCP 47 locale identifiers.
 *
 * <https://unicode.org/reports/tr35/#Unicode_Language_and_Locale_Identifiers>
 */
class MOZ_STACK_CLASS LanguageTagParser final {
 public:
  // Exposed as |public| for |MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS|.
  enum class TokenKind : uint8_t {
    None = 0b000,
    Alpha = 0b001,
    Digit = 0b010,
    AlphaDigit = 0b011,
    Error = 0b100
  };

 private:
  class Token final {
    size_t index_;
    size_t length_;
    TokenKind kind_;

   public:
    Token(TokenKind kind, size_t index, size_t length)
        : index_(index), length_(length), kind_(kind) {}

    TokenKind kind() const { return kind_; }
    size_t index() const { return index_; }
    size_t length() const { return length_; }

    bool isError() const { return kind_ == TokenKind::Error; }
    bool isNone() const { return kind_ == TokenKind::None; }
    bool isAlpha() const { return kind_ == TokenKind::Alpha; }
    bool isDigit() const { return kind_ == TokenKind::Digit; }
    bool isAlphaDigit() const { return kind_ == TokenKind::AlphaDigit; }
  };

  using LocaleChars = mozilla::Variant<const JS::Latin1Char*, const char16_t*>;

  const LocaleChars& locale_;
  size_t length_;
  size_t index_ = 0;

  LanguageTagParser(const LocaleChars& locale, size_t length)
      : locale_(locale), length_(length) {}

  char16_t charAtUnchecked(size_t index) const {
    if (locale_.is<const JS::Latin1Char*>()) {
      return locale_.as<const JS::Latin1Char*>()[index];
    }
    return locale_.as<const char16_t*>()[index];
  }

  char charAt(size_t index) const {
    char16_t c = charAtUnchecked(index);
    MOZ_ASSERT(mozilla::IsAscii(c));
    return c;
  }

  // Copy the token characters into |subtag|.
  template <size_t N>
  void copyChars(const Token& tok, LanguageTagSubtag<N>& subtag) const {
    size_t index = tok.index();
    size_t length = tok.length();
    if (locale_.is<const JS::Latin1Char*>()) {
      using T = const JS::Latin1Char;
      subtag.set(mozilla::Span(locale_.as<T*>() + index, length));
    } else {
      using T = const char16_t;
      subtag.set(mozilla::Span(locale_.as<T*>() + index, length));
    }
  }

  // Create a string copy of |length| characters starting at |index|.
  JS::UniqueChars chars(JSContext* cx, size_t index, size_t length) const;

  // Create a string copy of the token characters.
  JS::UniqueChars chars(JSContext* cx, const Token& tok) const {
    return chars(cx, tok.index(), tok.length());
  }

  JS::UniqueChars extension(JSContext* cx, const Token& start,
                            const Token& end) const {
    MOZ_ASSERT(start.index() < end.index());

    size_t length = end.index() - 1 - start.index();
    return chars(cx, start.index(), length);
  }

  Token nextToken();

  // unicode_language_subtag = alpha{2,3} | alpha{5,8} ;
  //
  // Four character language subtags are not allowed in Unicode BCP 47 locale
  // identifiers. Also see the comparison to Unicode CLDR locale identifiers in
  // <https://unicode.org/reports/tr35/#BCP_47_Conformance>.
  bool isLanguage(const Token& tok) const {
    return tok.isAlpha() && ((2 <= tok.length() && tok.length() <= 3) ||
                             (5 <= tok.length() && tok.length() <= 8));
  }

  // unicode_script_subtag = alpha{4} ;
  bool isScript(const Token& tok) const {
    return tok.isAlpha() && tok.length() == 4;
  }

  // unicode_region_subtag = (alpha{2} | digit{3}) ;
  bool isRegion(const Token& tok) const {
    return (tok.isAlpha() && tok.length() == 2) ||
           (tok.isDigit() && tok.length() == 3);
  }

  // unicode_variant_subtag = (alphanum{5,8} | digit alphanum{3}) ;
  bool isVariant(const Token& tok) const {
    return (5 <= tok.length() && tok.length() <= 8) ||
           (tok.length() == 4 && mozilla::IsAsciiDigit(charAt(tok.index())));
  }

  // Returns the code unit of the first character at the given singleton token.
  // Always returns the lower case form of an alphabetical character.
  char singletonKey(const Token& tok) const {
    MOZ_ASSERT(tok.length() == 1);
    return AsciiToLowerCase(charAt(tok.index()));
  }

  // extensions = unicode_locale_extensions |
  //              transformed_extensions |
  //              other_extensions ;
  //
  // unicode_locale_extensions = sep [uU] ((sep keyword)+ |
  //                                       (sep attribute)+ (sep keyword)*) ;
  //
  // transformed_extensions = sep [tT] ((sep tlang (sep tfield)*) |
  //                                    (sep tfield)+) ;
  //
  // other_extensions = sep [alphanum-[tTuUxX]] (sep alphanum{2,8})+ ;
  bool isExtensionStart(const Token& tok) const {
    return tok.length() == 1 && singletonKey(tok) != 'x';
  }

  // other_extensions = sep [alphanum-[tTuUxX]] (sep alphanum{2,8})+ ;
  bool isOtherExtensionPart(const Token& tok) const {
    return 2 <= tok.length() && tok.length() <= 8;
  }

  // unicode_locale_extensions = sep [uU] ((sep keyword)+ |
  //                                       (sep attribute)+ (sep keyword)*) ;
  // keyword = key (sep type)? ;
  bool isUnicodeExtensionPart(const Token& tok) const {
    return isUnicodeExtensionKey(tok) || isUnicodeExtensionType(tok) ||
           isUnicodeExtensionAttribute(tok);
  }

  // attribute = alphanum{3,8} ;
  bool isUnicodeExtensionAttribute(const Token& tok) const {
    return 3 <= tok.length() && tok.length() <= 8;
  }

  // key = alphanum alpha ;
  bool isUnicodeExtensionKey(const Token& tok) const {
    return tok.length() == 2 && mozilla::IsAsciiAlpha(charAt(tok.index() + 1));
  }

  // type = alphanum{3,8} (sep alphanum{3,8})* ;
  bool isUnicodeExtensionType(const Token& tok) const {
    return 3 <= tok.length() && tok.length() <= 8;
  }

  // tkey = alpha digit ;
  bool isTransformExtensionKey(const Token& tok) const {
    return tok.length() == 2 && mozilla::IsAsciiAlpha(charAt(tok.index())) &&
           mozilla::IsAsciiDigit(charAt(tok.index() + 1));
  }

  // tvalue = (sep alphanum{3,8})+ ;
  bool isTransformExtensionPart(const Token& tok) const {
    return 3 <= tok.length() && tok.length() <= 8;
  }

  // pu_extensions = sep [xX] (sep alphanum{1,8})+ ;
  bool isPrivateUseStart(const Token& tok) const {
    return tok.length() == 1 && singletonKey(tok) == 'x';
  }

  // pu_extensions = sep [xX] (sep alphanum{1,8})+ ;
  bool isPrivateUsePart(const Token& tok) const {
    return 1 <= tok.length() && tok.length() <= 8;
  }

  // Helper function for use in |parseBaseName| and
  // |parseTlangInTransformExtension|.  Do not use this directly!
  static JS::Result<bool> internalParseBaseName(JSContext* cx,
                                                LanguageTagParser& ts,
                                                LanguageTag& tag, Token& tok);

  // Parse the `unicode_language_id` production, i.e. the
  // language/script/region/variants portion of a language tag, into |tag|.
  // |tok| must be the current token.
  static JS::Result<bool> parseBaseName(JSContext* cx, LanguageTagParser& ts,
                                        LanguageTag& tag, Token& tok) {
    return internalParseBaseName(cx, ts, tag, tok);
  }

  // Parse the `tlang` production within a parsed 't' transform extension.
  // The precise requirements for "previously parsed" are:
  //
  //   * the input begins from current token |tok| with a valid `tlang`
  //   * the `tlang` is wholly lowercase (*not* canonical case)
  //   * variant subtags in the `tlang` may contain duplicates and be
  //     unordered
  //
  // Return an error on internal failure. Otherwise, return a success value. If
  // there was no `tlang`, then |tag.language().missing()|. But if there was a
  // `tlang`, then |tag| is filled with subtags exactly as they appeared in the
  // parse input.
  static JS::Result<JS::Ok> parseTlangInTransformExtension(
      JSContext* cx, LanguageTagParser& ts, LanguageTag& tag, Token& tok) {
    MOZ_ASSERT(ts.isLanguage(tok));
    return internalParseBaseName(cx, ts, tag, tok).map([](bool parsed) {
      MOZ_ASSERT(parsed);
      return JS::Ok();
    });
  }

  friend class LanguageTag;

  class Range final {
    size_t begin_;
    size_t length_;

   public:
    Range(size_t begin, size_t length) : begin_(begin), length_(length) {}

    template <typename T>
    T* begin(T* ptr) const {
      return ptr + begin_;
    }

    size_t length() const { return length_; }
  };

  using TFieldVector = js::Vector<Range, 8>;
  using AttributesVector = js::Vector<Range, 8>;
  using KeywordsVector = js::Vector<Range, 8>;

  // Parse |extension|, which must be a validated, fully lowercase
  // `transformed_extensions` subtag, and fill |tag| and |fields| from the
  // `tlang` and `tfield` components. Data in |tag| is lowercase, consistent
  // with |extension|.
  static JS::Result<bool> parseTransformExtension(
      JSContext* cx, mozilla::Span<const char> extension, LanguageTag& tag,
      TFieldVector& fields);

  // Parse |extension|, which must be a validated, fully lowercase
  // `unicode_locale_extensions` subtag, and fill |attributes| and |keywords|
  // from the `attribute` and `keyword` components.
  static JS::Result<bool> parseUnicodeExtension(
      JSContext* cx, mozilla::Span<const char> extension,
      AttributesVector& attributes, KeywordsVector& keywords);

  static JS::Result<bool> tryParse(JSContext* cx, LocaleChars& localeChars,
                                   size_t localeLength, LanguageTag& tag);

 public:
  // Parse the input string as a language tag. Reports an error to the context
  // if the input can't be parsed completely.
  static bool parse(JSContext* cx, JSLinearString* locale, LanguageTag& tag);

  // Parse the input string as a language tag. Reports an error to the context
  // if the input can't be parsed completely.
  static bool parse(JSContext* cx, mozilla::Span<const char> locale,
                    LanguageTag& tag);

  // Parse the input string as a language tag. Returns Ok(true) if the input
  // could be completely parsed, Ok(false) if the input couldn't be parsed,
  // or Err() in case of internal error.
  static JS::Result<bool> tryParse(JSContext* cx, JSLinearString* locale,
                                   LanguageTag& tag);

  // Parse the input string as a language tag. Returns Ok(true) if the input
  // could be completely parsed, Ok(false) if the input couldn't be parsed,
  // or Err() in case of internal error.
  static JS::Result<bool> tryParse(JSContext* cx,
                                   mozilla::Span<const char> locale,
                                   LanguageTag& tag);

  // Parse the input string as the base-name parts (language, script, region,
  // variants) of a language tag. Ignores any trailing characters.
  static bool parseBaseName(JSContext* cx, mozilla::Span<const char> locale,
                            LanguageTag& tag);

  // Parse the input string as the base-name parts (language, script, region,
  // variants) of a language tag. Returns Ok(true) if the input could be
  // completely parsed, Ok(false) if the input couldn't be parsed, or Err() in
  // case of internal error.
  static JS::Result<bool> tryParseBaseName(JSContext* cx,
                                           JSLinearString* locale,
                                           LanguageTag& tag);

  // Return true iff |extension| can be parsed as a Unicode extension subtag.
  static bool canParseUnicodeExtension(mozilla::Span<const char> extension);

  // Return true iff |unicodeType| can be parsed as a Unicode extension type.
  static bool canParseUnicodeExtensionType(JSLinearString* unicodeType);
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(LanguageTagParser::TokenKind)

/**
 * Parse a string as a standalone |language| tag. If |str| is a standalone
 * language tag, store it in |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseStandaloneLanguageTag(JS::Handle<JSLinearString*> str,
                                              LanguageSubtag& result);

/**
 * Parse a string as a standalone |script| tag. If |str| is a standalone script
 * tag, store it in |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseStandaloneScriptTag(JS::Handle<JSLinearString*> str,
                                            ScriptSubtag& result);

/**
 * Parse a string as a standalone |region| tag. If |str| is a standalone region
 * tag, store it in |result| and return true. Otherwise return false.
 */
[[nodiscard]] bool ParseStandaloneRegionTag(JS::Handle<JSLinearString*> str,
                                            RegionSubtag& result);

/**
 * Parse a string as an ISO-639 language code. Return |nullptr| in the result if
 * the input could not be parsed or the canonical form of the resulting language
 * tag contains more than a single language subtag.
 */
JS::Result<JSString*> ParseStandaloneISO639LanguageTag(
    JSContext* cx, JS::Handle<JSLinearString*> str);

class UnicodeExtensionKeyword final {
  char key_[LanguageTagLimits::UnicodeKeyLength];
  JSLinearString* type_;

 public:
  using UnicodeKey = const char (&)[LanguageTagLimits::UnicodeKeyLength + 1];
  using UnicodeKeySpan =
      mozilla::Span<const char, LanguageTagLimits::UnicodeKeyLength>;

  UnicodeExtensionKeyword(UnicodeKey key, JSLinearString* type)
      : key_{key[0], key[1]}, type_(type) {}

  UnicodeKeySpan key() const { return {key_, sizeof(key_)}; }
  JSLinearString* type() const { return type_; }

  void trace(JSTracer* trc);
};

[[nodiscard]] extern bool ApplyUnicodeExtensionToTag(
    JSContext* cx, LanguageTag& tag,
    JS::HandleVector<UnicodeExtensionKeyword> keywords);

}  // namespace intl

}  // namespace js

#endif /* builtin_intl_LanguageTag_h */
