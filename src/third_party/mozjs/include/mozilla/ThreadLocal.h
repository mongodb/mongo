/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Cross-platform lightweight thread local data wrappers. */

#ifndef mozilla_ThreadLocal_h
#define mozilla_ThreadLocal_h

#if !defined(XP_WIN) && !defined(__wasi__)
#  include <pthread.h>
#endif

#include <type_traits>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

namespace mozilla {

namespace detail {

#ifdef XP_MACOSX
#  if defined(__has_feature)
#    if __has_feature(cxx_thread_local)
#      define MACOSX_HAS_THREAD_LOCAL
#    endif
#  endif
#endif

/*
 * Thread Local Storage helpers.
 *
 * Usage:
 *
 * Do not directly instantiate this class.  Instead, use the
 * MOZ_THREAD_LOCAL macro to declare or define instances.  The macro
 * takes a type name as its argument.
 *
 * Declare like this:
 * extern MOZ_THREAD_LOCAL(int) tlsInt;
 * Define like this:
 * MOZ_THREAD_LOCAL(int) tlsInt;
 * or:
 * static MOZ_THREAD_LOCAL(int) tlsInt;
 *
 * Only static-storage-duration (e.g. global variables, or static class members)
 * objects of this class should be instantiated. This class relies on
 * zero-initialization, which is implicit for static-storage-duration objects.
 * It doesn't have a custom default constructor, to avoid static initializers.
 *
 * API usage:
 *
 * // Create a TLS item.
 * //
 * // Note that init() should be invoked before the first use of set()
 * // or get().  It is ok to call it multiple times.  This must be
 * // called in a way that avoids possible races with other threads.
 * MOZ_THREAD_LOCAL(int) tlsKey;
 * if (!tlsKey.init()) {
 *   // deal with the error
 * }
 *
 * // Set the TLS value
 * tlsKey.set(123);
 *
 * // Get the TLS value
 * int value = tlsKey.get();
 */

// Integral types narrower than void* must be extended to avoid
// warnings from valgrind on some platforms.  This helper type
// achieves that without penalizing the common case of ThreadLocals
// instantiated using a pointer type.
template <typename S>
struct Helper {
  typedef uintptr_t Type;
};

template <typename S>
struct Helper<S*> {
  typedef S* Type;
};

#if defined(XP_WIN)
/*
 * ThreadLocalKeyStorage uses Thread Local APIs that are declared in
 * processthreadsapi.h. To use this class on Windows, include that file
 * (or windows.h) before including ThreadLocal.h.
 *
 * TLS_OUT_OF_INDEXES is a #define that is used to detect whether
 * an appropriate header has been included prior to this file
 */
#  if defined(TLS_OUT_OF_INDEXES)
/* Despite not being used for MOZ_THREAD_LOCAL, we expose an implementation for
 * Windows for cases where it's not desirable to use thread_local */
template <typename T>
class ThreadLocalKeyStorage {
 public:
  ThreadLocalKeyStorage() : mKey(TLS_OUT_OF_INDEXES) {}

  inline bool initialized() const { return mKey != TLS_OUT_OF_INDEXES; }

  inline void init() { mKey = TlsAlloc(); }

  inline T get() const {
    void* h = TlsGetValue(mKey);
    return static_cast<T>(reinterpret_cast<typename Helper<T>::Type>(h));
  }

  inline bool set(const T aValue) {
    void* h = const_cast<void*>(reinterpret_cast<const void*>(
        static_cast<typename Helper<T>::Type>(aValue)));
    return TlsSetValue(mKey, h);
  }

 private:
  unsigned long mKey;
};
#  endif
#elif defined(__wasi__)
// There are no threads on WASI, so we just use a global variable.
template <typename T>
class ThreadLocalKeyStorage {
 public:
  constexpr ThreadLocalKeyStorage() : mInited(false) {}

  inline bool initialized() const { return mInited; }

  inline void init() { mInited = true; }

  inline T get() const { return mVal; }

  inline bool set(const T aValue) {
    mVal = aValue;
    return true;
  }

 private:
  bool mInited;
  T mVal;
};
#else
template <typename T>
class ThreadLocalKeyStorage {
 public:
  constexpr ThreadLocalKeyStorage() : mKey(0), mInited(false) {}

  inline bool initialized() const { return mInited; }

  inline void init() { mInited = !pthread_key_create(&mKey, nullptr); }

  inline T get() const {
    void* h = pthread_getspecific(mKey);
    return static_cast<T>(reinterpret_cast<typename Helper<T>::Type>(h));
  }

  inline bool set(const T aValue) {
    const void* h = reinterpret_cast<const void*>(
        static_cast<typename Helper<T>::Type>(aValue));
    return !pthread_setspecific(mKey, h);
  }

 private:
  pthread_key_t mKey;
  bool mInited;
};
#endif

template <typename T>
class ThreadLocalNativeStorage {
 public:
  // __thread does not allow non-trivial constructors, but we can
  // instead rely on zero-initialization.
  inline bool initialized() const { return true; }

  inline void init() {}

  inline T get() const { return mValue; }

  inline bool set(const T aValue) {
    mValue = aValue;
    return true;
  }

 private:
  T mValue;
};

template <typename T, template <typename U> class Storage>
class ThreadLocal : public Storage<T> {
 public:
  [[nodiscard]] inline bool init();

  void infallibleInit() {
    MOZ_RELEASE_ASSERT(init(), "Infallible TLS initialization failed");
  }

  inline T get() const;

  inline void set(const T aValue);

  using Type = T;
};

template <typename T, template <typename U> class Storage>
inline bool ThreadLocal<T, Storage>::init() {
  static_assert(std::is_pointer_v<T> || std::is_integral_v<T>,
                "mozilla::ThreadLocal must be used with a pointer or "
                "integral type");
  static_assert(sizeof(T) <= sizeof(void*),
                "mozilla::ThreadLocal can't be used for types larger than "
                "a pointer");

  if (!Storage<T>::initialized()) {
    Storage<T>::init();
  }
  return Storage<T>::initialized();
}

template <typename T, template <typename U> class Storage>
inline T ThreadLocal<T, Storage>::get() const {
  MOZ_ASSERT(Storage<T>::initialized());
  return Storage<T>::get();
}

template <typename T, template <typename U> class Storage>
inline void ThreadLocal<T, Storage>::set(const T aValue) {
  MOZ_ASSERT(Storage<T>::initialized());
  bool succeeded = Storage<T>::set(aValue);
  if (!succeeded) {
    MOZ_CRASH();
  }
}

#if (defined(XP_WIN) || defined(MACOSX_HAS_THREAD_LOCAL)) && \
    !defined(__MINGW32__)
#  define MOZ_THREAD_LOCAL(TYPE)                 \
    thread_local ::mozilla::detail::ThreadLocal< \
        TYPE, ::mozilla::detail::ThreadLocalNativeStorage>
#elif defined(HAVE_THREAD_TLS_KEYWORD)
#  define MOZ_THREAD_LOCAL(TYPE)             \
    __thread ::mozilla::detail::ThreadLocal< \
        TYPE, ::mozilla::detail::ThreadLocalNativeStorage>
#else
#  define MOZ_THREAD_LOCAL(TYPE)         \
    ::mozilla::detail::ThreadLocal<TYPE, \
                                   ::mozilla::detail::ThreadLocalKeyStorage>
#endif

}  // namespace detail
}  // namespace mozilla

#endif /* mozilla_ThreadLocal_h */
