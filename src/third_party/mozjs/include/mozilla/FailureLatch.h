/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// This header contains an interface `FailureLatch`, and some implementation
// helpers that may be used across a range of classes and functions to handle
// failures at any point during a process, and share that failure state so that
// the process may gracefully stop quickly and report the first error.
//
// It could be thought as a replacement for C++ exceptions, but it's less strong
// (cancellations may be delayed).
// Now, if possible, mozilla::Result may be a better option as C++ exceptions
// replacement, as it is more visible in all affected functions.
// Consider FailureLatch if failures may happen in different places, but where
// `return`ing this potential failure from all functions would be too arduous.

#ifndef mozilla_FailureLatch_h
#define mozilla_FailureLatch_h

#include <mozilla/Assertions.h>

#include <string>

namespace mozilla {

// ----------------------------------------------------------------------------
// Main interface
// ----------------------------------------------------------------------------

// Interface handling a failure latch (starting in a successful state, the first
// failure gets recorded, subsequent failures are ignored.)
class FailureLatch {
 public:
  virtual ~FailureLatch() = default;

  // Can this ever fail? (This may influence how some code deals with
  // failures, e.g., if infallible, OOMs should assert&crash.)
  [[nodiscard]] virtual bool Fallible() const = 0;

  // Set latch in its failed state because of an external cause.
  // The first call sets the reason, subsequent calls are ignored.
  virtual void SetFailure(std::string aReason) = 0;

  // Has there been any failure so far?
  [[nodiscard]] virtual bool Failed() const = 0;

  // Return first failure string, may be null if not failed yet.
  [[nodiscard]] virtual const char* GetFailure() const = 0;

  // Retrieve the one source FailureLatch. It could reference `*this`!
  // This may be used by dependent proxy FailureLatch'es to find where to
  // redirect calls.
  [[nodiscard]] virtual const FailureLatch& SourceFailureLatch() const = 0;
  [[nodiscard]] virtual FailureLatch& SourceFailureLatch() = 0;

  // Non-virtual helpers.

  // Transfer any failure from another FailureLatch.
  void SetFailureFrom(const FailureLatch& aOther) {
    if (Failed()) {
      return;
    }
    if (const char* otherFailure = aOther.GetFailure(); otherFailure) {
      SetFailure(otherFailure);
    }
  }
};

// ----------------------------------------------------------------------------
// Concrete implementations
// ----------------------------------------------------------------------------

// Concrete infallible FailureLatch class.
// Any `SetFailure` leads to an assert-crash, so the final runtime result can
// always be assumed to be succesful.
class FailureLatchInfallibleSource final : public FailureLatch {
 public:
  [[nodiscard]] bool Fallible() const final { return false; }

  void SetFailure(std::string aReason) final {
    MOZ_RELEASE_ASSERT(false,
                       "SetFailure in infallible FailureLatchInfallibleSource");
  }

  [[nodiscard]] bool Failed() const final { return false; }

  [[nodiscard]] const char* GetFailure() const final { return nullptr; }

  [[nodiscard]] const ::mozilla::FailureLatch& SourceFailureLatch()
      const final {
    return *this;
  }

  [[nodiscard]] ::mozilla::FailureLatch& SourceFailureLatch() final {
    return *this;
  }

  // Singleton FailureLatchInfallibleSource that may be used as default
  // FailureLatch proxy.
  static FailureLatchInfallibleSource& Singleton() {
    static FailureLatchInfallibleSource singleton;
    return singleton;
  }
};

// Concrete FailureLatch class, intended to be intantiated as an object shared
// between classes and functions that are part of a long operation, so that
// failures can happen anywhere and be visible everywhere.
// Not thread-safe.
class FailureLatchSource final : public FailureLatch {
 public:
  [[nodiscard]] bool Fallible() const final { return true; }

  void SetFailure(std::string aReason) final {
    if (!mFailed) {
      mFailed = true;
      mReason = std::move(aReason);
    }
  }

  [[nodiscard]] bool Failed() const final { return mFailed; }

  [[nodiscard]] const char* GetFailure() const final {
    return mFailed ? mReason.c_str() : nullptr;
  }

  [[nodiscard]] const FailureLatch& SourceFailureLatch() const final {
    return *this;
  }

  [[nodiscard]] FailureLatch& SourceFailureLatch() final { return *this; }

 private:
  bool mFailed = false;
  std::string mReason;
};

// ----------------------------------------------------------------------------
// Helper macros, to be used in FailureLatch-derived classes
// ----------------------------------------------------------------------------

// Classes deriving from FailureLatch can use this to forward virtual calls to
// another FailureLatch.
#define FAILURELATCH_IMPL_PROXY(FAILURELATCH_REF)                        \
  [[nodiscard]] bool Fallible() const final {                            \
    return static_cast<const ::mozilla::FailureLatch&>(FAILURELATCH_REF) \
        .Fallible();                                                     \
  }                                                                      \
  void SetFailure(std::string aReason) final {                           \
    static_cast<::mozilla::FailureLatch&>(FAILURELATCH_REF)              \
        .SetFailure(std::move(aReason));                                 \
  }                                                                      \
  [[nodiscard]] bool Failed() const final {                              \
    return static_cast<const ::mozilla::FailureLatch&>(FAILURELATCH_REF) \
        .Failed();                                                       \
  }                                                                      \
  [[nodiscard]] const char* GetFailure() const final {                   \
    return static_cast<const ::mozilla::FailureLatch&>(FAILURELATCH_REF) \
        .GetFailure();                                                   \
  }                                                                      \
  [[nodiscard]] const FailureLatch& SourceFailureLatch() const final {   \
    return static_cast<const ::mozilla::FailureLatch&>(FAILURELATCH_REF) \
        .SourceFailureLatch();                                           \
  }                                                                      \
  [[nodiscard]] FailureLatch& SourceFailureLatch() final {               \
    return static_cast<::mozilla::FailureLatch&>(FAILURELATCH_REF)       \
        .SourceFailureLatch();                                           \
  }

// Classes deriving from FailureLatch can use this to forward virtual calls to
// another FailureLatch through a pointer, unless it's null in which case act
// like an infallible FailureLatch.
#define FAILURELATCH_IMPL_PROXY_OR_INFALLIBLE(FAILURELATCH_PTR, CLASS_NAME)    \
  [[nodiscard]] bool Fallible() const final {                                  \
    return FAILURELATCH_PTR                                                    \
               ? static_cast<const ::mozilla::FailureLatch*>(FAILURELATCH_PTR) \
                     ->Fallible()                                              \
               : false;                                                        \
  }                                                                            \
  void SetFailure(std::string aReason) final {                                 \
    if (FAILURELATCH_PTR) {                                                    \
      static_cast<::mozilla::FailureLatch*>(FAILURELATCH_PTR)                  \
          ->SetFailure(std::move(aReason));                                    \
    } else {                                                                   \
      MOZ_RELEASE_ASSERT(false, "SetFailure in infallible " #CLASS_NAME);      \
    }                                                                          \
  }                                                                            \
  [[nodiscard]] bool Failed() const final {                                    \
    return FAILURELATCH_PTR                                                    \
               ? static_cast<const ::mozilla::FailureLatch*>(FAILURELATCH_PTR) \
                     ->Failed()                                                \
               : false;                                                        \
  }                                                                            \
  [[nodiscard]] const char* GetFailure() const final {                         \
    return FAILURELATCH_PTR                                                    \
               ? static_cast<const ::mozilla::FailureLatch*>(FAILURELATCH_PTR) \
                     ->GetFailure()                                            \
               : nullptr;                                                      \
  }                                                                            \
  [[nodiscard]] const FailureLatch& SourceFailureLatch() const final {         \
    return FAILURELATCH_PTR                                                    \
               ? static_cast<const ::mozilla::FailureLatch*>(FAILURELATCH_PTR) \
                     ->SourceFailureLatch()                                    \
               : ::mozilla::FailureLatchInfallibleSource::Singleton();         \
  }                                                                            \
  [[nodiscard]] FailureLatch& SourceFailureLatch() final {                     \
    return FAILURELATCH_PTR                                                    \
               ? static_cast<::mozilla::FailureLatch*>(FAILURELATCH_PTR)       \
                     ->SourceFailureLatch()                                    \
               : ::mozilla::FailureLatchInfallibleSource::Singleton();         \
  }

}  // namespace mozilla

#endif /* mozilla_FailureLatch_h */
