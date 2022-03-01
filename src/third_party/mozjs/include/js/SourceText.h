/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * SourceText encapsulates a count of char16_t (UTF-16) or Utf8Unit (UTF-8)
 * code units (note: code *units*, not bytes or code points) and those code
 * units ("source units").  (Latin-1 is not supported: all places where Latin-1
 * must be compiled first convert to a supported encoding.)
 *
 * A SourceText either observes without owning, or takes ownership of, source
 * units passed to |SourceText::init|.  Thus SourceText can be used to
 * efficiently avoid copying.
 *
 * Rules for use:
 *
 *  1) The passed-in source units must be allocated with js_malloc(),
 *     js_calloc(), or js_realloc() if |SourceText::init| is instructed to take
 *     ownership of the source units.
 *  2) If |SourceText::init| merely borrows the source units, the user must
 *     keep them alive until associated JS compilation is complete.
 *  3) Code that calls |SourceText::take{Chars,Units}()| must keep the source
 *     units alive until JS compilation completes.  Normally only the JS engine
 *     should call |SourceText::take{Chars,Units}()|.
 *  4) Use the appropriate SourceText parameterization depending on the source
 *     units encoding.
 *
 * Example use:
 *
 *    size_t length = 512;
 *    char16_t* chars = js_pod_malloc<char16_t>(length);
 *    if (!chars) {
 *        JS_ReportOutOfMemory(cx);
 *        return false;
 *    }
 *    JS::SourceText<char16_t> srcBuf;
 *    if (!srcBuf.init(cx, chars, length, JS::SourceOwnership::TakeOwnership)) {
 *        return false;
 *    }
 *    JS::Rooted<JSScript*> script(cx);
 *    if (!JS::Compile(cx, options, srcBuf, &script)) {
 *        return false;
 *    }
 */

#ifndef js_SourceText_h
#define js_SourceText_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_COLD, MOZ_IS_CLASS_INIT
#include "mozilla/Likely.h"      // MOZ_UNLIKELY

#include <stddef.h>     // size_t
#include <stdint.h>     // UINT32_MAX
#include <type_traits>  // std::conditional_t, std::is_same_v

#include "js/UniquePtr.h"  // js::UniquePtr
#include "js/Utility.h"    // JS::FreePolicy

namespace mozilla {
union Utf8Unit;
}

namespace JS {

namespace detail {

MOZ_COLD extern JS_PUBLIC_API void ReportSourceTooLong(JSContext* cx);

}  // namespace detail

enum class SourceOwnership {
  Borrowed,
  TakeOwnership,
};

template <typename Unit>
class SourceText final {
 private:
  static_assert(std::is_same_v<Unit, mozilla::Utf8Unit> ||
                    std::is_same_v<Unit, char16_t>,
                "Unit must be either char16_t or Utf8Unit for "
                "SourceText<Unit>");

  /** |char16_t| or |Utf8Unit| source units of uncertain validity. */
  const Unit* units_ = nullptr;

  /** The length in code units of |units_|. */
  uint32_t length_ = 0;

  /**
   * Whether this owns |units_| or merely observes source units owned by some
   * other object.
   */
  bool ownsUnits_ = false;

 public:
  // A C++ character type that can represent the source units -- suitable for
  // passing to C++ string functions.
  using CharT =
      std::conditional_t<std::is_same_v<Unit, char16_t>, char16_t, char>;

 public:
  /**
   * Construct a SourceText.  It must be initialized using |init()| before it
   * can be used as compilation source text.
   */
  SourceText() = default;

  /**
   * Construct a SourceText from contents extracted from |other|.  This
   * SourceText will then act exactly as |other| would have acted, had it
   * not been passed to this function.  |other| will return to its default-
   * constructed state and must have |init()| called on it to use it.
   */
  SourceText(SourceText&& other)
      : units_(other.units_),
        length_(other.length_),
        ownsUnits_(other.ownsUnits_) {
    other.units_ = nullptr;
    other.length_ = 0;
    other.ownsUnits_ = false;
  }

  ~SourceText() {
    if (ownsUnits_) {
      js_free(const_cast<Unit*>(units_));
    }
  }

  /**
   * Initialize this with source unit data: |char16_t| for UTF-16 source
   * units, or |Utf8Unit| for UTF-8 source units.
   *
   * If |ownership == TakeOwnership|, *this function* takes ownership of
   * |units|, *even if* this function fails, and you MUST NOT free |units|
   * yourself.  This single-owner-friendly approach reduces risk of leaks on
   * failure.
   *
   * |units| may be null if |unitsLength == 0|; if so, this will silently be
   * initialized using non-null, unowned units.
   */
  [[nodiscard]] MOZ_IS_CLASS_INIT bool init(JSContext* cx, const Unit* units,
                                            size_t unitsLength,
                                            SourceOwnership ownership) {
    MOZ_ASSERT_IF(units == nullptr, unitsLength == 0);

    // Ideally we'd use |Unit| and not cast below, but the risk of a static
    // initializer is too great.
    static const CharT emptyString[] = {'\0'};

    // Initialize all fields *before* checking length.  This ensures that
    // if |ownership == SourceOwnership::TakeOwnership|, |units| will be
    // freed when |this|'s destructor is called.
    if (units) {
      units_ = units;
      length_ = static_cast<uint32_t>(unitsLength);
      ownsUnits_ = ownership == SourceOwnership::TakeOwnership;
    } else {
      units_ = reinterpret_cast<const Unit*>(emptyString);
      length_ = 0;
      ownsUnits_ = false;
    }

    // IMPLEMENTATION DETAIL, DO NOT RELY ON: This limit is used so we can
    // store offsets in |JSScript|s as |uint32_t|.  It could be lifted
    // fairly easily if desired, as the compiler uses |size_t| internally.
    if (MOZ_UNLIKELY(unitsLength > UINT32_MAX)) {
      detail::ReportSourceTooLong(cx);
      return false;
    }

    return true;
  }

  /**
   * Exactly identical to the |init()| overload above that accepts
   * |const Unit*|, but instead takes character data: |const CharT*|.
   *
   * (We can't just write this to accept |const CharT*|, because then in the
   * UTF-16 case this overload and the one above would be identical.  So we
   * use SFINAE to expose the |CharT| overload only if it's different.)
   */
  template <typename Char,
            typename = std::enable_if_t<std::is_same_v<Char, CharT> &&
                                        !std::is_same_v<Char, Unit>>>
  [[nodiscard]] MOZ_IS_CLASS_INIT bool init(JSContext* cx, const Char* chars,
                                            size_t charsLength,
                                            SourceOwnership ownership) {
    return init(cx, reinterpret_cast<const Unit*>(chars), charsLength,
                ownership);
  }

  /**
   * Initialize this using source units transferred out of |data|.
   */
  [[nodiscard]] bool init(JSContext* cx,
                          js::UniquePtr<Unit[], JS::FreePolicy> data,
                          size_t dataLength) {
    return init(cx, data.release(), dataLength, SourceOwnership::TakeOwnership);
  }

  /**
   * Exactly identical to the |init()| overload above that accepts
   * |UniquePtr<Unit[], JS::FreePolicy>|, but instead takes character data:
   * |UniquePtr<CharT[], JS::FreePolicy>|.
   *
   * (We can't just duplicate the signature above with s/Unit/CharT/, because
   * then in the UTF-16 case this overload and the one above would be identical.
   * So we use SFINAE to expose the |CharT| overload only if it's different.)
   */
  template <typename Char,
            typename = std::enable_if_t<std::is_same_v<Char, CharT> &&
                                        !std::is_same_v<Char, Unit>>>
  [[nodiscard]] bool init(JSContext* cx,
                          js::UniquePtr<Char[], JS::FreePolicy> data,
                          size_t dataLength) {
    return init(cx, data.release(), dataLength, SourceOwnership::TakeOwnership);
  }

  /**
   * Access the encapsulated data using a code unit type.
   *
   * This function is useful for code that wants to interact with source text
   * as *code units*, not as string data.  This doesn't matter for UTF-16,
   * but it's a crucial distinction for UTF-8.  When UTF-8 source text is
   * encapsulated, |Unit| being |mozilla::Utf8Unit| unambiguously indicates
   * that the code units are UTF-8.  In contrast |const char*| returned by
   * |get()| below could hold UTF-8 (or its ASCII subset) or Latin-1 or (in
   * particularly cursed embeddings) EBCDIC or some other legacy character
   * set.  Prefer this function to |get()| wherever possible.
   */
  const Unit* units() const { return units_; }

  /**
   * Access the encapsulated data using a character type.
   *
   * This function is useful for interactions with character-centric actions
   * like interacting with UniqueChars/UniqueTwoByteChars or printing out
   * text in a debugger, that only work with |CharT|.  But as |CharT| loses
   * encoding specificity when UTF-8 source text is encapsulated, prefer
   * |units()| to this function.
   */
  const CharT* get() const { return reinterpret_cast<const CharT*>(units_); }

  /**
   * Returns true if this owns the source units and will free them on
   * destruction.  If true, it is legal to call |take{Chars,Units}()|.
   */
  bool ownsUnits() const { return ownsUnits_; }

  /**
   * Count of the underlying source units -- code units, not bytes or code
   * points -- in this.
   */
  uint32_t length() const { return length_; }

  /**
   * Retrieve and take ownership of the underlying source units.  The caller
   * is now responsible for calling js_free() on the returned value, *but
   * only after JS script compilation has completed*.
   *
   * After underlying source units have been taken, this will continue to
   * refer to the same data -- it just won't own the data.  get() and
   * length() will return the same values, but ownsUnits() will be false.
   * The taken source units must be kept alive until after JS script
   * compilation completes, as noted above, for this to be safe.
   *
   * The caller must check ownsUnits() before calling takeUnits().  Taking
   * and then free'ing an unowned buffer will have dire consequences.
   */
  Unit* takeUnits() {
    MOZ_ASSERT(ownsUnits_);
    ownsUnits_ = false;
    return const_cast<Unit*>(units_);
  }

  /**
   * Akin to |takeUnits()| in all respects, but returns characters rather
   * than units.
   */
  CharT* takeChars() { return reinterpret_cast<CharT*>(takeUnits()); }

 private:
  SourceText(const SourceText&) = delete;
  void operator=(const SourceText&) = delete;
};

}  // namespace JS

#endif /* js_SourceText_h */
