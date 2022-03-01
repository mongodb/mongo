/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* A class for optional values and in-place lazy construction. */

#ifndef mozilla_Maybe_h
#define mozilla_Maybe_h

#include <new>  // for placement new
#include <ostream>
#include <type_traits>
#include <utility>

#include "mozilla/Alignment.h"
#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/MaybeStorageBase.h"
#include "mozilla/MemoryChecking.h"
#include "mozilla/OperatorNewExtensions.h"
#include "mozilla/Poison.h"

class nsCycleCollectionTraversalCallback;

template <typename T>
inline void CycleCollectionNoteChild(
    nsCycleCollectionTraversalCallback& aCallback, T* aChild, const char* aName,
    uint32_t aFlags);

namespace mozilla {

struct Nothing {};

inline constexpr bool operator==(const Nothing&, const Nothing&) {
  return true;
}

template <class T>
class Maybe;

namespace detail {

// You would think that poisoning Maybe instances could just be a call
// to mozWritePoison.  Unfortunately, using a simple call to
// mozWritePoison generates poor code on MSVC for small structures.  The
// generated code contains (always not-taken) branches and does a bunch
// of setup for `rep stos{l,q}`, even though we know at compile time
// exactly how many words we're poisoning.  Instead, we're going to
// force MSVC to generate the code we want via recursive templates.

// Write the given poisonValue into p at offset*sizeof(uintptr_t).
template <size_t offset>
inline void WritePoisonAtOffset(void* p, const uintptr_t poisonValue) {
  memcpy(static_cast<char*>(p) + offset * sizeof(poisonValue), &poisonValue,
         sizeof(poisonValue));
}

template <size_t Offset, size_t NOffsets>
struct InlinePoisoner {
  static void poison(void* p, const uintptr_t poisonValue) {
    WritePoisonAtOffset<Offset>(p, poisonValue);
    InlinePoisoner<Offset + 1, NOffsets>::poison(p, poisonValue);
  }
};

template <size_t N>
struct InlinePoisoner<N, N> {
  static void poison(void*, const uintptr_t) {
    // All done!
  }
};

// We can't generate inline code for large structures, though, because we'll
// blow out recursive template instantiation limits, and the code would be
// bloated to boot.  So provide a fallback to the out-of-line poisoner.
template <size_t ObjectSize>
struct OutOfLinePoisoner {
  static MOZ_NEVER_INLINE void poison(void* p, const uintptr_t) {
    mozWritePoison(p, ObjectSize);
  }
};

template <typename T>
inline void PoisonObject(T* p) {
  const uintptr_t POISON = mozPoisonValue();
  std::conditional_t<(sizeof(T) <= 8 * sizeof(POISON)),
                     InlinePoisoner<0, sizeof(T) / sizeof(POISON)>,
                     OutOfLinePoisoner<sizeof(T)>>::poison(p, POISON);
}

template <typename T>
struct MaybePoisoner {
  static const size_t N = sizeof(T);

  static void poison(void* aPtr) {
#ifdef MOZ_DIAGNOSTIC_ASSERT_ENABLED
    if (N >= sizeof(uintptr_t)) {
      PoisonObject(static_cast<std::remove_cv_t<T>*>(aPtr));
    }
#endif
    MOZ_MAKE_MEM_UNDEFINED(aPtr, N);
  }
};

template <typename T,
          bool TriviallyDestructibleAndCopyable =
              IsTriviallyDestructibleAndCopyable<T>,
          bool Copyable = std::is_copy_constructible_v<T>,
          bool Movable = std::is_move_constructible_v<T>>
class Maybe_CopyMove_Enabler;

#define MOZ_MAYBE_COPY_OPS()                                                \
  Maybe_CopyMove_Enabler(const Maybe_CopyMove_Enabler& aOther) {            \
    if (downcast(aOther).isSome()) {                                        \
      downcast(*this).emplace(*downcast(aOther));                           \
    }                                                                       \
  }                                                                         \
                                                                            \
  Maybe_CopyMove_Enabler& operator=(const Maybe_CopyMove_Enabler& aOther) { \
    return downcast(*this).template operator=<T>(downcast(aOther));         \
  }

#define MOZ_MAYBE_MOVE_OPS()                                            \
  constexpr Maybe_CopyMove_Enabler(Maybe_CopyMove_Enabler&& aOther) {   \
    if (downcast(aOther).isSome()) {                                    \
      downcast(*this).emplace(std::move(*downcast(aOther)));            \
      downcast(aOther).reset();                                         \
    }                                                                   \
  }                                                                     \
                                                                        \
  constexpr Maybe_CopyMove_Enabler& operator=(                          \
      Maybe_CopyMove_Enabler&& aOther) {                                \
    downcast(*this).template operator=<T>(std::move(downcast(aOther))); \
                                                                        \
    return *this;                                                       \
  }

#define MOZ_MAYBE_DOWNCAST()                                          \
  static constexpr Maybe<T>& downcast(Maybe_CopyMove_Enabler& aObj) { \
    return static_cast<Maybe<T>&>(aObj);                              \
  }                                                                   \
  static constexpr const Maybe<T>& downcast(                          \
      const Maybe_CopyMove_Enabler& aObj) {                           \
    return static_cast<const Maybe<T>&>(aObj);                        \
  }

template <typename T>
class Maybe_CopyMove_Enabler<T, true, true, true> {
 public:
  Maybe_CopyMove_Enabler() = default;

  Maybe_CopyMove_Enabler(const Maybe_CopyMove_Enabler&) = default;
  Maybe_CopyMove_Enabler& operator=(const Maybe_CopyMove_Enabler&) = default;
  constexpr Maybe_CopyMove_Enabler(Maybe_CopyMove_Enabler&& aOther) {
    downcast(aOther).reset();
  }
  constexpr Maybe_CopyMove_Enabler& operator=(Maybe_CopyMove_Enabler&& aOther) {
    downcast(aOther).reset();
    return *this;
  }

 private:
  MOZ_MAYBE_DOWNCAST()
};

template <typename T>
class Maybe_CopyMove_Enabler<T, true, false, true> {
 public:
  Maybe_CopyMove_Enabler() = default;

  Maybe_CopyMove_Enabler(const Maybe_CopyMove_Enabler&) = delete;
  Maybe_CopyMove_Enabler& operator=(const Maybe_CopyMove_Enabler&) = delete;
  constexpr Maybe_CopyMove_Enabler(Maybe_CopyMove_Enabler&& aOther) {
    downcast(aOther).reset();
  }
  constexpr Maybe_CopyMove_Enabler& operator=(Maybe_CopyMove_Enabler&& aOther) {
    downcast(aOther).reset();
    return *this;
  }

 private:
  MOZ_MAYBE_DOWNCAST()
};

template <typename T>
class Maybe_CopyMove_Enabler<T, false, true, true> {
 public:
  Maybe_CopyMove_Enabler() = default;

  MOZ_MAYBE_COPY_OPS()
  MOZ_MAYBE_MOVE_OPS()

 private:
  MOZ_MAYBE_DOWNCAST()
};

template <typename T>
class Maybe_CopyMove_Enabler<T, false, false, true> {
 public:
  Maybe_CopyMove_Enabler() = default;

  MOZ_MAYBE_MOVE_OPS()

 private:
  MOZ_MAYBE_DOWNCAST()
};

template <typename T>
class Maybe_CopyMove_Enabler<T, false, true, false> {
 public:
  Maybe_CopyMove_Enabler() = default;

  MOZ_MAYBE_COPY_OPS()

 private:
  MOZ_MAYBE_DOWNCAST()
};

template <typename T, bool TriviallyDestructibleAndCopyable>
class Maybe_CopyMove_Enabler<T, TriviallyDestructibleAndCopyable, false,
                             false> {
 public:
  Maybe_CopyMove_Enabler() = default;

  Maybe_CopyMove_Enabler(const Maybe_CopyMove_Enabler&) = delete;
  Maybe_CopyMove_Enabler& operator=(const Maybe_CopyMove_Enabler&) = delete;
  Maybe_CopyMove_Enabler(Maybe_CopyMove_Enabler&&) = delete;
  Maybe_CopyMove_Enabler& operator=(Maybe_CopyMove_Enabler&&) = delete;
};

#undef MOZ_MAYBE_COPY_OPS
#undef MOZ_MAYBE_MOVE_OPS
#undef MOZ_MAYBE_DOWNCAST

template <typename T, bool TriviallyDestructibleAndCopyable =
                          IsTriviallyDestructibleAndCopyable<T>>
struct MaybeStorage;

template <typename T>
struct MaybeStorage<T, false> : MaybeStorageBase<T> {
 protected:
  char mIsSome = false;  // not bool -- guarantees minimal space consumption

  MaybeStorage() = default;
  explicit MaybeStorage(const T& aVal)
      : MaybeStorageBase<T>{aVal}, mIsSome{true} {}
  explicit MaybeStorage(T&& aVal)
      : MaybeStorageBase<T>{std::move(aVal)}, mIsSome{true} {}

  template <typename... Args>
  explicit MaybeStorage(std::in_place_t, Args&&... aArgs)
      : MaybeStorageBase<T>{std::in_place, std::forward<Args>(aArgs)...},
        mIsSome{true} {}

 public:
  // Copy and move operations are no-ops, since copying is moving is implemented
  // by Maybe_CopyMove_Enabler.

  MaybeStorage(const MaybeStorage&) : MaybeStorageBase<T>{} {}
  MaybeStorage& operator=(const MaybeStorage&) { return *this; }
  MaybeStorage(MaybeStorage&&) : MaybeStorageBase<T>{} {}
  MaybeStorage& operator=(MaybeStorage&&) { return *this; }

  ~MaybeStorage() {
    // This alias works around a compiler bug described here:
    // https://developercommunity.visualstudio.com/t/Explicit-destructor-call-inside-a-templa/1470165
    using alias_of_T = T;

    if (mIsSome) {
      this->addr()->alias_of_T::~alias_of_T();
    }
  }
};

template <typename T>
struct MaybeStorage<T, true> : MaybeStorageBase<T> {
 protected:
  char mIsSome = false;  // not bool -- guarantees minimal space consumption

  constexpr MaybeStorage() = default;
  constexpr explicit MaybeStorage(const T& aVal)
      : MaybeStorageBase<T>{aVal}, mIsSome{true} {}
  constexpr explicit MaybeStorage(T&& aVal)
      : MaybeStorageBase<T>{std::move(aVal)}, mIsSome{true} {}

  template <typename... Args>
  constexpr explicit MaybeStorage(std::in_place_t, Args&&... aArgs)
      : MaybeStorageBase<T>{std::in_place, std::forward<Args>(aArgs)...},
        mIsSome{true} {}
};

}  // namespace detail

template <typename T, typename U = typename std::remove_cv<
                          typename std::remove_reference<T>::type>::type>
constexpr Maybe<U> Some(T&& aValue);

/*
 * Maybe is a container class which contains either zero or one elements. It
 * serves two roles. It can represent values which are *semantically* optional,
 * augmenting a type with an explicit 'Nothing' value. In this role, it provides
 * methods that make it easy to work with values that may be missing, along with
 * equality and comparison operators so that Maybe values can be stored in
 * containers. Maybe values can be constructed conveniently in expressions using
 * type inference, as follows:
 *
 *   void doSomething(Maybe<Foo> aFoo) {
 *     if (aFoo)                  // Make sure that aFoo contains a value...
 *       aFoo->takeAction();      // and then use |aFoo->| to access it.
 *   }                            // |*aFoo| also works!
 *
 *   doSomething(Nothing());      // Passes a Maybe<Foo> containing no value.
 *   doSomething(Some(Foo(100))); // Passes a Maybe<Foo> containing |Foo(100)|.
 *
 * You'll note that it's important to check whether a Maybe contains a value
 * before using it, using conversion to bool, |isSome()|, or |isNothing()|. You
 * can avoid these checks, and sometimes write more readable code, using
 * |valueOr()|, |ptrOr()|, and |refOr()|, which allow you to retrieve the value
 * in the Maybe and provide a default for the 'Nothing' case.  You can also use
 * |apply()| to call a function only if the Maybe holds a value, and |map()| to
 * transform the value in the Maybe, returning another Maybe with a possibly
 * different type.
 *
 * Maybe's other role is to support lazily constructing objects without using
 * dynamic storage. A Maybe directly contains storage for a value, but it's
 * empty by default. |emplace()|, as mentioned above, can be used to construct a
 * value in Maybe's storage.  The value a Maybe contains can be destroyed by
 * calling |reset()|; this will happen automatically if a Maybe is destroyed
 * while holding a value.
 *
 * It's a common idiom in C++ to use a pointer as a 'Maybe' type, with a null
 * value meaning 'Nothing' and any other value meaning 'Some'. You can convert
 * from such a pointer to a Maybe value using 'ToMaybe()'.
 *
 * Maybe is inspired by similar types in the standard library of many other
 * languages (e.g. Haskell's Maybe and Rust's Option). In the C++ world it's
 * very similar to std::optional, which was proposed for C++14 and originated in
 * Boost. The most important differences between Maybe and std::optional are:
 *
 *   - std::optional<T> may be compared with T. We deliberately forbid that.
 *   - std::optional has |valueOr()|, equivalent to Maybe's |valueOr()|, but
 *     lacks corresponding methods for |refOr()| and |ptrOr()|.
 *   - std::optional lacks |map()| and |apply()|, making it less suitable for
 *     functional-style code.
 *   - std::optional lacks many convenience functions that Maybe has. Most
 *     unfortunately, it lacks equivalents of the type-inferred constructor
 *     functions |Some()| and |Nothing()|.
 */
template <class T>
class MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS Maybe
    : private detail::MaybeStorage<T>,
      public detail::Maybe_CopyMove_Enabler<T> {
  template <typename, bool, bool, bool>
  friend class detail::Maybe_CopyMove_Enabler;

  template <typename U, typename V>
  friend constexpr Maybe<V> Some(U&& aValue);

  struct SomeGuard {};

  template <typename U>
  constexpr Maybe(U&& aValue, SomeGuard)
      : detail::MaybeStorage<T>{std::forward<U>(aValue)} {}

  using detail::MaybeStorage<T>::mIsSome;
  using detail::MaybeStorage<T>::mStorage;

  void poisonData() { detail::MaybePoisoner<T>::poison(&mStorage.val); }

 public:
  using ValueType = T;

  MOZ_ALLOW_TEMPORARY constexpr Maybe() = default;

  MOZ_ALLOW_TEMPORARY MOZ_IMPLICIT constexpr Maybe(Nothing) : Maybe{} {}

  template <typename... Args>
  constexpr explicit Maybe(std::in_place_t, Args&&... aArgs)
      : detail::MaybeStorage<T>{std::in_place, std::forward<Args>(aArgs)...} {}

  /**
   * Maybe<T> can be copy-constructed from a Maybe<U> if T is constructible from
   * a const U&.
   */
  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, const U&>>>
  MOZ_IMPLICIT Maybe(const Maybe<U>& aOther) {
    if (aOther.isSome()) {
      emplace(*aOther);
    }
  }

  /**
   * Maybe<T> can be move-constructed from a Maybe<U> if T is constructible from
   * a U&&.
   */
  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, U&&>>>
  MOZ_IMPLICIT Maybe(Maybe<U>&& aOther) {
    if (aOther.isSome()) {
      emplace(std::move(*aOther));
      aOther.reset();
    }
  }

  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, const U&>>>
  Maybe& operator=(const Maybe<U>& aOther) {
    if (aOther.isSome()) {
      if (mIsSome) {
        ref() = aOther.ref();
      } else {
        emplace(*aOther);
      }
    } else {
      reset();
    }
    return *this;
  }

  template <typename U,
            typename = std::enable_if_t<std::is_constructible_v<T, U&&>>>
  Maybe& operator=(Maybe<U>&& aOther) {
    if (aOther.isSome()) {
      if (mIsSome) {
        ref() = std::move(aOther.ref());
      } else {
        emplace(std::move(*aOther));
      }
      aOther.reset();
    } else {
      reset();
    }

    return *this;
  }

  constexpr Maybe& operator=(Nothing) {
    reset();
    return *this;
  }

  /* Methods that check whether this Maybe contains a value */
  constexpr explicit operator bool() const { return isSome(); }
  constexpr bool isSome() const { return mIsSome; }
  constexpr bool isNothing() const { return !mIsSome; }

  /* Returns the contents of this Maybe<T> by value. Unsafe unless |isSome()|.
   */
  constexpr T value() const&;
  constexpr T value() &&;
  constexpr T value() const&&;

  /**
   * Move the contents of this Maybe<T> out of internal storage and return it
   * without calling the destructor. The internal storage is also reset to
   * avoid multiple calls. Unsafe unless |isSome()|.
   */
  T extract() {
    MOZ_DIAGNOSTIC_ASSERT(isSome());
    T v = std::move(mStorage.val);
    reset();
    return v;
  }

  /**
   * Returns the value (possibly |Nothing()|) by moving it out of this Maybe<T>
   * and leaving |Nothing()| in its place.
   */
  Maybe<T> take() { return std::exchange(*this, Nothing()); }

  /*
   * Returns the contents of this Maybe<T> by value. If |isNothing()|, returns
   * the default value provided.
   *
   * Note: If the value passed to aDefault is not the result of a trivial
   * expression, but expensive to evaluate, e.g. |valueOr(ExpensiveFunction())|,
   * use |valueOrFrom| instead, e.g.
   * |valueOrFrom([arg] { return ExpensiveFunction(arg); })|. This ensures
   * that the expensive expression is only evaluated when its result will
   * actually be used.
   */
  template <typename V>
  constexpr T valueOr(V&& aDefault) const {
    if (isSome()) {
      return ref();
    }
    return std::forward<V>(aDefault);
  }

  /*
   * Returns the contents of this Maybe<T> by value. If |isNothing()|, returns
   * the value returned from the function or functor provided.
   */
  template <typename F>
  constexpr T valueOrFrom(F&& aFunc) const {
    if (isSome()) {
      return ref();
    }
    return aFunc();
  }

  /* Returns the contents of this Maybe<T> by pointer. Unsafe unless |isSome()|.
   */
  T* ptr();
  constexpr const T* ptr() const;

  /*
   * Returns the contents of this Maybe<T> by pointer. If |isNothing()|,
   * returns the default value provided.
   */
  T* ptrOr(T* aDefault) {
    if (isSome()) {
      return ptr();
    }
    return aDefault;
  }

  constexpr const T* ptrOr(const T* aDefault) const {
    if (isSome()) {
      return ptr();
    }
    return aDefault;
  }

  /*
   * Returns the contents of this Maybe<T> by pointer. If |isNothing()|,
   * returns the value returned from the function or functor provided.
   */
  template <typename F>
  T* ptrOrFrom(F&& aFunc) {
    if (isSome()) {
      return ptr();
    }
    return aFunc();
  }

  template <typename F>
  const T* ptrOrFrom(F&& aFunc) const {
    if (isSome()) {
      return ptr();
    }
    return aFunc();
  }

  constexpr T* operator->();
  constexpr const T* operator->() const;

  /* Returns the contents of this Maybe<T> by ref. Unsafe unless |isSome()|. */
  constexpr T& ref() &;
  constexpr const T& ref() const&;
  constexpr T&& ref() &&;
  constexpr const T&& ref() const&&;

  /*
   * Returns the contents of this Maybe<T> by ref. If |isNothing()|, returns
   * the default value provided.
   */
  constexpr T& refOr(T& aDefault) {
    if (isSome()) {
      return ref();
    }
    return aDefault;
  }

  constexpr const T& refOr(const T& aDefault) const {
    if (isSome()) {
      return ref();
    }
    return aDefault;
  }

  /*
   * Returns the contents of this Maybe<T> by ref. If |isNothing()|, returns the
   * value returned from the function or functor provided.
   */
  template <typename F>
  constexpr T& refOrFrom(F&& aFunc) {
    if (isSome()) {
      return ref();
    }
    return aFunc();
  }

  template <typename F>
  constexpr const T& refOrFrom(F&& aFunc) const {
    if (isSome()) {
      return ref();
    }
    return aFunc();
  }

  constexpr T& operator*() &;
  constexpr const T& operator*() const&;
  constexpr T&& operator*() &&;
  constexpr const T&& operator*() const&&;

  /* If |isSome()|, runs the provided function or functor on the contents of
   * this Maybe. */
  template <typename Func>
  constexpr Maybe& apply(Func&& aFunc) {
    if (isSome()) {
      std::forward<Func>(aFunc)(ref());
    }
    return *this;
  }

  template <typename Func>
  constexpr const Maybe& apply(Func&& aFunc) const {
    if (isSome()) {
      std::forward<Func>(aFunc)(ref());
    }
    return *this;
  }

  /*
   * If |isSome()|, runs the provided function and returns the result wrapped
   * in a Maybe. If |isNothing()|, returns an empty Maybe value with the same
   * value type as what the provided function would have returned.
   */
  template <typename Func>
  constexpr auto map(Func&& aFunc) {
    if (isSome()) {
      return Some(std::forward<Func>(aFunc)(ref()));
    }
    return Maybe<decltype(std::forward<Func>(aFunc)(ref()))>{};
  }

  template <typename Func>
  constexpr auto map(Func&& aFunc) const {
    if (isSome()) {
      return Some(std::forward<Func>(aFunc)(ref()));
    }
    return Maybe<decltype(std::forward<Func>(aFunc)(ref()))>{};
  }

  /* If |isSome()|, empties this Maybe and destroys its contents. */
  constexpr void reset() {
    if (isSome()) {
      if constexpr (!std::is_trivially_destructible_v<T>) {
        ref().T::~T();
        poisonData();
      }
      mIsSome = false;
    }
  }

  /*
   * Constructs a T value in-place in this empty Maybe<T>'s storage. The
   * arguments to |emplace()| are the parameters to T's constructor.
   */
  template <typename... Args>
  constexpr void emplace(Args&&... aArgs);

  template <typename U>
  constexpr std::enable_if_t<std::is_same_v<T, U> &&
                             std::is_copy_constructible_v<U> &&
                             !std::is_move_constructible_v<U>>
  emplace(U&& aArgs) {
    emplace(aArgs);
  }

  friend std::ostream& operator<<(std::ostream& aStream,
                                  const Maybe<T>& aMaybe) {
    if (aMaybe) {
      aStream << aMaybe.ref();
    } else {
      aStream << "<Nothing>";
    }
    return aStream;
  }
};

template <typename T>
class Maybe<T&> {
 public:
  constexpr Maybe() = default;
  constexpr MOZ_IMPLICIT Maybe(Nothing) {}

  void emplace(T& aRef) { mValue = &aRef; }

  /* Methods that check whether this Maybe contains a value */
  constexpr explicit operator bool() const { return isSome(); }
  constexpr bool isSome() const { return mValue; }
  constexpr bool isNothing() const { return !mValue; }

  T& ref() const {
    MOZ_DIAGNOSTIC_ASSERT(isSome());
    return *mValue;
  }

  T* operator->() const { return &ref(); }
  T& operator*() const { return ref(); }

  // Deliberately not defining value and ptr accessors, as these may be
  // confusing on a reference-typed Maybe.

  // XXX Should we define refOr?

  void reset() { mValue = nullptr; }

  template <typename Func>
  Maybe& apply(Func&& aFunc) {
    if (isSome()) {
      std::forward<Func>(aFunc)(ref());
    }
    return *this;
  }

  template <typename Func>
  const Maybe& apply(Func&& aFunc) const {
    if (isSome()) {
      std::forward<Func>(aFunc)(ref());
    }
    return *this;
  }

  template <typename Func>
  auto map(Func&& aFunc) {
    Maybe<decltype(std::forward<Func>(aFunc)(ref()))> val;
    if (isSome()) {
      val.emplace(std::forward<Func>(aFunc)(ref()));
    }
    return val;
  }

  template <typename Func>
  auto map(Func&& aFunc) const {
    Maybe<decltype(std::forward<Func>(aFunc)(ref()))> val;
    if (isSome()) {
      val.emplace(std::forward<Func>(aFunc)(ref()));
    }
    return val;
  }

  bool refEquals(const Maybe<T&>& aOther) const {
    return mValue == aOther.mValue;
  }

  bool refEquals(const T& aOther) const { return mValue == &aOther; }

 private:
  T* mValue = nullptr;
};

template <typename T>
constexpr T Maybe<T>::value() const& {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return ref();
}

template <typename T>
constexpr T Maybe<T>::value() && {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return std::move(ref());
}

template <typename T>
constexpr T Maybe<T>::value() const&& {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return std::move(ref());
}

template <typename T>
T* Maybe<T>::ptr() {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return &ref();
}

template <typename T>
constexpr const T* Maybe<T>::ptr() const {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return &ref();
}

template <typename T>
constexpr T* Maybe<T>::operator->() {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return ptr();
}

template <typename T>
constexpr const T* Maybe<T>::operator->() const {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return ptr();
}

template <typename T>
constexpr T& Maybe<T>::ref() & {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return mStorage.val;
}

template <typename T>
constexpr const T& Maybe<T>::ref() const& {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return mStorage.val;
}

template <typename T>
constexpr T&& Maybe<T>::ref() && {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return std::move(mStorage.val);
}

template <typename T>
constexpr const T&& Maybe<T>::ref() const&& {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return std::move(mStorage.val);
}

template <typename T>
constexpr T& Maybe<T>::operator*() & {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return ref();
}

template <typename T>
constexpr const T& Maybe<T>::operator*() const& {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return ref();
}

template <typename T>
constexpr T&& Maybe<T>::operator*() && {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return std::move(ref());
}

template <typename T>
constexpr const T&& Maybe<T>::operator*() const&& {
  MOZ_DIAGNOSTIC_ASSERT(isSome());
  return std::move(ref());
}

template <typename T>
template <typename... Args>
constexpr void Maybe<T>::emplace(Args&&... aArgs) {
  MOZ_DIAGNOSTIC_ASSERT(!isSome());
  ::new (KnownNotNull, &mStorage.val) T(std::forward<Args>(aArgs)...);
  mIsSome = true;
}

/*
 * Some() creates a Maybe<T> value containing the provided T value. If T has a
 * move constructor, it's used to make this as efficient as possible.
 *
 * Some() selects the type of Maybe it returns by removing any const, volatile,
 * or reference qualifiers from the type of the value you pass to it. This gives
 * it more intuitive behavior when used in expressions, but it also means that
 * if you need to construct a Maybe value that holds a const, volatile, or
 * reference value, you need to use emplace() instead.
 */
template <typename T, typename U>
constexpr Maybe<U> Some(T&& aValue) {
  return {std::forward<T>(aValue), typename Maybe<U>::SomeGuard{}};
}

template <typename T>
constexpr Maybe<T&> SomeRef(T& aValue) {
  Maybe<T&> value;
  value.emplace(aValue);
  return value;
}

template <typename T>
constexpr Maybe<T&> ToMaybeRef(T* const aPtr) {
  return aPtr ? SomeRef(*aPtr) : Nothing{};
}

template <typename T>
Maybe<std::remove_cv_t<std::remove_reference_t<T>>> ToMaybe(T* aPtr) {
  if (aPtr) {
    return Some(*aPtr);
  }
  return Nothing();
}

/*
 * Two Maybe<T> values are equal if
 * - both are Nothing, or
 * - both are Some, and the values they contain are equal.
 */
template <typename T>
constexpr bool operator==(const Maybe<T>& aLHS, const Maybe<T>& aRHS) {
  static_assert(!std::is_reference_v<T>,
                "operator== is not defined for Maybe<T&>, compare values or "
                "addresses explicitly instead");
  if (aLHS.isNothing() != aRHS.isNothing()) {
    return false;
  }
  return aLHS.isNothing() || *aLHS == *aRHS;
}

template <typename T>
constexpr bool operator!=(const Maybe<T>& aLHS, const Maybe<T>& aRHS) {
  return !(aLHS == aRHS);
}

/*
 * We support comparison to Nothing to allow reasonable expressions like:
 *   if (maybeValue == Nothing()) { ... }
 */
template <typename T>
constexpr bool operator==(const Maybe<T>& aLHS, const Nothing& aRHS) {
  return aLHS.isNothing();
}

template <typename T>
constexpr bool operator!=(const Maybe<T>& aLHS, const Nothing& aRHS) {
  return !(aLHS == aRHS);
}

template <typename T>
constexpr bool operator==(const Nothing& aLHS, const Maybe<T>& aRHS) {
  return aRHS.isNothing();
}

template <typename T>
constexpr bool operator!=(const Nothing& aLHS, const Maybe<T>& aRHS) {
  return !(aLHS == aRHS);
}

/*
 * Maybe<T> values are ordered in the same way T values are ordered, except that
 * Nothing comes before anything else.
 */
template <typename T>
constexpr bool operator<(const Maybe<T>& aLHS, const Maybe<T>& aRHS) {
  if (aLHS.isNothing()) {
    return aRHS.isSome();
  }
  if (aRHS.isNothing()) {
    return false;
  }
  return *aLHS < *aRHS;
}

template <typename T>
constexpr bool operator>(const Maybe<T>& aLHS, const Maybe<T>& aRHS) {
  return !(aLHS < aRHS || aLHS == aRHS);
}

template <typename T>
constexpr bool operator<=(const Maybe<T>& aLHS, const Maybe<T>& aRHS) {
  return aLHS < aRHS || aLHS == aRHS;
}

template <typename T>
constexpr bool operator>=(const Maybe<T>& aLHS, const Maybe<T>& aRHS) {
  return !(aLHS < aRHS);
}

template <typename T>
inline void ImplCycleCollectionTraverse(
    nsCycleCollectionTraversalCallback& aCallback, mozilla::Maybe<T>& aField,
    const char* aName, uint32_t aFlags = 0) {
  if (aField) {
    ImplCycleCollectionTraverse(aCallback, aField.ref(), aName, aFlags);
  }
}

template <typename T>
inline void ImplCycleCollectionUnlink(mozilla::Maybe<T>& aField) {
  if (aField) {
    ImplCycleCollectionUnlink(aField.ref());
  }
}

}  // namespace mozilla

#endif /* mozilla_Maybe_h */
