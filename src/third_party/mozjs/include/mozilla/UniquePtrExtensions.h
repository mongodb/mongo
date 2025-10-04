/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Useful extensions to UniquePtr. */

#ifndef mozilla_UniquePtrExtensions_h
#define mozilla_UniquePtrExtensions_h

#include <type_traits>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/fallible.h"
#include "mozilla/UniquePtr.h"

#ifdef XP_WIN
#  include <cstdint>
#endif
#if defined(XP_DARWIN) && !defined(RUST_BINDGEN)
#  include <mach/mach.h>
#endif

namespace mozilla {

/**
 * MakeUniqueFallible works exactly like MakeUnique, except that the memory
 * allocation performed is done fallibly, i.e. it can return nullptr.
 */
template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject MakeUniqueFallible(
    Args&&... aArgs) {
  return UniquePtr<T>(new (fallible) T(std::forward<Args>(aArgs)...));
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUniqueFallible(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new (fallible) ArrayType[aN]());
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUniqueFallible(
    Args&&... aArgs) = delete;

/**
 * MakeUniqueForOverwrite and MakeUniqueFallibleForOverwrite are like MakeUnique
 * and MakeUniqueFallible except they use default-initialization. This is
 * useful, for example, when you have a POD type array that will be overwritten
 * directly after construction and so zero-initialization is a waste.
 */
template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject MakeUniqueForOverwrite() {
  return UniquePtr<T>(new T);
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUniqueForOverwrite(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new ArrayType[aN]);
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUniqueForOverwrite(
    Args&&... aArgs) = delete;

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::SingleObject
MakeUniqueForOverwriteFallible() {
  return UniquePtr<T>(new (fallible) T);
}

template <typename T>
typename detail::UniqueSelector<T>::UnknownBound MakeUniqueForOverwriteFallible(
    decltype(sizeof(int)) aN) {
  using ArrayType = std::remove_extent_t<T>;
  return UniquePtr<T>(new (fallible) ArrayType[aN]);
}

template <typename T, typename... Args>
typename detail::UniqueSelector<T>::KnownBound MakeUniqueForOverwriteFallible(
    Args&&... aArgs) = delete;

namespace detail {

template <typename T>
struct FreePolicy {
  void operator()(const void* ptr) { free(const_cast<void*>(ptr)); }
};

#if defined(XP_WIN)
// Can't include <windows.h> to get the actual definition of HANDLE
// because of namespace pollution.
typedef void* FileHandleType;
#elif defined(XP_UNIX)
typedef int FileHandleType;
#else
#  error "Unsupported OS?"
#endif

struct FileHandleHelper {
  MOZ_IMPLICIT FileHandleHelper(FileHandleType aHandle) : mHandle(aHandle) {
#if defined(XP_UNIX) && (defined(DEBUG) || defined(FUZZING))
    MOZ_RELEASE_ASSERT(aHandle == kInvalidHandle || aHandle > 2);
#endif
  }

  MOZ_IMPLICIT constexpr FileHandleHelper(std::nullptr_t)
      : mHandle(kInvalidHandle) {}

  bool operator!=(std::nullptr_t) const {
#ifdef XP_WIN
    // Windows uses both nullptr and INVALID_HANDLE_VALUE (-1 cast to
    // HANDLE) in different situations, but nullptr is more reliably
    // null while -1 is also valid input to some calls that take
    // handles.  So class considers both to be null (since neither
    // should be closed) but default-constructs as nullptr.
    if (mHandle == (void*)-1) {
      return false;
    }
#endif
    return mHandle != kInvalidHandle;
  }

  operator FileHandleType() const { return mHandle; }

#ifdef XP_WIN
  // NSPR uses an integer type for PROsfd, so this conversion is
  // provided for working with it without needing reinterpret casts
  // everywhere.
  operator std::intptr_t() const {
    return reinterpret_cast<std::intptr_t>(mHandle);
  }
#endif

  // When there's only one user-defined conversion operator, the
  // compiler will use that to derive equality, but that doesn't work
  // when the conversion is ambiguoug (the XP_WIN case above).
  bool operator==(const FileHandleHelper& aOther) const {
    return mHandle == aOther.mHandle;
  }

 private:
  FileHandleType mHandle;

#ifdef XP_WIN
  // See above for why this is nullptr.  (Also, INVALID_HANDLE_VALUE
  // can't be expressed as a constexpr.)
  static constexpr FileHandleType kInvalidHandle = nullptr;
#else
  static constexpr FileHandleType kInvalidHandle = -1;
#endif
};

struct FileHandleDeleter {
  using pointer = FileHandleHelper;
  using receiver = FileHandleType;
  MFBT_API void operator()(FileHandleHelper aHelper);
};

#if defined(XP_DARWIN) && !defined(RUST_BINDGEN)
struct MachPortHelper {
  MOZ_IMPLICIT MachPortHelper(mach_port_t aPort) : mPort(aPort) {}

  MOZ_IMPLICIT constexpr MachPortHelper(std::nullptr_t)
      : mPort(MACH_PORT_NULL) {}

  bool operator!=(std::nullptr_t) const { return mPort != MACH_PORT_NULL; }

  operator const mach_port_t&() const { return mPort; }
  operator mach_port_t&() { return mPort; }

 private:
  mach_port_t mPort;
};

struct MachSendRightDeleter {
  using pointer = MachPortHelper;
  using receiver = mach_port_t;
  MFBT_API void operator()(MachPortHelper aHelper) {
    DebugOnly<kern_return_t> kr =
        mach_port_deallocate(mach_task_self(), aHelper);
    MOZ_ASSERT(kr == KERN_SUCCESS, "failed to deallocate mach send right");
  }
};

struct MachReceiveRightDeleter {
  using pointer = MachPortHelper;
  using receiver = mach_port_t;
  MFBT_API void operator()(MachPortHelper aHelper) {
    DebugOnly<kern_return_t> kr = mach_port_mod_refs(
        mach_task_self(), aHelper, MACH_PORT_RIGHT_RECEIVE, -1);
    MOZ_ASSERT(kr == KERN_SUCCESS, "failed to release mach receive right");
  }
};

struct MachPortSetDeleter {
  using pointer = MachPortHelper;
  using receiver = mach_port_t;
  MFBT_API void operator()(MachPortHelper aHelper) {
    DebugOnly<kern_return_t> kr = mach_port_mod_refs(
        mach_task_self(), aHelper, MACH_PORT_RIGHT_PORT_SET, -1);
    MOZ_ASSERT(kr == KERN_SUCCESS, "failed to release mach port set");
  }
};
#endif

}  // namespace detail

template <typename T>
using UniqueFreePtr = UniquePtr<T, detail::FreePolicy<T>>;

// A RAII class for the OS construct used for open files and similar
// objects: a file descriptor on Unix or a handle on Windows.
using UniqueFileHandle =
    UniquePtr<detail::FileHandleType, detail::FileHandleDeleter>;

#if defined(XP_DARWIN) && !defined(RUST_BINDGEN)
// A RAII class for a Mach port that names a send right.
using UniqueMachSendRight =
    UniquePtr<mach_port_t, detail::MachSendRightDeleter>;
// A RAII class for a Mach port that names a receive right.
using UniqueMachReceiveRight =
    UniquePtr<mach_port_t, detail::MachReceiveRightDeleter>;
// A RAII class for a Mach port set.
using UniqueMachPortSet = UniquePtr<mach_port_t, detail::MachPortSetDeleter>;

// Increases the user reference count for MACH_PORT_RIGHT_SEND by 1 and returns
// a new UniqueMachSendRight to manage the additional right.
inline UniqueMachSendRight RetainMachSendRight(mach_port_t aPort) {
  kern_return_t kr =
      mach_port_mod_refs(mach_task_self(), aPort, MACH_PORT_RIGHT_SEND, 1);
  if (kr == KERN_SUCCESS) {
    return UniqueMachSendRight(aPort);
  }
  return nullptr;
}
#endif

namespace detail {

struct HasReceiverTypeHelper {
  template <class U>
  static double Test(...);
  template <class U>
  static char Test(typename U::receiver* = 0);
};

template <class T>
class HasReceiverType
    : public std::integral_constant<bool, sizeof(HasReceiverTypeHelper::Test<T>(
                                              0)) == 1> {};

template <class T, class D, bool = HasReceiverType<D>::value>
struct ReceiverTypeImpl {
  using Type = typename D::receiver;
};

template <class T, class D>
struct ReceiverTypeImpl<T, D, false> {
  using Type = typename PointerType<T, D>::Type;
};

template <class T, class D>
struct ReceiverType {
  using Type = typename ReceiverTypeImpl<T, std::remove_reference_t<D>>::Type;
};

template <typename T, typename D>
class MOZ_TEMPORARY_CLASS UniquePtrGetterTransfers {
 public:
  using Ptr = UniquePtr<T, D>;
  using Receiver = typename detail::ReceiverType<T, D>::Type;

  explicit UniquePtrGetterTransfers(Ptr& p)
      : mPtr(p), mReceiver(typename Ptr::Pointer(nullptr)) {}
  ~UniquePtrGetterTransfers() { mPtr.reset(mReceiver); }

  operator Receiver*() { return &mReceiver; }
  Receiver& operator*() { return mReceiver; }

  // operator void** is conditionally enabled if `Receiver` is a pointer.
  template <typename U = Receiver,
            std::enable_if_t<
                std::is_pointer_v<U> && std::is_same_v<U, Receiver>, int> = 0>
  operator void**() {
    return reinterpret_cast<void**>(&mReceiver);
  }

 private:
  Ptr& mPtr;
  Receiver mReceiver;
};

}  // namespace detail

// Helper for passing a UniquePtr to an old-style function that uses raw
// pointers for out params. Example usage:
//
//   void AllocateFoo(Foo** out) { *out = new Foo(); }
//   UniquePtr<Foo> foo;
//   AllocateFoo(getter_Transfers(foo));
template <typename T, typename D>
auto getter_Transfers(UniquePtr<T, D>& up) {
  return detail::UniquePtrGetterTransfers<T, D>(up);
}

}  // namespace mozilla

#endif  // mozilla_UniquePtrExtensions_h
