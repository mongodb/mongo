/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A JSON pretty-printer class. */

// A typical JSON-writing library requires you to first build up a data
// structure that represents a JSON object and then serialize it (to file, or
// somewhere else). This approach makes for a clean API, but building the data
// structure takes up memory. Sometimes that isn't desirable, such as when the
// JSON data is produced for memory reporting.
//
// The JSONWriter class instead allows JSON data to be written out
// incrementally without building up large data structures.
//
// The API is slightly uglier than you would see in a typical JSON-writing
// library, but still fairly easy to use. It's possible to generate invalid
// JSON with JSONWriter, but typically the most basic testing will identify any
// such problems.
//
// Similarly, there are no RAII facilities for automatically closing objects
// and arrays. These would be nice if you are generating all your code within
// nested functions, but in other cases you'd have to maintain an explicit
// stack of RAII objects and manually unwind it, which is no better than just
// calling "end" functions. Furthermore, the consequences of forgetting to
// close an object or array are obvious and, again, will be identified via
// basic testing, unlike other cases where RAII is typically used (e.g. smart
// pointers) and the consequences of defects are more subtle.
//
// Importantly, the class does solve the two hard problems of JSON
// pretty-printing, which are (a) correctly escaping strings, and (b) adding
// appropriate indentation and commas between items.
//
// By default, every property is placed on its own line. However, it is
// possible to request that objects and arrays be placed entirely on a single
// line, which can reduce output size significantly in some cases.
//
// Strings used (for property names and string property values) are |const
// char*| throughout, and can be ASCII or UTF-8.
//
// EXAMPLE
// -------
// Assume that |MyWriteFunc| is a class that implements |JSONWriteFunc|. The
// following code:
//
//   JSONWriter w(MakeUnique<MyWriteFunc>());
//   w.Start();
//   {
//     w.NullProperty("null");
//     w.BoolProperty("bool", true);
//     w.IntProperty("int", 1);
//     w.StartArrayProperty("array");
//     {
//       w.StringElement("string");
//       w.StartObjectElement();
//       {
//         w.DoubleProperty("double", 3.4);
//         w.StartArrayProperty("single-line array", w.SingleLineStyle);
//         {
//           w.IntElement(1);
//           w.StartObjectElement();  // SingleLineStyle is inherited from
//           w.EndObjectElement();    //   above for this collection
//         }
//         w.EndArray();
//       }
//       w.EndObjectElement();
//     }
//     w.EndArrayProperty();
//   }
//   w.End();
//
// will produce pretty-printed output for the following JSON object:
//
//  {
//   "null": null,
//   "bool": true,
//   "int": 1,
//   "array": [
//    "string",
//    {
//     "double": 3.4,
//     "single-line array": [1, {}]
//    }
//   ]
//  }
//
// The nesting in the example code is obviously optional, but can aid
// readability.

#ifndef mozilla_JSONWriter_h
#define mozilla_JSONWriter_h

#include "double-conversion/double-conversion.h"
#include "mozilla/Assertions.h"
#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Span.h"
#include "mozilla/Sprintf.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/Vector.h"

#include <utility>

namespace mozilla {

// A quasi-functor for JSONWriter. We don't use a true functor because that
// requires templatizing JSONWriter, and the templatization seeps to lots of
// places we don't want it to.
class JSONWriteFunc {
 public:
  virtual void Write(const Span<const char>& aStr) = 0;
  virtual ~JSONWriteFunc() = default;
};

// Ideally this would be within |EscapedString| but when compiling with GCC
// on Linux that caused link errors, whereas this formulation didn't.
namespace detail {
extern MFBT_DATA const char gTwoCharEscapes[256];
}  // namespace detail

class JSONWriter {
  // From http://www.ietf.org/rfc/rfc4627.txt:
  //
  //   "All Unicode characters may be placed within the quotation marks except
  //   for the characters that must be escaped: quotation mark, reverse
  //   solidus, and the control characters (U+0000 through U+001F)."
  //
  // This implementation uses two-char escape sequences where possible, namely:
  //
  //   \", \\, \b, \f, \n, \r, \t
  //
  // All control characters not in the above list are represented with a
  // six-char escape sequence, e.g. '\u000b' (a.k.a. '\v').
  //
  class EscapedString {
    // `mStringSpan` initially points at the user-provided string. If that
    // string needs escaping, `mStringSpan` will point at `mOwnedStr` below.
    Span<const char> mStringSpan;
    // String storage in case escaping is actually needed, null otherwise.
    UniquePtr<char[]> mOwnedStr;

    void CheckInvariants() const {
      // Either there was no escaping so `mOwnedStr` is null, or escaping was
      // needed, in which case `mStringSpan` should point at `mOwnedStr`.
      MOZ_ASSERT(!mOwnedStr || mStringSpan.data() == mOwnedStr.get());
    }

    static char hexDigitToAsciiChar(uint8_t u) {
      u = u & 0xf;
      return u < 10 ? '0' + u : 'a' + (u - 10);
    }

   public:
    explicit EscapedString(const Span<const char>& aStr) : mStringSpan(aStr) {
      // First, see if we need to modify the string.
      size_t nExtra = 0;
      for (const char& c : aStr) {
        // ensure it can't be interpreted as negative
        uint8_t u = static_cast<uint8_t>(c);
        if (u == 0) {
          // Null terminator within the span, assume we may have been given a
          // span to a buffer that contains a null-terminated string in it.
          // We need to truncate the Span so that it doesn't include this null
          // terminator and anything past it; Either we will return it as-is, or
          // processing should stop there.
          mStringSpan = mStringSpan.First(&c - mStringSpan.data());
          break;
        }
        if (detail::gTwoCharEscapes[u]) {
          nExtra += 1;
        } else if (u <= 0x1f) {
          nExtra += 5;
        }
      }

      // Note: Don't use `aStr` anymore, as it could contain a null terminator;
      // use the correctly-sized `mStringSpan` instead.

      if (nExtra == 0) {
        // No escapes needed. mStringSpan already points at the original string.
        CheckInvariants();
        return;
      }

      // Escapes are needed. We'll create a new string.
      mOwnedStr = MakeUnique<char[]>(mStringSpan.Length() + nExtra);

      size_t i = 0;
      for (const char c : mStringSpan) {
        // ensure it can't be interpreted as negative
        uint8_t u = static_cast<uint8_t>(c);
        MOZ_ASSERT(u != 0, "Null terminator should have been handled above");
        if (detail::gTwoCharEscapes[u]) {
          mOwnedStr[i++] = '\\';
          mOwnedStr[i++] = detail::gTwoCharEscapes[u];
        } else if (u <= 0x1f) {
          mOwnedStr[i++] = '\\';
          mOwnedStr[i++] = 'u';
          mOwnedStr[i++] = '0';
          mOwnedStr[i++] = '0';
          mOwnedStr[i++] = hexDigitToAsciiChar((u & 0x00f0) >> 4);
          mOwnedStr[i++] = hexDigitToAsciiChar(u & 0x000f);
        } else {
          mOwnedStr[i++] = u;
        }
      }
      MOZ_ASSERT(i == mStringSpan.Length() + nExtra);
      mStringSpan = Span<const char>(mOwnedStr.get(), i);
      CheckInvariants();
    }

    explicit EscapedString(const char* aStr) = delete;

    const Span<const char>& SpanRef() const { return mStringSpan; }
  };

 public:
  // Collections (objects and arrays) are printed in a multi-line style by
  // default. This can be changed to a single-line style if SingleLineStyle is
  // specified. If a collection is printed in single-line style, every nested
  // collection within it is also printed in single-line style, even if
  // multi-line style is requested.
  // If SingleLineStyle is set in the constructer, all JSON whitespace is
  // eliminated, including spaces after colons and commas, for the most compact
  // encoding possible.
  enum CollectionStyle {
    MultiLineStyle,  // the default
    SingleLineStyle
  };

 protected:
  static constexpr Span<const char> scArrayBeginString = MakeStringSpan("[");
  static constexpr Span<const char> scArrayEndString = MakeStringSpan("]");
  static constexpr Span<const char> scCommaString = MakeStringSpan(",");
  static constexpr Span<const char> scEmptyString = MakeStringSpan("");
  static constexpr Span<const char> scFalseString = MakeStringSpan("false");
  static constexpr Span<const char> scNewLineString = MakeStringSpan("\n");
  static constexpr Span<const char> scNullString = MakeStringSpan("null");
  static constexpr Span<const char> scObjectBeginString = MakeStringSpan("{");
  static constexpr Span<const char> scObjectEndString = MakeStringSpan("}");
  static constexpr Span<const char> scPropertyBeginString =
      MakeStringSpan("\"");
  static constexpr Span<const char> scPropertyEndString = MakeStringSpan("\":");
  static constexpr Span<const char> scQuoteString = MakeStringSpan("\"");
  static constexpr Span<const char> scSpaceString = MakeStringSpan(" ");
  static constexpr Span<const char> scTopObjectBeginString =
      MakeStringSpan("{");
  static constexpr Span<const char> scTopObjectEndString = MakeStringSpan("}");
  static constexpr Span<const char> scTrueString = MakeStringSpan("true");

  JSONWriteFunc& mWriter;
  const UniquePtr<JSONWriteFunc> mMaybeOwnedWriter;
  Vector<bool, 8> mNeedComma;     // do we need a comma at depth N?
  Vector<bool, 8> mNeedNewlines;  // do we need newlines at depth N?
  size_t mDepth;                  // the current nesting depth

  void Indent() {
    for (size_t i = 0; i < mDepth; i++) {
      mWriter.Write(scSpaceString);
    }
  }

  // Adds whatever is necessary (maybe a comma, and then a newline and
  // whitespace) to separate an item (property or element) from what's come
  // before.
  void Separator() {
    if (mNeedComma[mDepth]) {
      mWriter.Write(scCommaString);
    }
    if (mDepth > 0 && mNeedNewlines[mDepth]) {
      mWriter.Write(scNewLineString);
      Indent();
    } else if (mNeedComma[mDepth] && mNeedNewlines[0]) {
      mWriter.Write(scSpaceString);
    }
  }

  void PropertyNameAndColon(const Span<const char>& aName) {
    mWriter.Write(scPropertyBeginString);
    mWriter.Write(EscapedString(aName).SpanRef());
    mWriter.Write(scPropertyEndString);
    if (mNeedNewlines[0]) {
      mWriter.Write(scSpaceString);
    }
  }

  void Scalar(const Span<const char>& aMaybePropertyName,
              const Span<const char>& aStringValue) {
    Separator();
    if (!aMaybePropertyName.empty()) {
      PropertyNameAndColon(aMaybePropertyName);
    }
    mWriter.Write(aStringValue);
    mNeedComma[mDepth] = true;
  }

  void QuotedScalar(const Span<const char>& aMaybePropertyName,
                    const Span<const char>& aStringValue) {
    Separator();
    if (!aMaybePropertyName.empty()) {
      PropertyNameAndColon(aMaybePropertyName);
    }
    mWriter.Write(scQuoteString);
    mWriter.Write(aStringValue);
    mWriter.Write(scQuoteString);
    mNeedComma[mDepth] = true;
  }

  void NewVectorEntries(bool aNeedNewLines) {
    // If these tiny allocations OOM we might as well just crash because we
    // must be in serious memory trouble.
    MOZ_RELEASE_ASSERT(mNeedComma.resizeUninitialized(mDepth + 1));
    MOZ_RELEASE_ASSERT(mNeedNewlines.resizeUninitialized(mDepth + 1));
    mNeedComma[mDepth] = false;
    mNeedNewlines[mDepth] = aNeedNewLines;
  }

  void StartCollection(const Span<const char>& aMaybePropertyName,
                       const Span<const char>& aStartChar,
                       CollectionStyle aStyle = MultiLineStyle) {
    Separator();
    if (!aMaybePropertyName.empty()) {
      PropertyNameAndColon(aMaybePropertyName);
    }
    mWriter.Write(aStartChar);
    mNeedComma[mDepth] = true;
    mDepth++;
    NewVectorEntries(mNeedNewlines[mDepth - 1] && aStyle == MultiLineStyle);
  }

  // Adds the whitespace and closing char necessary to end a collection.
  void EndCollection(const Span<const char>& aEndChar) {
    MOZ_ASSERT(mDepth > 0);
    if (mNeedNewlines[mDepth]) {
      mWriter.Write(scNewLineString);
      mDepth--;
      Indent();
    } else {
      mDepth--;
    }
    mWriter.Write(aEndChar);
  }

 public:
  explicit JSONWriter(JSONWriteFunc& aWriter,
                      CollectionStyle aStyle = MultiLineStyle)
      : mWriter(aWriter), mNeedComma(), mNeedNewlines(), mDepth(0) {
    NewVectorEntries(aStyle == MultiLineStyle);
  }

  explicit JSONWriter(UniquePtr<JSONWriteFunc> aWriter,
                      CollectionStyle aStyle = MultiLineStyle)
      : mWriter(*aWriter),
        mMaybeOwnedWriter(std::move(aWriter)),
        mNeedComma(),
        mNeedNewlines(),
        mDepth(0) {
    MOZ_RELEASE_ASSERT(
        mMaybeOwnedWriter,
        "JSONWriter must be given a non-null UniquePtr<JSONWriteFunc>");
    NewVectorEntries(aStyle == MultiLineStyle);
  }

  // Returns the JSONWriteFunc passed in at creation, for temporary use. The
  // JSONWriter object still owns the JSONWriteFunc.
  JSONWriteFunc& WriteFunc() const { return mWriter; }

  // For all the following functions, the "Prints:" comment indicates what the
  // basic output looks like. However, it doesn't indicate the whitespace and
  // trailing commas, which are automatically added as required.
  //
  // All property names and string properties are escaped as necessary.

  // Prints: {
  void Start(CollectionStyle aStyle = MultiLineStyle) {
    StartCollection(scEmptyString, scTopObjectBeginString, aStyle);
  }

  // Prints: } and final newline.
  void End() {
    EndCollection(scTopObjectEndString);
    if (mNeedNewlines[mDepth]) {
      mWriter.Write(scNewLineString);
    }
  }

  // Prints: "<aName>": null
  void NullProperty(const Span<const char>& aName) {
    Scalar(aName, scNullString);
  }

  template <size_t N>
  void NullProperty(const char (&aName)[N]) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    NullProperty(Span<const char>(aName, N));
  }

  // Prints: null
  void NullElement() { NullProperty(scEmptyString); }

  // Prints: "<aName>": <aBool>
  void BoolProperty(const Span<const char>& aName, bool aBool) {
    Scalar(aName, aBool ? scTrueString : scFalseString);
  }

  template <size_t N>
  void BoolProperty(const char (&aName)[N], bool aBool) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    BoolProperty(Span<const char>(aName, N), aBool);
  }

  // Prints: <aBool>
  void BoolElement(bool aBool) { BoolProperty(scEmptyString, aBool); }

  // Prints: "<aName>": <aInt>
  void IntProperty(const Span<const char>& aName, int64_t aInt) {
    char buf[64];
    int len = SprintfLiteral(buf, "%" PRId64, aInt);
    MOZ_RELEASE_ASSERT(len > 0);
    Scalar(aName, Span<const char>(buf, size_t(len)));
  }

  template <size_t N>
  void IntProperty(const char (&aName)[N], int64_t aInt) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    IntProperty(Span<const char>(aName, N), aInt);
  }

  // Prints: <aInt>
  void IntElement(int64_t aInt) { IntProperty(scEmptyString, aInt); }

  // Prints: "<aName>": <aDouble>
  void DoubleProperty(const Span<const char>& aName, double aDouble) {
    static const size_t buflen = 64;
    char buf[buflen];
    const double_conversion::DoubleToStringConverter& converter =
        double_conversion::DoubleToStringConverter::EcmaScriptConverter();
    double_conversion::StringBuilder builder(buf, buflen);
    converter.ToShortest(aDouble, &builder);
    // TODO: The builder should know the length?!
    Scalar(aName, MakeStringSpan(builder.Finalize()));
  }

  template <size_t N>
  void DoubleProperty(const char (&aName)[N], double aDouble) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    DoubleProperty(Span<const char>(aName, N), aDouble);
  }

  // Prints: <aDouble>
  void DoubleElement(double aDouble) { DoubleProperty(scEmptyString, aDouble); }

  // Prints: "<aName>": "<aStr>"
  void StringProperty(const Span<const char>& aName,
                      const Span<const char>& aStr) {
    QuotedScalar(aName, EscapedString(aStr).SpanRef());
  }

  template <size_t NN>
  void StringProperty(const char (&aName)[NN], const Span<const char>& aStr) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    StringProperty(Span<const char>(aName, NN), aStr);
  }

  template <size_t SN>
  void StringProperty(const Span<const char>& aName, const char (&aStr)[SN]) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    StringProperty(aName, Span<const char>(aStr, SN));
  }

  template <size_t NN, size_t SN>
  void StringProperty(const char (&aName)[NN], const char (&aStr)[SN]) {
    // Keep null terminators from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    StringProperty(Span<const char>(aName, NN), Span<const char>(aStr, SN));
  }

  // Prints: "<aStr>"
  void StringElement(const Span<const char>& aStr) {
    StringProperty(scEmptyString, aStr);
  }

  template <size_t N>
  void StringElement(const char (&aName)[N]) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    StringElement(Span<const char>(aName, N));
  }

  // Prints: "<aName>": [
  void StartArrayProperty(const Span<const char>& aName,
                          CollectionStyle aStyle = MultiLineStyle) {
    StartCollection(aName, scArrayBeginString, aStyle);
  }

  template <size_t N>
  void StartArrayProperty(const char (&aName)[N],
                          CollectionStyle aStyle = MultiLineStyle) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    StartArrayProperty(Span<const char>(aName, N), aStyle);
  }

  // Prints: [
  void StartArrayElement(CollectionStyle aStyle = MultiLineStyle) {
    StartArrayProperty(scEmptyString, aStyle);
  }

  // Prints: ]
  void EndArray() { EndCollection(scArrayEndString); }

  // Prints: "<aName>": {
  void StartObjectProperty(const Span<const char>& aName,
                           CollectionStyle aStyle = MultiLineStyle) {
    StartCollection(aName, scObjectBeginString, aStyle);
  }

  template <size_t N>
  void StartObjectProperty(const char (&aName)[N],
                           CollectionStyle aStyle = MultiLineStyle) {
    // Keep null terminator from literal strings, will be removed by
    // EscapedString. This way C buffer arrays can be used as well.
    StartObjectProperty(Span<const char>(aName, N), aStyle);
  }

  // Prints: {
  void StartObjectElement(CollectionStyle aStyle = MultiLineStyle) {
    StartObjectProperty(scEmptyString, aStyle);
  }

  // Prints: }
  void EndObject() { EndCollection(scObjectEndString); }
};

}  // namespace mozilla

#endif /* mozilla_JSONWriter_h */
