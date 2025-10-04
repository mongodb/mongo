/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_DynamicallyLinkedFunctionPtr_h
#define mozilla_DynamicallyLinkedFunctionPtr_h

#include <windows.h>

#include <utility>

#include "mozilla/Attributes.h"

namespace mozilla {
namespace detail {

template <typename T>
struct FunctionPtrCracker;

template <typename R, typename... Args>
struct FunctionPtrCracker<R (*)(Args...)> {
  using ReturnT = R;
  using FunctionPtrT = R (*)(Args...);
};

#if defined(_M_IX86)
template <typename R, typename... Args>
struct FunctionPtrCracker<R(__stdcall*)(Args...)> {
  using ReturnT = R;
  using FunctionPtrT = R(__stdcall*)(Args...);
};

template <typename R, typename... Args>
struct FunctionPtrCracker<R(__fastcall*)(Args...)> {
  using ReturnT = R;
  using FunctionPtrT = R(__fastcall*)(Args...);
};
#endif  // defined(_M_IX86)

template <typename T>
class DynamicallyLinkedFunctionPtrBase {
 public:
  using ReturnT = typename FunctionPtrCracker<T>::ReturnT;
  using FunctionPtrT = typename FunctionPtrCracker<T>::FunctionPtrT;

  DynamicallyLinkedFunctionPtrBase(const wchar_t* aLibName,
                                   const char* aFuncName)
      : mModule(::LoadLibraryW(aLibName)), mFunction(nullptr) {
    if (!mModule) {
      return;
    }

    mFunction =
        reinterpret_cast<FunctionPtrT>(::GetProcAddress(mModule, aFuncName));

    if (!mFunction) {
      // Since the function doesn't exist, there is no point in holding a
      // reference to mModule anymore.
      ::FreeLibrary(mModule);
      mModule = nullptr;
    }
  }

  DynamicallyLinkedFunctionPtrBase(const DynamicallyLinkedFunctionPtrBase&) =
      delete;
  DynamicallyLinkedFunctionPtrBase& operator=(
      const DynamicallyLinkedFunctionPtrBase&) = delete;

  DynamicallyLinkedFunctionPtrBase(DynamicallyLinkedFunctionPtrBase&&) = delete;
  DynamicallyLinkedFunctionPtrBase& operator=(
      DynamicallyLinkedFunctionPtrBase&&) = delete;

  template <typename... Args>
  ReturnT operator()(Args&&... args) const {
    return mFunction(std::forward<Args>(args)...);
  }

  explicit operator bool() const { return !!mFunction; }

 protected:
  HMODULE mModule;
  FunctionPtrT mFunction;
};

}  // namespace detail

/**
 * In most cases, this class is the one that you want to use for resolving a
 * dynamically-linked function pointer. It should be instantiated as a static
 * local variable.
 *
 * NB: It has a trivial destructor, so the DLL that is loaded is never freed.
 * Assuming that this function is called fairly often, this is the most
 * sensible option. OTOH, if the function you are calling is a one-off, or the
 * static local requirement is too restrictive, use DynamicallyLinkedFunctionPtr
 * instead.
 */
template <typename T>
class MOZ_STATIC_LOCAL_CLASS StaticDynamicallyLinkedFunctionPtr final
    : public detail::DynamicallyLinkedFunctionPtrBase<T> {
 public:
  StaticDynamicallyLinkedFunctionPtr(const wchar_t* aLibName,
                                     const char* aFuncName)
      : detail::DynamicallyLinkedFunctionPtrBase<T>(aLibName, aFuncName) {}

  /**
   * We only offer this operator for the static local case, as it is not
   * possible for this object to be destroyed while the returned pointer is
   * being held.
   */
  operator typename detail::DynamicallyLinkedFunctionPtrBase<T>::FunctionPtrT()
      const {
    return this->mFunction;
  }
};

template <typename T>
class MOZ_NON_PARAM MOZ_NON_TEMPORARY_CLASS DynamicallyLinkedFunctionPtr final
    : public detail::DynamicallyLinkedFunctionPtrBase<T> {
 public:
  DynamicallyLinkedFunctionPtr(const wchar_t* aLibName, const char* aFuncName)
      : detail::DynamicallyLinkedFunctionPtrBase<T>(aLibName, aFuncName) {}

  ~DynamicallyLinkedFunctionPtr() {
    if (!this->mModule) {
      return;
    }

    ::FreeLibrary(this->mModule);
  }
};

}  // namespace mozilla

#endif  // mozilla_DynamicallyLinkedFunctionPtr_h
