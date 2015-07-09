/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS allows using a typed enum as bit flags.
 */

#ifndef mozilla_TypedEnumBits_h
#define mozilla_TypedEnumBits_h

#include "mozilla/Attributes.h"
#include "mozilla/IntegerTypeTraits.h"

namespace mozilla {

/*
 * The problem that CastableTypedEnumResult aims to solve is that
 * typed enums are not convertible to bool, and there is no way to make them
 * be, yet user code wants to be able to write
 *
 *   if (myFlags & Flags::SOME_PARTICULAR_FLAG)              (1)
 *
 * There are different approaches to solving this. Most of them require
 * adapting user code. For example, we could implement operator! and have
 * the user write
 *
 *   if (!!(myFlags & Flags::SOME_PARTICULAR_FLAG))          (2)
 *
 * Or we could supply a IsNonZero() or Any() function returning whether
 * an enum value is nonzero, and have the user write
 *
 *   if (Any(Flags & Flags::SOME_PARTICULAR_FLAG))           (3)
 *
 * But instead, we choose to preserve the original user syntax (1) as it
 * is inherently more readable, and to ease porting existing code to typed
 * enums. We achieve this by having operator& and other binary bitwise
 * operators have as return type a class, CastableTypedEnumResult,
 * that wraps a typed enum but adds bool convertibility.
 */
template<typename E>
class CastableTypedEnumResult
{
private:
  const E mValue;

public:
  explicit MOZ_CONSTEXPR CastableTypedEnumResult(E aValue)
    : mValue(aValue)
  {}

  MOZ_CONSTEXPR operator E() const { return mValue; }

  template<typename DestinationType>
  MOZ_EXPLICIT_CONVERSION MOZ_CONSTEXPR
  operator DestinationType() const { return DestinationType(mValue); }

  MOZ_CONSTEXPR bool operator !() const { return !bool(mValue); }
};

#define MOZ_CASTABLETYPEDENUMRESULT_BINOP(Op, OtherType, ReturnType) \
template<typename E> \
MOZ_CONSTEXPR ReturnType \
operator Op(const OtherType& aE, const CastableTypedEnumResult<E>& aR) \
{ \
  return ReturnType(aE Op OtherType(aR)); \
} \
template<typename E> \
MOZ_CONSTEXPR ReturnType \
operator Op(const CastableTypedEnumResult<E>& aR, const OtherType& aE) \
{ \
  return ReturnType(OtherType(aR) Op aE); \
} \
template<typename E> \
MOZ_CONSTEXPR ReturnType \
operator Op(const CastableTypedEnumResult<E>& aR1, \
            const CastableTypedEnumResult<E>& aR2) \
{ \
  return ReturnType(OtherType(aR1) Op OtherType(aR2)); \
}

MOZ_CASTABLETYPEDENUMRESULT_BINOP(|, E, CastableTypedEnumResult<E>)
MOZ_CASTABLETYPEDENUMRESULT_BINOP(&, E, CastableTypedEnumResult<E>)
MOZ_CASTABLETYPEDENUMRESULT_BINOP(^, E, CastableTypedEnumResult<E>)
MOZ_CASTABLETYPEDENUMRESULT_BINOP(==, E, bool)
MOZ_CASTABLETYPEDENUMRESULT_BINOP(!=, E, bool)
MOZ_CASTABLETYPEDENUMRESULT_BINOP(||, bool, bool)
MOZ_CASTABLETYPEDENUMRESULT_BINOP(&&, bool, bool)

template <typename E>
MOZ_CONSTEXPR CastableTypedEnumResult<E>
operator ~(const CastableTypedEnumResult<E>& aR)
{
  return CastableTypedEnumResult<E>(~(E(aR)));
}

#define MOZ_CASTABLETYPEDENUMRESULT_COMPOUND_ASSIGN_OP(Op) \
template<typename E> \
E& \
operator Op(E& aR1, \
            const CastableTypedEnumResult<E>& aR2) \
{ \
  return aR1 Op E(aR2); \
}

MOZ_CASTABLETYPEDENUMRESULT_COMPOUND_ASSIGN_OP(&=)
MOZ_CASTABLETYPEDENUMRESULT_COMPOUND_ASSIGN_OP(|=)
MOZ_CASTABLETYPEDENUMRESULT_COMPOUND_ASSIGN_OP(^=)

#undef MOZ_CASTABLETYPEDENUMRESULT_COMPOUND_ASSIGN_OP

#undef MOZ_CASTABLETYPEDENUMRESULT_BINOP

namespace detail {
template<typename E>
struct UnsignedIntegerTypeForEnum
  : UnsignedStdintTypeForSize<sizeof(E)>
{};
}

} // namespace mozilla

#define MOZ_MAKE_ENUM_CLASS_BINOP_IMPL(Name, Op) \
   inline MOZ_CONSTEXPR mozilla::CastableTypedEnumResult<Name> \
   operator Op(Name a, Name b) \
   { \
     typedef mozilla::CastableTypedEnumResult<Name> Result; \
     typedef mozilla::detail::UnsignedIntegerTypeForEnum<Name>::Type U; \
     return Result(Name(U(a) Op U(b))); \
   } \
 \
   inline Name& \
   operator Op##=(Name& a, Name b) \
   { \
     return a = a Op b; \
   }

/**
 * MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS generates standard bitwise operators
 * for the given enum type. Use this to enable using an enum type as bit-field.
 */
#define MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(Name) \
   MOZ_MAKE_ENUM_CLASS_BINOP_IMPL(Name, |) \
   MOZ_MAKE_ENUM_CLASS_BINOP_IMPL(Name, &) \
   MOZ_MAKE_ENUM_CLASS_BINOP_IMPL(Name, ^) \
   inline MOZ_CONSTEXPR mozilla::CastableTypedEnumResult<Name> \
   operator~(Name a) \
   { \
     typedef mozilla::CastableTypedEnumResult<Name> Result; \
     typedef mozilla::detail::UnsignedIntegerTypeForEnum<Name>::Type U; \
     return Result(Name(~(U(a)))); \
   }

#endif // mozilla_TypedEnumBits_h
