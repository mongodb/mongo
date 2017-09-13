/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* DEPRECATED: Use UniquePtr.h instead. */

#ifndef mozilla_Scoped_h
#define mozilla_Scoped_h

/*
 * DEPRECATED: Use UniquePtr.h instead.
 *
 * Resource Acquisition Is Initialization is a programming idiom used
 * to write robust code that is able to deallocate resources properly,
 * even in presence of execution errors or exceptions that need to be
 * propagated.  The Scoped* classes defined in this header perform the
 * deallocation of the resource they hold once program execution
 * reaches the end of the scope for which they have been defined.
 *
 * This header provides the following RAII classes:
 *
 * - |ScopedFreePtr| - a container for a pointer, that automatically calls
 *   |free()| at the end of the scope;
 * - |ScopedDeletePtr| - a container for a pointer, that automatically calls
 *   |delete| at the end of the scope;
 *
 * |ScopedDeleteArray| is removed in favor of |UniquePtr<T[]>|.
 *
 * The general scenario for each of the RAII classes is the following:
 *
 * ScopedClass foo(create_value());
 * // ... In this scope, |foo| is defined. Use |foo.get()| or |foo.rwget()|
 *        to access the value.
 * // ... In case of |return| or |throw|, |foo| is deallocated automatically.
 * // ... If |foo| needs to be returned or stored, use |foo.forget()|
 *
 * Note that the RAII classes defined in this header do _not_ perform any form
 * of reference-counting or garbage-collection. These classes have exactly two
 * behaviors:
 *
 * - if |forget()| has not been called, the resource is always deallocated at
 *   the end of the scope;
 * - if |forget()| has been called, any control on the resource is unbound
 *   and the resource is not deallocated by the class.
 *
 * Extension:
 *
 * In addition, this header provides class |Scoped| and macros |SCOPED_TEMPLATE|
 * and |MOZ_TYPE_SPECIFIC_SCOPED_POINTER_TEMPLATE|  to simplify the definition
 * of RAII classes for other scenarios. These macros have been used to
 * automatically close file descriptors/file handles when reaching the end of
 * the scope, graphics contexts, etc.
 */

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/GuardObjects.h"
#include "mozilla/Move.h"

namespace mozilla {

/*
 * Scoped is a helper to create RAII wrappers
 * Type argument |Traits| is expected to have the following structure:
 *
 *   struct Traits
 *   {
 *     // Define the type of the value stored in the wrapper
 *     typedef value_type type;
 *     // Returns the value corresponding to the uninitialized or freed state
 *     const static type empty();
 *     // Release resources corresponding to the wrapped value
 *     // This function is responsible for not releasing an |empty| value
 *     const static void release(type);
 *   }
 */
template<typename Traits>
class MOZ_NON_TEMPORARY_CLASS Scoped
{
public:
  typedef typename Traits::type Resource;

  explicit Scoped(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)
    : mValue(Traits::empty())
  {
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
  }

  explicit Scoped(const Resource& aValue
                  MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
    : mValue(aValue)
  {
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
  }

  /* Move constructor. */
  Scoped(Scoped&& aOther
         MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
    : mValue(Move(aOther.mValue))
  {
    MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    aOther.mValue = Traits::empty();
  }

  ~Scoped() { Traits::release(mValue); }

  // Constant getter
  operator const Resource&() const { return mValue; }
  const Resource& operator->() const { return mValue; }
  const Resource& get() const { return mValue; }
  // Non-constant getter.
  Resource& rwget() { return mValue; }

  /*
   * Forget the resource.
   *
   * Once |forget| has been called, the |Scoped| is neutralized, i.e. it will
   * have no effect at destruction (unless it is reset to another resource by
   * |operator=|).
   *
   * @return The original resource.
   */
  Resource forget()
  {
    Resource tmp = mValue;
    mValue = Traits::empty();
    return tmp;
  }

  /*
   * Perform immediate clean-up of this |Scoped|.
   *
   * If this |Scoped| is currently empty, this method has no effect.
   */
  void dispose()
  {
    Traits::release(mValue);
    mValue = Traits::empty();
  }

  bool operator==(const Resource& aOther) const { return mValue == aOther; }

  /*
   * Replace the resource with another resource.
   *
   * Calling |operator=| has the side-effect of triggering clean-up. If you do
   * not want to trigger clean-up, you should first invoke |forget|.
   *
   * @return this
   */
  Scoped& operator=(const Resource& aOther) { return reset(aOther); }

  Scoped& reset(const Resource& aOther)
  {
    Traits::release(mValue);
    mValue = aOther;
    return *this;
  }

  /* Move assignment operator. */
  Scoped& operator=(Scoped&& aRhs)
  {
    MOZ_ASSERT(&aRhs != this, "self-move-assignment not allowed");
    this->~Scoped();
    new(this) Scoped(Move(aRhs));
    return *this;
  }

private:
  explicit Scoped(const Scoped& aValue) = delete;
  Scoped& operator=(const Scoped& aValue) = delete;

private:
  Resource mValue;
  MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * SCOPED_TEMPLATE defines a templated class derived from Scoped
 * This allows to implement templates such as ScopedFreePtr.
 *
 * @param name The name of the class to define.
 * @param Traits A struct implementing clean-up. See the implementations
 * for more details.
 */
#define SCOPED_TEMPLATE(name, Traits)                                         \
template<typename Type>                                                       \
struct MOZ_NON_TEMPORARY_CLASS name : public mozilla::Scoped<Traits<Type> >   \
{                                                                             \
  typedef mozilla::Scoped<Traits<Type> > Super;                               \
  typedef typename Super::Resource Resource;                                  \
  name& operator=(Resource aRhs)                                              \
  {                                                                           \
    Super::operator=(aRhs);                                                   \
    return *this;                                                             \
  }                                                                           \
  name& operator=(name&& aRhs)                                                \
  {                                                                           \
    Super::operator=(Move(aRhs));                                             \
    return *this;                                                             \
  }                                                                           \
  explicit name(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)                         \
    : Super(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM_TO_PARENT)                   \
  {}                                                                          \
  explicit name(Resource aRhs                                                 \
                MOZ_GUARD_OBJECT_NOTIFIER_PARAM)                              \
    : Super(aRhs                                                              \
            MOZ_GUARD_OBJECT_NOTIFIER_PARAM_TO_PARENT)                        \
  {}                                                                          \
  name(name&& aRhs                                                            \
       MOZ_GUARD_OBJECT_NOTIFIER_PARAM)                                       \
    : Super(Move(aRhs)                                                        \
            MOZ_GUARD_OBJECT_NOTIFIER_PARAM_TO_PARENT)                        \
  {}                                                                          \
private:                                                                      \
  explicit name(name&) = delete;                                              \
  name& operator=(name&) = delete;                                            \
};

/*
 * ScopedFreePtr is a RAII wrapper for pointers that need to be free()d.
 *
 *   struct S { ... };
 *   ScopedFreePtr<S> foo = malloc(sizeof(S));
 *   ScopedFreePtr<char> bar = strdup(str);
 */
template<typename T>
struct ScopedFreePtrTraits
{
  typedef T* type;
  static T* empty() { return nullptr; }
  static void release(T* aPtr) { free(aPtr); }
};
SCOPED_TEMPLATE(ScopedFreePtr, ScopedFreePtrTraits)

/*
 * ScopedDeletePtr is a RAII wrapper for pointers that need to be deleted.
 *
 *   struct S { ... };
 *   ScopedDeletePtr<S> foo = new S();
 */
template<typename T>
struct ScopedDeletePtrTraits : public ScopedFreePtrTraits<T>
{
  static void release(T* aPtr) { delete aPtr; }
};
SCOPED_TEMPLATE(ScopedDeletePtr, ScopedDeletePtrTraits)

/*
 * MOZ_TYPE_SPECIFIC_SCOPED_POINTER_TEMPLATE makes it easy to create scoped
 * pointers for types with custom deleters; just overload
 * TypeSpecificDelete(T*) in the same namespace as T to call the deleter for
 * type T.
 *
 * @param name The name of the class to define.
 * @param Type A struct implementing clean-up. See the implementations
 * for more details.
 * *param Deleter The function that is used to delete/destroy/free a
 *        non-null value of Type*.
 *
 * Example:
 *
 *   MOZ_TYPE_SPECIFIC_SCOPED_POINTER_TEMPLATE(ScopedPRFileDesc, PRFileDesc, \
 *                                             PR_Close)
 *   ...
 *   {
 *       ScopedPRFileDesc file(PR_OpenFile(...));
 *       ...
 *   } // file is closed with PR_Close here
 */
#define MOZ_TYPE_SPECIFIC_SCOPED_POINTER_TEMPLATE(name, Type, Deleter) \
template <> inline void TypeSpecificDelete(Type* aValue) { Deleter(aValue); } \
typedef ::mozilla::TypeSpecificScopedPointer<Type> name;

template <typename T> void TypeSpecificDelete(T* aValue);

template <typename T>
struct TypeSpecificScopedPointerTraits
{
  typedef T* type;
  static type empty() { return nullptr; }
  static void release(type aValue)
  {
    if (aValue) {
      TypeSpecificDelete(aValue);
    }
  }
};

SCOPED_TEMPLATE(TypeSpecificScopedPointer, TypeSpecificScopedPointerTraits)

} /* namespace mozilla */

#endif /* mozilla_Scoped_h */
