// Copyright 2019 Mozilla Foundation. See the COPYRIGHT
// file at the top-level directory of this distribution.
//
// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// https://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or https://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

// Mostly copied and pasted from
// third_party/rust/chardetng/src/lib.rs , so
// "top-level directory of this distribution" above refers to
// third_party/rust/chardetng/

#ifndef mozilla_EncodingDetector_h
#define mozilla_EncodingDetector_h

#include "mozilla/Encoding.h"

namespace mozilla {
class EncodingDetector;
};  // namespace mozilla

#define CHARDETNG_ENCODING_DETECTOR mozilla::EncodingDetector

#include "chardetng.h"

namespace mozilla {

/**
 * A Web browser-oriented detector for guessing what character
 * encoding a stream of bytes is encoded in.
 *
 * The bytes are fed to the detector incrementally using the `feed`
 * method. The current guess of the detector can be queried using
 * the `guess` method. The guessing parameters are arguments to the
 * `guess` method rather than arguments to the constructor in order
 * to enable the application to check if the arguments affect the
 * guessing outcome. (The specific use case is to disable UI for
 * re-running the detector with UTF-8 allowed and the top-level
 * domain name ignored if those arguments don't change the guess.)
 */
class EncodingDetector final {
 public:
  ~EncodingDetector() = default;

  static void operator delete(void* aDetector) {
    chardetng_encoding_detector_free(
        reinterpret_cast<EncodingDetector*>(aDetector));
  }

  /**
   * Creates a new instance of the detector.
   */
  static inline UniquePtr<EncodingDetector> Create() {
    UniquePtr<EncodingDetector> detector(chardetng_encoding_detector_new());
    return detector;
  }

  /**
   * Queries whether the TLD is considered non-generic and could affect the
   * guess.
   */
  static inline bool TldMayAffectGuess(Span<const char> aTLD) {
    return chardetng_encoding_detector_tld_may_affect_guess(aTLD.Elements(),
                                                            aTLD.Length());
  }

  /**
   * Inform the detector of a chunk of input.
   *
   * The byte stream is represented as a sequence of calls to this
   * method such that the concatenation of the arguments to this
   * method form the byte stream. It does not matter how the application
   * chooses to chunk the stream. It is OK to call this method with
   * a zero-length byte slice.
   *
   * The end of the stream is indicated by calling this method with
   * `aLast` set to `true`. In that case, the end of the stream is
   * considered to occur after the last byte of the `aBuffer` (which
   * may be zero-length) passed in the same call. Once this method
   * has been called with `last` set to `true` this method must not
   * be called again.
   *
   * If you want to perform detection on just the prefix of a longer
   * stream, do not pass `aLast=true` after the prefix if the stream
   * actually still continues.
   *
   * Returns `true` if after processing `aBuffer` the stream has
   * contained at least one non-ASCII byte and `false` if only
   * ASCII has been seen so far.
   *
   * # Panics
   *
   * If this method has previously been called with `aLast` set to `true`.
   */
  inline bool Feed(Span<const uint8_t> aBuffer, bool aLast) {
    return chardetng_encoding_detector_feed(this, aBuffer.Elements(),
                                            aBuffer.Length(), aLast);
  }

  /**
   * Guess the encoding given the bytes pushed to the detector so far
   * (via `Feed()`), the top-level domain name from which the bytes were
   * loaded, and an indication of whether to consider UTF-8 as a permissible
   * guess.
   *
   * The `aTld` argument takes the rightmost DNS label of the hostname of the
   * host the stream was loaded from in lower-case ASCII form. That is, if
   * the label is an internationalized top-level domain name, it must be
   * provided in its Punycode form. If the TLD that the stream was loaded
   * from is unavalable, an empty `Spane` may be passed instead, which is
   * equivalent to passing a `Span` for "com".
   *
   * If the `aAllowUTF8` argument is set to `false`, the return value of
   * this method won't be `UTF_8_ENCODING`. When performing detection
   * on `text/html` on non-`file:` URLs, Web browsers must pass `false`,
   * unless the user has taken a specific contextual action to request an
   * override. This way, Web developers cannot start depending on UTF-8
   * detection. Such reliance would make the Web Platform more brittle.
   *
   * Returns the guessed encoding.
   *
   * # Panics
   *
   * If `aTld` contains non-ASCII, period, or upper-case letters. (The panic
   * condition is intentionally limited to signs of failing to extract the
   * label correctly, failing to provide it in its Punycode form, and failure
   * to lower-case it. Full DNS label validation is intentionally not performed
   * to avoid panics when the reality doesn't match the specs.)
   */
  inline mozilla::NotNull<const mozilla::Encoding*> Guess(
      Span<const char> aTLD, bool aAllowUTF8) const {
    return WrapNotNull(chardetng_encoding_detector_guess(
        this, aTLD.Elements(), aTLD.Length(), aAllowUTF8));
  }

 private:
  EncodingDetector() = delete;
  EncodingDetector(const EncodingDetector&) = delete;
  EncodingDetector& operator=(const EncodingDetector&) = delete;
};

};  // namespace mozilla

#endif  // mozilla_EncodingDetector_h
