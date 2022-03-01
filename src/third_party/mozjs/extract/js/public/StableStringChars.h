/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Safely access the contents of a string even as GC can cause the string's
 * contents to move around in memory.
 */

#ifndef js_StableStringChars_h
#define js_StableStringChars_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_INIT_OUTSIDE_CTOR, MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe
#include "mozilla/Range.h"       // mozilla::Range

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/AllocPolicy.h"
#include "js/RootingAPI.h"  // JS::Handle, JS::Rooted
#include "js/String.h"      // JS::GetStringLength
#include "js/TypeDecls.h"   // JSContext, JS::Latin1Char, JSString
#include "js/Vector.h"      // js::Vector

class JSLinearString;

namespace JS {

/**
 * This class provides safe access to a string's chars across a GC. If we ever
 * nursery allocate strings' out of line chars, this class will have to make a
 * copy, so it's best to avoid using this class unless you really need it. It's
 * usually more efficient to use the latin1Chars/twoByteChars JSString methods
 * and often the code can be rewritten so that only indexes instead of char
 * pointers are used in parts of the code that can GC.
 */
class MOZ_STACK_CLASS JS_PUBLIC_API AutoStableStringChars final {
  /*
   * When copying string char, use this many bytes of inline storage.  This is
   * chosen to allow the inline string types to be copied without allocating.
   * This is asserted in AutoStableStringChars::allocOwnChars.
   */
  static const size_t InlineCapacity = 24;

  /* Ensure the string is kept alive while we're using its chars. */
  Rooted<JSString*> s_;
  union MOZ_INIT_OUTSIDE_CTOR {
    const char16_t* twoByteChars_;
    const Latin1Char* latin1Chars_;
  };
  mozilla::Maybe<js::Vector<uint8_t, InlineCapacity>> ownChars_;
  enum State { Uninitialized, Latin1, TwoByte };
  State state_;

 public:
  explicit AutoStableStringChars(JSContext* cx)
      : s_(cx), state_(Uninitialized) {}

  [[nodiscard]] bool init(JSContext* cx, JSString* s);

  /* Like init(), but Latin1 chars are inflated to TwoByte. */
  [[nodiscard]] bool initTwoByte(JSContext* cx, JSString* s);

  bool isLatin1() const { return state_ == Latin1; }
  bool isTwoByte() const { return state_ == TwoByte; }

  const Latin1Char* latin1Chars() const {
    MOZ_ASSERT(state_ == Latin1);
    return latin1Chars_;
  }
  const char16_t* twoByteChars() const {
    MOZ_ASSERT(state_ == TwoByte);
    return twoByteChars_;
  }

  mozilla::Range<const Latin1Char> latin1Range() const {
    MOZ_ASSERT(state_ == Latin1);
    return mozilla::Range<const Latin1Char>(latin1Chars_, GetStringLength(s_));
  }

  mozilla::Range<const char16_t> twoByteRange() const {
    MOZ_ASSERT(state_ == TwoByte);
    return mozilla::Range<const char16_t>(twoByteChars_, GetStringLength(s_));
  }

  /* If we own the chars, transfer ownership to the caller. */
  bool maybeGiveOwnershipToCaller() {
    MOZ_ASSERT(state_ != Uninitialized);
    if (!ownChars_.isSome() || !ownChars_->extractRawBuffer()) {
      return false;
    }
    state_ = Uninitialized;
    ownChars_.reset();
    return true;
  }

 private:
  AutoStableStringChars(const AutoStableStringChars& other) = delete;
  void operator=(const AutoStableStringChars& other) = delete;

  bool baseIsInline(Handle<JSLinearString*> linearString);
  template <typename T>
  T* allocOwnChars(JSContext* cx, size_t count);
  bool copyLatin1Chars(JSContext* cx, Handle<JSLinearString*> linearString);
  bool copyTwoByteChars(JSContext* cx, Handle<JSLinearString*> linearString);
  bool copyAndInflateLatin1Chars(JSContext*,
                                 Handle<JSLinearString*> linearString);
};

}  // namespace JS

#endif /* js_StableStringChars_h */
