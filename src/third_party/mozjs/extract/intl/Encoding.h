// Copyright 2015-2016 Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

// Adapted from third_party/rust/encoding_c/include/encoding_rs_cpp.h, so the
// "top-level directory" in the above notice refers to
// third_party/rust/encoding_c/.

#ifndef mozilla_Encoding_h
#define mozilla_Encoding_h

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/NotNull.h"
#include "mozilla/Span.h"
#include "mozilla/Tuple.h"
#include "nsString.h"

namespace mozilla {
class Encoding;
class Decoder;
class Encoder;
};  // namespace mozilla

#define ENCODING_RS_ENCODING mozilla::Encoding
#define ENCODING_RS_NOT_NULL_CONST_ENCODING_PTR \
  mozilla::NotNull<const mozilla::Encoding*>
#define ENCODING_RS_ENCODER mozilla::Encoder
#define ENCODING_RS_DECODER mozilla::Decoder

#include "encoding_rs.h"

extern "C" {

nsresult mozilla_encoding_decode_to_nsstring(mozilla::Encoding const** encoding,
                                             uint8_t const* src, size_t src_len,
                                             nsAString* dst);

nsresult mozilla_encoding_decode_to_nsstring_with_bom_removal(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsAString* dst);

nsresult mozilla_encoding_decode_to_nsstring_without_bom_handling(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsAString* dst);

nsresult
mozilla_encoding_decode_to_nsstring_without_bom_handling_and_without_replacement(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsAString* dst);

nsresult mozilla_encoding_encode_from_utf16(mozilla::Encoding const** encoding,
                                            char16_t const* src, size_t src_len,
                                            nsACString* dst);

nsresult mozilla_encoding_decode_to_nscstring(
    mozilla::Encoding const** encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_decode_to_nscstring_with_bom_removal(
    mozilla::Encoding const* encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_decode_to_nscstring_without_bom_handling(
    mozilla::Encoding const* encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_decode_from_slice_to_nscstring_without_bom_handling(
    mozilla::Encoding const* encoding, uint8_t const* src, size_t src_len,
    nsACString* dst, size_t already_validated);

nsresult
mozilla_encoding_decode_to_nscstring_without_bom_handling_and_without_replacement(
    mozilla::Encoding const* encoding, nsACString const* src, nsACString* dst);

nsresult mozilla_encoding_encode_from_nscstring(
    mozilla::Encoding const** encoding, nsACString const* src, nsACString* dst);

}  // extern "C"

namespace mozilla {

/**
 * Return value from `Decoder`/`Encoder` to indicate that input
 * was exhausted.
 */
const uint32_t kInputEmpty = INPUT_EMPTY;

/**
 * Return value from `Decoder`/`Encoder` to indicate that output
 * space was insufficient.
 */
const uint32_t kOutputFull = OUTPUT_FULL;

/**
 * An encoding as defined in the Encoding Standard
 * (https://encoding.spec.whatwg.org/).
 *
 * See https://docs.rs/encoding_rs/ for the Rust API docs.
 *
 * An _encoding_ defines a mapping from a byte sequence to a Unicode code point
 * sequence and, in most cases, vice versa. Each encoding has a name, an output
 * encoding, and one or more labels.
 *
 * _Labels_ are ASCII-case-insensitive strings that are used to identify an
 * encoding in formats and protocols. The _name_ of the encoding is the
 * preferred label in the case appropriate for returning from the
 * `characterSet` property of the `Document` DOM interface, except for
 * the replacement encoding whose name is not one of its labels.
 *
 * The _output encoding_ is the encoding used for form submission and URL
 * parsing on Web pages in the encoding. This is UTF-8 for the replacement,
 * UTF-16LE and UTF-16BE encodings and the encoding itself for other
 * encodings.
 *
 * # Streaming vs. Non-Streaming
 *
 * When you have the entire input in a single buffer, you can use the
 * methods `Decode()`, `DecodeWithBOMRemoval()`,
 * `DecodeWithoutBOMHandling()`,
 * `DecodeWithoutBOMHandlingAndWithoutReplacement()` and
 * `Encode()`. Unlike the rest of the API (apart from the `NewDecoder()` and
 * NewEncoder()` methods), these methods perform heap allocations. You should
 * the `Decoder` and `Encoder` objects when your input is split into multiple
 * buffers or when you want to control the allocation of the output buffers.
 *
 * # Instances
 *
 * All instances of `Encoding` are statically allocated and have the process's
 * lifetime. There is precisely one unique `Encoding` instance for each
 * encoding defined in the Encoding Standard.
 *
 * To obtain a reference to a particular encoding whose identity you know at
 * compile time, use a `static` that refers to encoding. There is a `static`
 * for each encoding. The `static`s are named in all caps with hyphens
 * replaced with underscores and with `_ENCODING` appended to the
 * name. For example, if you know at compile time that you will want to
 * decode using the UTF-8 encoding, use the `UTF_8_ENCODING` `static`.
 *
 * If you don't know what encoding you need at compile time and need to
 * dynamically get an encoding by label, use `Encoding::for_label()`.
 *
 * Pointers to `Encoding` can be compared with `==` to check for the sameness
 * of two encodings.
 *
 * A pointer to a `mozilla::Encoding` in C++ is the same thing as a pointer
 * to an `encoding_rs::Encoding` in Rust. When writing FFI code, use
 * `const mozilla::Encoding*` in the C signature and
 * `*const encoding_rs::Encoding` is the corresponding Rust signature.
 */
class Encoding final {
 public:
  /**
   * Implements the _get an encoding_ algorithm
   * (https://encoding.spec.whatwg.org/#concept-encoding-get).
   *
   * If, after ASCII-lowercasing and removing leading and trailing
   * whitespace, the argument matches a label defined in the Encoding
   * Standard, `const Encoding*` representing the corresponding
   * encoding is returned. If there is no match, `nullptr` is returned.
   *
   * This is the right method to use if the action upon the method returning
   * `nullptr` is to use a fallback encoding (e.g. `WINDOWS_1252_ENCODING`)
   * instead. When the action upon the method returning `nullptr` is not to
   * proceed with a fallback but to refuse processing,
   * `ForLabelNoReplacement()` is more appropriate.
   */
  static inline const Encoding* ForLabel(Span<const char> aLabel) {
    return encoding_for_label(
        reinterpret_cast<const uint8_t*>(aLabel.Elements()), aLabel.Length());
  }

  /**
   * `nsAString` argument version. See above for docs.
   */
  static inline const Encoding* ForLabel(const nsAString& aLabel) {
    return Encoding::ForLabel(NS_ConvertUTF16toUTF8(aLabel));
  }

  /**
   * This method behaves the same as `ForLabel()`, except when `ForLabel()`
   * would return `REPLACEMENT_ENCODING`, this method returns `nullptr` instead.
   *
   * This method is useful in scenarios where a fatal error is required
   * upon invalid label, because in those cases the caller typically wishes
   * to treat the labels that map to the replacement encoding as fatal
   * errors, too.
   *
   * It is not OK to use this method when the action upon the method returning
   * `nullptr` is to use a fallback encoding (e.g. `WINDOWS_1252_ENCODING`). In
   * such a case, the `ForLabel()` method should be used instead in order to
   * avoid unsafe fallback for labels that `ForLabel()` maps to
   * `REPLACEMENT_ENCODING`.
   */
  static inline const Encoding* ForLabelNoReplacement(Span<const char> aLabel) {
    return encoding_for_label_no_replacement(
        reinterpret_cast<const uint8_t*>(aLabel.Elements()), aLabel.Length());
  }

  /**
   * `nsAString` argument version. See above for docs.
   */
  static inline const Encoding* ForLabelNoReplacement(const nsAString& aLabel) {
    return Encoding::ForLabelNoReplacement(NS_ConvertUTF16toUTF8(aLabel));
  }

  /**
   * Performs non-incremental BOM sniffing.
   *
   * The argument must either be a buffer representing the entire input
   * stream (non-streaming case) or a buffer representing at least the first
   * three bytes of the input stream (streaming case).
   *
   * Returns `MakeTuple(UTF_8_ENCODING, 3)`, `MakeTuple(UTF_16LE_ENCODING, 2)`
   * or `MakeTuple(UTF_16BE_ENCODING, 3)` if the argument starts with the
   * UTF-8, UTF-16LE or UTF-16BE BOM or `MakeTuple(nullptr, 0)` otherwise.
   */
  static inline Tuple<const Encoding*, size_t> ForBOM(
      Span<const uint8_t> aBuffer) {
    size_t len = aBuffer.Length();
    const Encoding* encoding = encoding_for_bom(aBuffer.Elements(), &len);
    return MakeTuple(encoding, len);
  }

  /**
   * Writes the name of this encoding into `aName`.
   *
   * This name is appropriate to return as-is from the DOM
   * `document.characterSet` property.
   */
  inline void Name(nsACString& aName) const {
    aName.SetLength(ENCODING_NAME_MAX_LENGTH);
    size_t length =
        encoding_name(this, reinterpret_cast<uint8_t*>(aName.BeginWriting()));
    aName.SetLength(length);  // truncation is the 64-bit case is OK
  }

  /**
   * Checks whether the _output encoding_ of this encoding can encode every
   * Unicode code point. (Only true if the output encoding is UTF-8.)
   */
  inline bool CanEncodeEverything() const {
    return encoding_can_encode_everything(this);
  }

  /**
   * Checks whether this encoding maps one byte to one Basic Multilingual
   * Plane code point (i.e. byte length equals decoded UTF-16 length) and
   * vice versa (for mappable characters).
   *
   * `true` iff this encoding is on the list of Legacy single-byte
   * encodings (https://encoding.spec.whatwg.org/#legacy-single-byte-encodings)
   * in the spec or x-user-defined.
   */
  inline bool IsSingleByte() const { return encoding_is_single_byte(this); }

  /**
   * Checks whether the bytes 0x00...0x7F map exclusively to the characters
   * U+0000...U+007F and vice versa.
   */
  inline bool IsAsciiCompatible() const {
    return encoding_is_ascii_compatible(this);
  }

  /**
   * Checks whether this is a Japanese legacy encoding.
   */
  inline bool IsJapaneseLegacy() const {
    return this == SHIFT_JIS_ENCODING || this == EUC_JP_ENCODING ||
           this == ISO_2022_JP_ENCODING;
  }

  /**
   * Returns the _output encoding_ of this encoding. This is UTF-8 for
   * UTF-16BE, UTF-16LE and replacement and the encoding itself otherwise.
   */
  inline NotNull<const mozilla::Encoding*> OutputEncoding() const {
    return WrapNotNull(encoding_output_encoding(this));
  }

  /**
   * Decode complete input to `nsACString` _with BOM sniffing_ and with
   * malformed sequences replaced with the REPLACEMENT CHARACTER when the
   * entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream).
   *
   * This method implements the (non-streaming version of) the
   * _decode_ (https://encoding.spec.whatwg.org/#decode) spec concept.
   *
   * The second item in the returned tuple is the encoding that was actually
   * used (which may differ from this encoding thanks to BOM sniffing).
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise as the first item of the
   * tuple.
   *
   * The backing buffer of the string isn't copied if the input buffer
   * is heap-allocated and decoding from UTF-8 and the input is valid
   * BOMless UTF-8, decoding from an ASCII-compatible encoding and
   * the input is valid ASCII or decoding from ISO-2022-JP and the
   * input stays in the ASCII state of ISO-2022-JP. It is OK to pass
   * the same string as both arguments.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use `NewDecoder()`
   * when decoding segmented input.
   */
  inline Tuple<nsresult, NotNull<const mozilla::Encoding*>> Decode(
      const nsACString& aBytes, nsACString& aOut) const {
    const Encoding* encoding = this;
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    nsresult rv;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      rv = mozilla_encoding_decode_to_nscstring(&encoding, &temp, out);
    } else {
      rv = mozilla_encoding_decode_to_nscstring(&encoding, bytes, out);
    }
    return MakeTuple(rv, WrapNotNull(encoding));
  }

  /**
   * Decode complete input to `nsAString` _with BOM sniffing_ and with
   * malformed sequences replaced with the REPLACEMENT CHARACTER when the
   * entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream).
   *
   * This method implements the (non-streaming version of) the
   * _decode_ (https://encoding.spec.whatwg.org/#decode) spec concept.
   *
   * The second item in the returned tuple is the encoding that was actually
   * used (which may differ from this encoding thanks to BOM sniffing).
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise as the first item of the
   * tuple.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use `NewDecoder()`
   * when decoding segmented input.
   */
  inline Tuple<nsresult, NotNull<const mozilla::Encoding*>> Decode(
      Span<const uint8_t> aBytes, nsAString& aOut) const {
    const Encoding* encoding = this;
    nsresult rv = mozilla_encoding_decode_to_nsstring(
        &encoding, aBytes.Elements(), aBytes.Length(), &aOut);
    return MakeTuple(rv, WrapNotNull(encoding));
  }

  /**
   * Decode complete input to `nsACString` _with BOM removal_ and with
   * malformed sequences replaced with the REPLACEMENT CHARACTER when the
   * entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream).
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode_
   * (https://encoding.spec.whatwg.org/#utf-8-decode) spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise.
   *
   * The backing buffer of the string isn't copied if the input buffer
   * is heap-allocated and decoding from UTF-8 and the input is valid
   * BOMless UTF-8, decoding from an ASCII-compatible encoding and
   * the input is valid ASCII or decoding from ISO-2022-JP and the
   * input stays in the ASCII state of ISO-2022-JP. It is OK to pass
   * the same string as both arguments.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithBOMRemoval()` when decoding segmented input.
   */
  inline nsresult DecodeWithBOMRemoval(const nsACString& aBytes,
                                       nsACString& aOut) const {
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      return mozilla_encoding_decode_to_nscstring_with_bom_removal(this, &temp,
                                                                   out);
    }
    return mozilla_encoding_decode_to_nscstring_with_bom_removal(this, bytes,
                                                                 out);
  }

  /**
   * Decode complete input to `nsAString` _with BOM removal_ and with
   * malformed sequences replaced with the REPLACEMENT CHARACTER when the
   * entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream).
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode_
   * (https://encoding.spec.whatwg.org/#utf-8-decode) spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithBOMRemoval()` when decoding segmented input.
   */
  inline nsresult DecodeWithBOMRemoval(Span<const uint8_t> aBytes,
                                       nsAString& aOut) const {
    return mozilla_encoding_decode_to_nsstring_with_bom_removal(
        this, aBytes.Elements(), aBytes.Length(), &aOut);
  }

  /**
   * Decode complete input to `nsACString` _without BOM handling_ and
   * with malformed sequences replaced with the REPLACEMENT CHARACTER when
   * the entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream).
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode without BOM_
   * (https://encoding.spec.whatwg.org/#utf-8-decode-without-bom) spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise.
   *
   * The backing buffer of the string isn't copied if the input buffer
   * is heap-allocated and decoding from UTF-8 and the input is valid
   * UTF-8, decoding from an ASCII-compatible encoding and the input
   * is valid ASCII or decoding from ISO-2022-JP and the input stays
   * in the ASCII state of ISO-2022-JP. It is OK to pass the same string
   * as both arguments.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithoutBOMHandling()` when decoding segmented input.
   */
  inline nsresult DecodeWithoutBOMHandling(const nsACString& aBytes,
                                           nsACString& aOut) const {
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      return mozilla_encoding_decode_to_nscstring_without_bom_handling(
          this, &temp, out);
    }
    return mozilla_encoding_decode_to_nscstring_without_bom_handling(
        this, bytes, out);
  }

  /**
   * Decode complete input to `nsAString` _without BOM handling_ and
   * with malformed sequences replaced with the REPLACEMENT CHARACTER when
   * the entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream).
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode without BOM_
   * (https://encoding.spec.whatwg.org/#utf-8-decode-without-bom) spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithoutBOMHandling()` when decoding segmented input.
   */
  inline nsresult DecodeWithoutBOMHandling(Span<const uint8_t> aBytes,
                                           nsAString& aOut) const {
    return mozilla_encoding_decode_to_nsstring_without_bom_handling(
        this, aBytes.Elements(), aBytes.Length(), &aOut);
  }

  /**
   * Decode complete input to `nsACString` _without BOM handling_ and
   * _with malformed sequences treated as fatal_ when the entire input is
   * available as a single buffer (i.e. the end of the buffer marks the end
   * of the stream).
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode without BOM or fail_
   * (https://encoding.spec.whatwg.org/#utf-8-decode-without-bom-or-fail)
   * spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_ERROR_UDEC_ILLEGALINPUT`
   * if a malformed sequence was encountered and `NS_OK` otherwise.
   *
   * The backing buffer of the string isn't copied if the input buffer
   * is heap-allocated and decoding from UTF-8 and the input is valid
   * UTF-8, decoding from an ASCII-compatible encoding and the input
   * is valid ASCII or decoding from ISO-2022-JP and the input stays
   * in the ASCII state of ISO-2022-JP. It is OK to pass the same string
   * as both arguments.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithoutBOMHandling()` when decoding segmented input.
   */
  inline nsresult DecodeWithoutBOMHandlingAndWithoutReplacement(
      const nsACString& aBytes, nsACString& aOut) const {
    const nsACString* bytes = &aBytes;
    nsACString* out = &aOut;
    if (bytes == out) {
      nsAutoCString temp(aBytes);
      return mozilla_encoding_decode_to_nscstring_without_bom_handling_and_without_replacement(
          this, &temp, out);
    }
    return mozilla_encoding_decode_to_nscstring_without_bom_handling_and_without_replacement(
        this, bytes, out);
  }

  /**
   * Decode complete input to `nsACString` _without BOM handling_ and
   * with malformed sequences replaced with the REPLACEMENT CHARACTER when
   * the entire input is available as a single buffer (i.e. the end of the
   * buffer marks the end of the stream) _asserting that a number of bytes
   * from the start are already known to be valid UTF-8_.
   *
   * The use case for this method is avoiding copying when dealing with
   * input that has a UTF-8 BOM. _When in doubt, do not use this method._
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode without BOM_
   * (https://encoding.spec.whatwg.org/#utf-8-decode-without-bom) spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_OK_HAD_REPLACEMENTS`
   * if there were malformed sequences (that were replaced with the
   * REPLACEMENT CHARACTER) and `NS_OK` otherwise.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithoutBOMHandling()` when decoding segmented input.
   *
   * # Safety
   *
   * The first `aAlreadyValidated` bytes of `aBytes` _must_ be valid UTF-8.
   * `aBytes` _must not_ alias the buffer (if any) of `aOut`.
   */
  inline nsresult DecodeWithoutBOMHandling(Span<const uint8_t> aBytes,
                                           nsACString& aOut,
                                           size_t aAlreadyValidated) const {
    return mozilla_encoding_decode_from_slice_to_nscstring_without_bom_handling(
        this, aBytes.Elements(), aBytes.Length(), &aOut, aAlreadyValidated);
  }

  /**
   * Decode complete input to `nsAString` _without BOM handling_ and
   * _with malformed sequences treated as fatal_ when the entire input is
   * available as a single buffer (i.e. the end of the buffer marks the end
   * of the stream).
   *
   * When invoked on `UTF_8`, this method implements the (non-streaming
   * version of) the _UTF-8 decode without BOM or fail_
   * (https://encoding.spec.whatwg.org/#utf-8-decode-without-bom-or-fail)
   * spec concept.
   *
   * Returns `NS_ERROR_OUT_OF_MEMORY` upon OOM, `NS_ERROR_UDEC_ILLEGALINPUT`
   * if a malformed sequence was encountered and `NS_OK` otherwise.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use
   * `NewDecoderWithoutBOMHandling()` when decoding segmented input.
   */
  inline nsresult DecodeWithoutBOMHandlingAndWithoutReplacement(
      Span<const uint8_t> aBytes, nsAString& aOut) const {
    return mozilla_encoding_decode_to_nsstring_without_bom_handling_and_without_replacement(
        this, aBytes.Elements(), aBytes.Length(), &aOut);
  }

  /**
   * Encode complete input to `nsACString` with unmappable characters
   * replaced with decimal numeric character references when the entire input
   * is available as a single buffer (i.e. the end of the buffer marks the
   * end of the stream).
   *
   * This method implements the (non-streaming version of) the
   * _encode_ (https://encoding.spec.whatwg.org/#encode) spec concept.
   *
   * The second item in the returned tuple is the encoding that was actually
   * used (which may differ from this encoding thanks to some encodings
   * having UTF-8 as their output encoding).
   *
   * The first item of the returned tuple is `NS_ERROR_UDEC_ILLEGALINPUT` if
   * the input is not valid UTF-8, `NS_ERROR_OUT_OF_MEMORY` upon OOM,
   * `NS_OK_HAD_REPLACEMENTS` if there were unmappable code points (that were
   * replaced with numeric character references) and `NS_OK` otherwise.
   *
   * The backing buffer of the string isn't copied if the input buffer
   * is heap-allocated and encoding to UTF-8 and the input is valid
   * UTF-8, encoding to an ASCII-compatible encoding and the input
   * is valid ASCII or encoding from ISO-2022-JP and the input stays
   * in the ASCII state of ISO-2022-JP. It is OK to pass the same string
   * as both arguments.
   *
   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use `NewEncoder()`
   * when encoding segmented output.
   */
  inline Tuple<nsresult, NotNull<const mozilla::Encoding*>> Encode(
      const nsACString& aString, nsACString& aOut) const {
    const Encoding* encoding = this;
    const nsACString* string = &aString;
    nsACString* out = &aOut;
    nsresult rv;
    if (string == out) {
      nsAutoCString temp(aString);
      rv = mozilla_encoding_encode_from_nscstring(&encoding, &temp, out);
    } else {
      rv = mozilla_encoding_encode_from_nscstring(&encoding, string, out);
    }
    return MakeTuple(rv, WrapNotNull(encoding));
  }

  /**
   * Encode complete input to `nsACString` with unmappable characters
   * replaced with decimal numeric character references when the entire input
   * is available as a single buffer (i.e. the end of the buffer marks the
   * end of the stream).
   *
   * This method implements the (non-streaming version of) the
   * _encode_ (https://encoding.spec.whatwg.org/#encode) spec concept.
   *
   * The second item in the returned tuple is the encoding that was actually
   * used (which may differ from this encoding thanks to some encodings
   * having UTF-8 as their output encoding).
   *
   * The first item of the returned tuple is `NS_ERROR_OUT_OF_MEMORY` upon
   * OOM, `NS_OK_HAD_REPLACEMENTS` if there were unmappable code points (that
   * were replaced with numeric character references) and `NS_OK` otherwise.

   * _Note:_ It is wrong to use this when the input buffer represents only
   * a segment of the input instead of the whole input. Use `NewEncoder()`
   * when encoding segmented output.
   */
  inline Tuple<nsresult, NotNull<const mozilla::Encoding*>> Encode(
      Span<const char16_t> aString, nsACString& aOut) const {
    const Encoding* encoding = this;
    nsresult rv = mozilla_encoding_encode_from_utf16(
        &encoding, aString.Elements(), aString.Length(), &aOut);
    return MakeTuple(rv, WrapNotNull(encoding));
  }

  /**
   * Instantiates a new decoder for this encoding with BOM sniffing enabled.
   *
   * BOM sniffing may cause the returned decoder to morph into a decoder
   * for UTF-8, UTF-16LE or UTF-16BE instead of this encoding.
   */
  inline UniquePtr<Decoder> NewDecoder() const {
    UniquePtr<Decoder> decoder(encoding_new_decoder(this));
    return decoder;
  }

  /**
   * Instantiates a new decoder for this encoding with BOM sniffing enabled
   * into memory occupied by a previously-instantiated decoder.
   *
   * BOM sniffing may cause the returned decoder to morph into a decoder
   * for UTF-8, UTF-16LE or UTF-16BE instead of this encoding.
   */
  inline void NewDecoderInto(Decoder& aDecoder) const {
    encoding_new_decoder_into(this, &aDecoder);
  }

  /**
   * Instantiates a new decoder for this encoding with BOM removal.
   *
   * If the input starts with bytes that are the BOM for this encoding,
   * those bytes are removed. However, the decoder never morphs into a
   * decoder for another encoding: A BOM for another encoding is treated as
   * (potentially malformed) input to the decoding algorithm for this
   * encoding.
   */
  inline UniquePtr<Decoder> NewDecoderWithBOMRemoval() const {
    UniquePtr<Decoder> decoder(encoding_new_decoder_with_bom_removal(this));
    return decoder;
  }

  /**
   * Instantiates a new decoder for this encoding with BOM removal
   * into memory occupied by a previously-instantiated decoder.
   *
   * If the input starts with bytes that are the BOM for this encoding,
   * those bytes are removed. However, the decoder never morphs into a
   * decoder for another encoding: A BOM for another encoding is treated as
   * (potentially malformed) input to the decoding algorithm for this
   * encoding.
   */
  inline void NewDecoderWithBOMRemovalInto(Decoder& aDecoder) const {
    encoding_new_decoder_with_bom_removal_into(this, &aDecoder);
  }

  /**
   * Instantiates a new decoder for this encoding with BOM handling disabled.
   *
   * If the input starts with bytes that look like a BOM, those bytes are
   * not treated as a BOM. (Hence, the decoder never morphs into a decoder
   * for another encoding.)
   *
   * _Note:_ If the caller has performed BOM sniffing on its own but has not
   * removed the BOM, the caller should use `NewDecoderWithBOMRemoval()`
   * instead of this method to cause the BOM to be removed.
   */
  inline UniquePtr<Decoder> NewDecoderWithoutBOMHandling() const {
    UniquePtr<Decoder> decoder(encoding_new_decoder_without_bom_handling(this));
    return decoder;
  }

  /**
   * Instantiates a new decoder for this encoding with BOM handling disabled
   * into memory occupied by a previously-instantiated decoder.
   *
   * If the input starts with bytes that look like a BOM, those bytes are
   * not treated as a BOM. (Hence, the decoder never morphs into a decoder
   * for another encoding.)
   *
   * _Note:_ If the caller has performed BOM sniffing on its own but has not
   * removed the BOM, the caller should use `NewDecoderWithBOMRemovalInto()`
   * instead of this method to cause the BOM to be removed.
   */
  inline void NewDecoderWithoutBOMHandlingInto(Decoder& aDecoder) const {
    encoding_new_decoder_without_bom_handling_into(this, &aDecoder);
  }

  /**
   * Instantiates a new encoder for the output encoding of this encoding.
   */
  inline UniquePtr<Encoder> NewEncoder() const {
    UniquePtr<Encoder> encoder(encoding_new_encoder(this));
    return encoder;
  }

  /**
   * Instantiates a new encoder for the output encoding of this encoding
   * into memory occupied by a previously-instantiated encoder.
   */
  inline void NewEncoderInto(Encoder& aEncoder) const {
    encoding_new_encoder_into(this, &aEncoder);
  }

  /**
   * Validates UTF-8.
   *
   * Returns the index of the first byte that makes the input malformed as
   * UTF-8 or the length of the input if the input is entirely valid.
   */
  static inline size_t UTF8ValidUpTo(Span<const uint8_t> aBuffer) {
    return encoding_utf8_valid_up_to(aBuffer.Elements(), aBuffer.Length());
  }

  /**
   * Validates ASCII.
   *
   * Returns the index of the first byte that makes the input malformed as
   * ASCII or the length of the input if the input is entirely valid.
   */
  static inline size_t ASCIIValidUpTo(Span<const uint8_t> aBuffer) {
    return encoding_ascii_valid_up_to(aBuffer.Elements(), aBuffer.Length());
  }

  /**
   * Validates ISO-2022-JP ASCII-state data.
   *
   * Returns the index of the first byte that makes the input not
   * representable in the ASCII state of ISO-2022-JP or the length of the
   * input if the input is entirely representable in the ASCII state of
   * ISO-2022-JP.
   */
  static inline size_t ISO2022JPASCIIValidUpTo(Span<const uint8_t> aBuffer) {
    return encoding_iso_2022_jp_ascii_valid_up_to(aBuffer.Elements(),
                                                  aBuffer.Length());
  }

 private:
  Encoding() = delete;
  Encoding(const Encoding&) = delete;
  Encoding& operator=(const Encoding&) = delete;
  ~Encoding() = delete;
};

/**
 * A converter that decodes a byte stream into Unicode according to a
 * character encoding in a streaming (incremental) manner.
 *
 * The various `Decode*` methods take an input buffer (`aSrc`) and an output
 * buffer `aDst` both of which are caller-allocated. There are variants for
 * both UTF-8 and UTF-16 output buffers.
 *
 * A `Decode*` method decodes bytes from `aSrc` into Unicode characters stored
 * into `aDst` until one of the following three things happens:
 *
 * 1. A malformed byte sequence is encountered (`*WithoutReplacement`
 *    variants only).
 *
 * 2. The output buffer has been filled so near capacity that the decoder
 *    cannot be sure that processing an additional byte of input wouldn't
 *    cause so much output that the output buffer would overflow.
 *
 * 3. All the input bytes have been processed.
 *
 * The `Decode*` method then returns tuple of a status indicating which one
 * of the three reasons to return happened, how many input bytes were read,
 * how many output code units (`uint8_t` when decoding into UTF-8 and `char16_t`
 * when decoding to UTF-16) were written, and in the case of the
 * variants performing replacement, a boolean indicating whether an error was
 * replaced with the REPLACEMENT CHARACTER during the call.
 *
 * The number of bytes "written" is what's logically written. Garbage may be
 * written in the output buffer beyond the point logically written to.
 *
 * In the case of the `*WithoutReplacement` variants, the status is a
 * `uint32_t` whose possible values are packed info about a malformed byte
 * sequence, `kOutputFull` and `kInputEmpty` corresponding to the three cases
 * listed above).
 *
 * Packed info about malformed sequences has the following format:
 * The lowest 8 bits, which can have the decimal value 0, 1, 2 or 3,
 * indicate the number of bytes that were consumed after the malformed
 * sequence and whose next-lowest 8 bits, when shifted right by 8 indicate
 * the length of the malformed byte sequence (possible decimal values 1, 2,
 * 3 or 4). The maximum possible sum of the two is 6.
 *
 * In the case of methods whose name does not end with
 * `*WithoutReplacement`, malformed sequences are automatically replaced
 * with the REPLACEMENT CHARACTER and errors do not cause the methods to
 * return early.
 *
 * When decoding to UTF-8, the output buffer must have at least 4 bytes of
 * space. When decoding to UTF-16, the output buffer must have at least two
 * UTF-16 code units (`char16_t`) of space.
 *
 * When decoding to UTF-8 without replacement, the methods are guaranteed
 * not to return indicating that more output space is needed if the length
 * of the output buffer is at least the length returned by
 * `MaxUTF8BufferLengthWithoutReplacement()`. When decoding to UTF-8
 * with replacement, the length of the output buffer that guarantees the
 * methods not to return indicating that more output space is needed is given
 * by `MaxUTF8BufferLength()`. When decoding to UTF-16 with
 * or without replacement, the length of the output buffer that guarantees
 * the methods not to return indicating that more output space is needed is
 * given by `MaxUTF16BufferLength()`.
 *
 * The output written into `aDst` is guaranteed to be valid UTF-8 or UTF-16,
 * and the output after each `Decode*` call is guaranteed to consist of
 * complete characters. (I.e. the code unit sequence for the last character is
 * guaranteed not to be split across output buffers.)
 *
 * The boolean argument `aLast` indicates that the end of the stream is reached
 * when all the bytes in `aSrc` have been consumed.
 *
 * A `Decoder` object can be used to incrementally decode a byte stream.
 *
 * During the processing of a single stream, the caller must call `Decode*`
 * zero or more times with `aLast` set to `false` and then call `Decode*` at
 * least once with `aLast` set to `true`. If `Decode*` returns `kInputEmpty`,
 * the processing of the stream has ended. Otherwise, the caller must call
 * `Decode*` again with `aLast` set to `true` (or treat a malformed result,
 * i.e. neither `kInputEmpty` nor `kOutputFull`, as a fatal error).
 *
 * Once the stream has ended, the `Decoder` object must not be used anymore.
 * That is, you need to create another one to process another stream.
 *
 * When the decoder returns `kOutputFull` or the decoder returns a malformed
 * result and the caller does not wish to treat it as a fatal error, the input
 * buffer `aSrc` may not have been completely consumed. In that case, the caller
 * must pass the unconsumed contents of `aSrc` to `Decode*` again upon the next
 * call.
 *
 * # Infinite loops
 *
 * When converting with a fixed-size output buffer whose size is too small to
 * accommodate one character of output, an infinite loop ensues. When
 * converting with a fixed-size output buffer, it generally makes sense to
 * make the buffer fairly large (e.g. couple of kilobytes).
 */
class Decoder final {
 public:
  ~Decoder() = default;
  static void operator delete(void* aDecoder) {
    decoder_free(reinterpret_cast<Decoder*>(aDecoder));
  }

  /**
   * The `Encoding` this `Decoder` is for.
   *
   * BOM sniffing can change the return value of this method during the life
   * of the decoder.
   */
  inline NotNull<const mozilla::Encoding*> Encoding() const {
    return WrapNotNull(decoder_encoding(this));
  }

  /**
   * Query the worst-case UTF-8 output size _with replacement_.
   *
   * Returns the size of the output buffer in UTF-8 code units (`uint8_t`)
   * that will not overflow given the current state of the decoder and
   * `aByteLength` number of additional input bytes when decoding with
   * errors handled by outputting a REPLACEMENT CHARACTER for each malformed
   * sequence.
   */
  inline CheckedInt<size_t> MaxUTF8BufferLength(size_t aByteLength) const {
    CheckedInt<size_t> max(decoder_max_utf8_buffer_length(this, aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Query the worst-case UTF-8 output size _without replacement_.
   *
   * Returns the size of the output buffer in UTF-8 code units (`uint8_t`)
   * that will not overflow given the current state of the decoder and
   * `aByteLength` number of additional input bytes when decoding without
   * replacement error handling.
   *
   * Note that this value may be too small for the `WithReplacement` case.
   * Use `MaxUTF8BufferLength()` for that case.
   */
  inline CheckedInt<size_t> MaxUTF8BufferLengthWithoutReplacement(
      size_t aByteLength) const {
    CheckedInt<size_t> max(
        decoder_max_utf8_buffer_length_without_replacement(this, aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Incrementally decode a byte stream into UTF-8 with malformed sequences
   * replaced with the REPLACEMENT CHARACTER.
   *
   * See the documentation of the class for documentation for `Decode*`
   * methods collectively.
   */
  inline Tuple<uint32_t, size_t, size_t, bool> DecodeToUTF8(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result =
        decoder_decode_to_utf8(this, aSrc.Elements(), &srcRead, aDst.Elements(),
                               &dstWritten, aLast, &hadReplacements);
    return MakeTuple(result, srcRead, dstWritten, hadReplacements);
  }

  /**
   * Incrementally decode a byte stream into UTF-8 _without replacement_.
   *
   * See the documentation of the class for documentation for `Decode*`
   * methods collectively.
   */
  inline Tuple<uint32_t, size_t, size_t> DecodeToUTF8WithoutReplacement(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = decoder_decode_to_utf8_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return MakeTuple(result, srcRead, dstWritten);
  }

  /**
   * Query the worst-case UTF-16 output size (with or without replacement).
   *
   * Returns the size of the output buffer in UTF-16 code units (`char16_t`)
   * that will not overflow given the current state of the decoder and
   * `aByteLength` number of additional input bytes.
   *
   * Since the REPLACEMENT CHARACTER fits into one UTF-16 code unit, the
   * return value of this method applies also in the
   * `_without_replacement` case.
   */
  inline CheckedInt<size_t> MaxUTF16BufferLength(size_t aU16Length) const {
    CheckedInt<size_t> max(decoder_max_utf16_buffer_length(this, aU16Length));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Incrementally decode a byte stream into UTF-16 with malformed sequences
   * replaced with the REPLACEMENT CHARACTER.
   *
   * See the documentation of the class for documentation for `Decode*`
   * methods collectively.
   */
  inline Tuple<uint32_t, size_t, size_t, bool> DecodeToUTF16(
      Span<const uint8_t> aSrc, Span<char16_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result = decoder_decode_to_utf16(this, aSrc.Elements(), &srcRead,
                                              aDst.Elements(), &dstWritten,
                                              aLast, &hadReplacements);
    return MakeTuple(result, srcRead, dstWritten, hadReplacements);
  }

  /**
   * Incrementally decode a byte stream into UTF-16 _without replacement_.
   *
   * See the documentation of the class for documentation for `Decode*`
   * methods collectively.
   */
  inline Tuple<uint32_t, size_t, size_t> DecodeToUTF16WithoutReplacement(
      Span<const uint8_t> aSrc, Span<char16_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = decoder_decode_to_utf16_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return MakeTuple(result, srcRead, dstWritten);
  }

  /**
   * Checks for compatibility with storing Unicode scalar values as unsigned
   * bytes taking into account the state of the decoder.
   *
   * Returns `mozilla::Nothing()` if the decoder is not in a neutral state,
   * including waiting for the BOM, or if the encoding is never
   * Latin1-byte-compatible.
   *
   * Otherwise returns the index of the first byte whose unsigned value doesn't
   * directly correspond to the decoded Unicode scalar value, or the length
   * of the input if all bytes in the input decode directly to scalar values
   * corresponding to the unsigned byte values.
   *
   * Does not change the state of the decoder.
   *
   * Do not use this unless you are supporting SpiderMonkey-style string
   * storage optimizations.
   */
  inline mozilla::Maybe<size_t> Latin1ByteCompatibleUpTo(
      Span<const uint8_t> aBuffer) const {
    size_t upTo = decoder_latin1_byte_compatible_up_to(this, aBuffer.Elements(),
                                                       aBuffer.Length());
    if (upTo == std::numeric_limits<size_t>::max()) {
      return mozilla::Nothing();
    }
    return mozilla::Some(upTo);
  }

 private:
  Decoder() = delete;
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
};

/**
 * A converter that encodes a Unicode stream into bytes according to a
 * character encoding in a streaming (incremental) manner.
 *
 * The various `Encode*` methods take an input buffer (`aSrc`) and an output
 * buffer `aDst` both of which are caller-allocated. There are variants for
 * both UTF-8 and UTF-16 input buffers.
 *
 * An `Encode*` method encode characters from `aSrc` into bytes characters
 * stored into `aDst` until one of the following three things happens:
 *
 * 1. An unmappable character is encountered (`*WithoutReplacement` variants
 *    only).
 *
 * 2. The output buffer has been filled so near capacity that the decoder
 *    cannot be sure that processing an additional character of input wouldn't
 *    cause so much output that the output buffer would overflow.
 *
 * 3. All the input characters have been processed.
 *
 * The `Encode*` method then returns tuple of a status indicating which one
 * of the three reasons to return happened, how many input code units (`uint8_t`
 * when encoding from UTF-8 and `char16_t` when encoding from UTF-16) were read,
 * how many output bytes were written, and in the case of the variants that
 * perform replacement, a boolean indicating whether an unmappable
 * character was replaced with a numeric character reference during the call.
 *
 * The number of bytes "written" is what's logically written. Garbage may be
 * written in the output buffer beyond the point logically written to.
 *
 * In the case of the methods whose name ends with
 * `*WithoutReplacement`, the status is a `uint32_t` whose possible values
 * are an unmappable code point, `kOutputFull` and `kInputEmpty` corresponding
 * to the three cases listed above).
 *
 * In the case of methods whose name does not end with
 * `*WithoutReplacement`, unmappable characters are automatically replaced
 * with the corresponding numeric character references and unmappable
 * characters do not cause the methods to return early.
 *
 * When encoding from UTF-8 without replacement, the methods are guaranteed
 * not to return indicating that more output space is needed if the length
 * of the output buffer is at least the length returned by
 * `MaxBufferLengthFromUTF8WithoutReplacement()`. When encoding from
 * UTF-8 with replacement, the length of the output buffer that guarantees the
 * methods not to return indicating that more output space is needed in the
 * absence of unmappable characters is given by
 * `MaxBufferLengthFromUTF8IfNoUnmappables()`. When encoding from
 * UTF-16 without replacement, the methods are guaranteed not to return
 * indicating that more output space is needed if the length of the output
 * buffer is at least the length returned by
 * `MaxBufferLengthFromUTF16WithoutReplacement()`. When encoding
 * from UTF-16 with replacement, the the length of the output buffer that
 * guarantees the methods not to return indicating that more output space is
 * needed in the absence of unmappable characters is given by
 * `MaxBufferLengthFromUTF16IfNoUnmappables()`.
 * When encoding with replacement, applications are not expected to size the
 * buffer for the worst case ahead of time but to resize the buffer if there
 * are unmappable characters. This is why max length queries are only available
 * for the case where there are no unmappable characters.
 *
 * When encoding from UTF-8, each `aSrc` buffer _must_ be valid UTF-8. When
 * encoding from UTF-16, unpaired surrogates in the input are treated as U+FFFD
 * REPLACEMENT CHARACTERS. Therefore, in order for astral characters not to
 * turn into a pair of REPLACEMENT CHARACTERS, the caller must ensure that
 * surrogate pairs are not split across input buffer boundaries.
 *
 * After an `Encode*` call returns, the output produced so far, taken as a
 * whole from the start of the stream, is guaranteed to consist of a valid
 * byte sequence in the target encoding. (I.e. the code unit sequence for a
 * character is guaranteed not to be split across output buffers. However, due
 * to the stateful nature of ISO-2022-JP, the stream needs to be considered
 * from the start for it to be valid. For other encodings, the validity holds
 * on a per-output buffer basis.)
 *
 * The boolean argument `aLast` indicates that the end of the stream is reached
 * when all the characters in `aSrc` have been consumed. This argument is needed
 * for ISO-2022-JP and is ignored for other encodings.
 *
 * An `Encoder` object can be used to incrementally encode a byte stream.
 *
 * During the processing of a single stream, the caller must call `Encode*`
 * zero or more times with `aLast` set to `false` and then call `Encode*` at
 * least once with `aLast` set to `true`. If `Encode*` returns `kInputEmpty`,
 * the processing of the stream has ended. Otherwise, the caller must call
 * `Encode*` again with `aLast` set to `true` (or treat an unmappable result,
 * i.e. neither `kInputEmpty` nor `kOutputFull`, as a fatal error).
 *
 * Once the stream has ended, the `Encoder` object must not be used anymore.
 * That is, you need to create another one to process another stream.
 *
 * When the encoder returns `kOutputFull` or the encoder returns an unmappable
 * result and the caller does not wish to treat it as a fatal error, the input
 * buffer `aSrc` may not have been completely consumed. In that case, the caller
 * must pass the unconsumed contents of `aSrc` to `Encode*` again upon the next
 * call.
 *
 * # Infinite loops
 *
 * When converting with a fixed-size output buffer whose size is too small to
 * accommodate one character of output, an infinite loop ensues. When
 * converting with a fixed-size output buffer, it generally makes sense to
 * make the buffer fairly large (e.g. couple of kilobytes).
 */
class Encoder final {
 public:
  ~Encoder() = default;

  static void operator delete(void* aEncoder) {
    encoder_free(reinterpret_cast<Encoder*>(aEncoder));
  }

  /**
   * The `Encoding` this `Encoder` is for.
   */
  inline NotNull<const mozilla::Encoding*> Encoding() const {
    return WrapNotNull(encoder_encoding(this));
  }

  /**
   * Returns `true` if this is an ISO-2022-JP encoder that's not in the
   * ASCII state and `false` otherwise.
   */
  inline bool HasPendingState() const {
    return encoder_has_pending_state(this);
  }

  /**
   * Query the worst-case output size when encoding from UTF-8 with
   * replacement.
   *
   * Returns the size of the output buffer in bytes that will not overflow
   * given the current state of the encoder and `aByteLength` number of
   * additional input code units if there are no unmappable characters in
   * the input.
   */
  inline CheckedInt<size_t> MaxBufferLengthFromUTF8IfNoUnmappables(
      size_t aByteLength) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf8_if_no_unmappables(this,
                                                              aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Query the worst-case output size when encoding from UTF-8 without
   * replacement.
   *
   * Returns the size of the output buffer in bytes that will not overflow
   * given the current state of the encoder and `aByteLength` number of
   * additional input code units.
   */
  inline CheckedInt<size_t> MaxBufferLengthFromUTF8WithoutReplacement(
      size_t aByteLength) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf8_without_replacement(this,
                                                                aByteLength));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Incrementally encode into byte stream from UTF-8 with unmappable
   * characters replaced with HTML (decimal) numeric character references.
   *
   * See the documentation of the class for documentation for `Encode*`
   * methods collectively.
   *
   * WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING:
   * The input ***MUST*** be valid UTF-8 or bad things happen! Unless
   * absolutely sure, use `Encoding::UTF8ValidUpTo()` to check.
   */
  inline Tuple<uint32_t, size_t, size_t, bool> EncodeFromUTF8(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result = encoder_encode_from_utf8(this, aSrc.Elements(), &srcRead,
                                               aDst.Elements(), &dstWritten,
                                               aLast, &hadReplacements);
    return MakeTuple(result, srcRead, dstWritten, hadReplacements);
  }

  /**
   * Incrementally encode into byte stream from UTF-8 _without replacement_.
   *
   * See the documentation of the class for documentation for `Encode*`
   * methods collectively.
   *
   * WARNING WARNING WARNING WARNING WARNING WARNING WARNING WARNING:
   * The input ***MUST*** be valid UTF-8 or bad things happen! Unless
   * absolutely sure, use `Encoding::UTF8ValidUpTo()` to check.
   */
  inline Tuple<uint32_t, size_t, size_t> EncodeFromUTF8WithoutReplacement(
      Span<const uint8_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = encoder_encode_from_utf8_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return MakeTuple(result, srcRead, dstWritten);
  }

  /**
   * Query the worst-case output size when encoding from UTF-16 with
   * replacement.
   *
   * Returns the size of the output buffer in bytes that will not overflow
   * given the current state of the encoder and `aU16Length` number of
   * additional input code units if there are no unmappable characters in
   * the input.
   */
  inline CheckedInt<size_t> MaxBufferLengthFromUTF16IfNoUnmappables(
      size_t aU16Length) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf16_if_no_unmappables(this,
                                                               aU16Length));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Query the worst-case output size when encoding from UTF-16 without
   * replacement.
   *
   * Returns the size of the output buffer in bytes that will not overflow
   * given the current state of the encoder and `aU16Length` number of
   * additional input code units.
   */
  inline CheckedInt<size_t> MaxBufferLengthFromUTF16WithoutReplacement(
      size_t aU16Length) const {
    CheckedInt<size_t> max(
        encoder_max_buffer_length_from_utf16_without_replacement(this,
                                                                 aU16Length));
    if (max.value() == std::numeric_limits<size_t>::max()) {
      // Mark invalid by overflowing
      max++;
      MOZ_ASSERT(!max.isValid());
    }
    return max;
  }

  /**
   * Incrementally encode into byte stream from UTF-16 with unmappable
   * characters replaced with HTML (decimal) numeric character references.
   *
   * See the documentation of the class for documentation for `Encode*`
   * methods collectively.
   */
  inline Tuple<uint32_t, size_t, size_t, bool> EncodeFromUTF16(
      Span<const char16_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    bool hadReplacements;
    uint32_t result = encoder_encode_from_utf16(this, aSrc.Elements(), &srcRead,
                                                aDst.Elements(), &dstWritten,
                                                aLast, &hadReplacements);
    return MakeTuple(result, srcRead, dstWritten, hadReplacements);
  }

  /**
   * Incrementally encode into byte stream from UTF-16 _without replacement_.
   *
   * See the documentation of the class for documentation for `Encode*`
   * methods collectively.
   */
  inline Tuple<uint32_t, size_t, size_t> EncodeFromUTF16WithoutReplacement(
      Span<const char16_t> aSrc, Span<uint8_t> aDst, bool aLast) {
    size_t srcRead = aSrc.Length();
    size_t dstWritten = aDst.Length();
    uint32_t result = encoder_encode_from_utf16_without_replacement(
        this, aSrc.Elements(), &srcRead, aDst.Elements(), &dstWritten, aLast);
    return MakeTuple(result, srcRead, dstWritten);
  }

 private:
  Encoder() = delete;
  Encoder(const Encoder&) = delete;
  Encoder& operator=(const Encoder&) = delete;
};

};  // namespace mozilla

#endif  // mozilla_Encoding_h
