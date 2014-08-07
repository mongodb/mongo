//  scoped_enum_emulation.hpp  ---------------------------------------------------------//

//  Copyright Beman Dawes, 2009
//  Copyright (C) 2011-2012 Vicente J. Botet Escriba
//  Copyright (C) 2012 Anthony Williams

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

/*
[section:scoped_enums Scoped Enums]

Generates C++0x scoped enums if the feature is present, otherwise emulates C++0x
scoped enums with C++03 namespaces and enums. The Boost.Config BOOST_NO_CXX11_SCOPED_ENUMS
macro is used to detect feature support.

See http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2007/n2347.pdf for a
description of the scoped enum feature. Note that the committee changed the name
from strongly typed enum to scoped enum.

Some of the enumerations defined in the standard library are scoped enums.

  enum class future_errc
  {
      broken_promise,
      future_already_retrieved,
      promise_already_satisfied,
      no_state
  };

On compilers that don't support them, the library provides two emulations:

[heading Strict]

* Able to specify the underlying type.
* explicit conversion to/from underlying type.
* The wrapper is not a C++03 enum type.

The user can declare  declare these types as

  BOOST_SCOPED_ENUM_DECLARE_BEGIN(future_errc)
  {
      broken_promise,
      future_already_retrieved,
      promise_already_satisfied,
      no_state
  }
  BOOST_SCOPED_ENUM_DECLARE_END(future_errc)

These macros allows to use 'future_errc' in almost all the cases as an scoped enum.

  future_errc err = future_errc::no_state;

There are however some limitations:

* The type is not a C++ enum, so 'is_enum<future_errc>' will be false_type.
* The emulated scoped enum can not be used in switch nor in template arguments. For these cases the user needs to use some macros.

Instead of

        switch (ev)
        {
        case future_errc::broken_promise:
  // ...

use

        switch (boost::native_value(ev))
        {
        case future_errc::broken_promise:

And instead of

    #ifdef BOOST_NO_CXX11_SCOPED_ENUMS
    template <>
    struct BOOST_SYMBOL_VISIBLE is_error_code_enum<future_errc> : public true_type { };
    #endif

use

    #ifdef BOOST_NO_CXX11_SCOPED_ENUMS
    template <>
    struct BOOST_SYMBOL_VISIBLE is_error_code_enum<future_errc::enum_type > : public true_type { };
    #endif


Sample usage:

     BOOST_SCOPED_ENUM_UT_DECLARE_BEGIN(algae, char) { green, red, cyan }; BOOST_SCOPED_ENUM_DECLARE_END(algae)
     ...
     algae sample( algae::red );
     void foo( algae color );
     ...
     sample = algae::green;
     foo( algae::cyan );

 Light
  Caution: only the syntax is emulated; the semantics are not emulated and
  the syntax emulation doesn't include being able to specify the underlying
  representation type.

  The literal scoped emulation is via struct rather than namespace to allow use within classes.
  Thanks to Andrey Semashev for pointing that out.
  However the type is an real C++03 enum and so convertible implicitly to an int.

  Sample usage:

     BOOST_SCOPED_ENUM_START(algae) { green, red, cyan }; BOOST_SCOPED_ENUM_END
     ...
     BOOST_SCOPED_ENUM(algae) sample( algae::red );
     void foo( BOOST_SCOPED_ENUM(algae) color );
     ...
     sample = algae::green;
     foo( algae::cyan );

  Helpful comments and suggestions were also made by Kjell Elster, Phil Endecott,
  Joel Falcou, Mathias Gaunard, Felipe Magno de Almeida, Matt Calabrese, Vicente
  Botet, and Daniel James.

[endsect]
*/


#ifndef BOOST_SCOPED_ENUM_EMULATION_HPP
#define BOOST_SCOPED_ENUM_EMULATION_HPP

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

namespace boost
{

#ifdef BOOST_NO_CXX11_SCOPED_ENUMS
  /**
   * Meta-function to get the underlying type of a scoped enum.
   *
   * Requires EnumType must be an enum type or the emulation of a scoped enum
   */
  template <typename EnumType>
  struct underlying_type
  {
    /**
     * The member typedef type names the underlying type of EnumType. It is EnumType::underlying_type when the EnumType is an emulated scoped enum,
     * std::underlying_type<EnumType>::type when the standard library std::underlying_type is provided.
     *
     * The user will need to specialize it when the compiler supports scoped enums but don't provides std::underlying_type.
     */
    typedef typename EnumType::underlying_type type;
  };

  /**
   * Meta-function to get the native enum type associated to an enum class or its emulation.
   */
  template <typename EnumType>
  struct native_type
  {
    /**
     * The member typedef type names the native enum type associated to the scoped enum,
     * which is it self if the compiler supports scoped enums or EnumType::enum_type if it is an emulated scoped enum.
     */
    typedef typename EnumType::enum_type type;
  };

  /**
   * Casts a scoped enum to its underlying type.
   *
   * This function is useful when working with scoped enum classes, which doens't implicitly convert to the underlying type.
   * @param v A scoped enum.
   * @returns The underlying type.
   * @throws No-throws.
   */
  template <typename UnderlyingType, typename EnumType>
  UnderlyingType underlying_cast(EnumType v)
  {
    return v.get_underlying_value_();
  }

  /**
   * Casts a scoped enum to its native enum type.
   *
   * This function is useful to make programs portable when the scoped enum emulation can not be use where native enums can.
   *
   * EnumType the scoped enum type
   *
   * @param v A scoped enum.
   * @returns The native enum value.
   * @throws No-throws.
   */
  template <typename EnumType>
  inline
  typename EnumType::enum_type native_value(EnumType e)
  {
    return e.native_value_();
  }

#else  // BOOST_NO_CXX11_SCOPED_ENUMS

  template <typename EnumType>
  struct underlying_type
  {
    //typedef typename std::underlying_type<EnumType>::type type;
  };

  template <typename EnumType>
  struct native_type
  {
    typedef EnumType type;
  };

  template <typename UnderlyingType, typename EnumType>
  UnderlyingType underlying_cast(EnumType v)
  {
    return static_cast<UnderlyingType>(v);
  }

  template <typename EnumType>
  inline
  EnumType native_value(EnumType e)
  {
    return e;
 }

#endif
}


#ifdef BOOST_NO_CXX11_SCOPED_ENUMS

#ifndef BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS

#define BOOST_SCOPED_ENUM_UT_DECLARE_CONVERSION_OPERATOR \
     explicit operator underlying_type() const { return get_underlying_value_(); }

#else

#define BOOST_SCOPED_ENUM_UT_DECLARE_CONVERSION_OPERATOR

#endif

/**
 * Start a declaration of a scoped enum.
 *
 * @param EnumType The new scoped enum.
 * @param UnderlyingType The underlying type.
 */
#define BOOST_SCOPED_ENUM_UT_DECLARE_BEGIN(EnumType, UnderlyingType)    \
    struct EnumType {                                                   \
        typedef UnderlyingType underlying_type;                         \
        EnumType() BOOST_NOEXCEPT {}                                    \
        explicit EnumType(underlying_type v) : v_(v) {}                 \
        underlying_type get_underlying_value_() const { return v_; }               \
        BOOST_SCOPED_ENUM_UT_DECLARE_CONVERSION_OPERATOR                \
    private:                                                            \
        underlying_type v_;                                             \
        typedef EnumType self_type;                                     \
    public:                                                             \
        enum enum_type

#define BOOST_SCOPED_ENUM_DECLARE_END2() \
        enum_type get_native_value_() const BOOST_NOEXCEPT { return enum_type(v_); } \
        operator enum_type() const BOOST_NOEXCEPT { return get_native_value_(); } \
        friend bool operator ==(self_type lhs, self_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)==enum_type(rhs.v_); } \
        friend bool operator ==(self_type lhs, enum_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)==rhs; } \
        friend bool operator ==(enum_type lhs, self_type rhs) BOOST_NOEXCEPT { return lhs==enum_type(rhs.v_); } \
        friend bool operator !=(self_type lhs, self_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)!=enum_type(rhs.v_); } \
        friend bool operator !=(self_type lhs, enum_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)!=rhs; } \
        friend bool operator !=(enum_type lhs, self_type rhs) BOOST_NOEXCEPT { return lhs!=enum_type(rhs.v_); } \
        friend bool operator <(self_type lhs, self_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)<enum_type(rhs.v_); } \
        friend bool operator <(self_type lhs, enum_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)<rhs; } \
        friend bool operator <(enum_type lhs, self_type rhs) BOOST_NOEXCEPT { return lhs<enum_type(rhs.v_); } \
        friend bool operator <=(self_type lhs, self_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)<=enum_type(rhs.v_); } \
        friend bool operator <=(self_type lhs, enum_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)<=rhs; } \
        friend bool operator <=(enum_type lhs, self_type rhs) BOOST_NOEXCEPT { return lhs<=enum_type(rhs.v_); } \
        friend bool operator >(self_type lhs, self_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)>enum_type(rhs.v_); } \
        friend bool operator >(self_type lhs, enum_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)>rhs; } \
        friend bool operator >(enum_type lhs, self_type rhs) BOOST_NOEXCEPT { return lhs>enum_type(rhs.v_); } \
        friend bool operator >=(self_type lhs, self_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)>=enum_type(rhs.v_); } \
        friend bool operator >=(self_type lhs, enum_type rhs) BOOST_NOEXCEPT { return enum_type(lhs.v_)>=rhs; } \
        friend bool operator >=(enum_type lhs, self_type rhs) BOOST_NOEXCEPT { return lhs>=enum_type(rhs.v_); } \
    };

#define BOOST_SCOPED_ENUM_DECLARE_END(EnumType) \
    ; \
    EnumType(enum_type v) BOOST_NOEXCEPT : v_(v) {}                 \
    BOOST_SCOPED_ENUM_DECLARE_END2()

/**
 * Starts a declaration of a scoped enum with the default int underlying type.
 *
 * @param EnumType The new scoped enum.
 */
#define BOOST_SCOPED_ENUM_DECLARE_BEGIN(EnumType) \
  BOOST_SCOPED_ENUM_UT_DECLARE_BEGIN(EnumType,int)

/**
 * Name of the native enum type.
 *
 * @param NT The new scoped enum.
 */
#define BOOST_SCOPED_ENUM_NATIVE(EnumType) EnumType::enum_type
/**
 * Forward declares an scoped enum.
 *
 * @param NT The scoped enum.
 */
#define BOOST_SCOPED_ENUM_FORWARD_DECLARE(EnumType) struct EnumType

#else  // BOOST_NO_CXX11_SCOPED_ENUMS

#define BOOST_SCOPED_ENUM_UT_DECLARE_BEGIN(EnumType,UnderlyingType) enum class EnumType:UnderlyingType
#define BOOST_SCOPED_ENUM_DECLARE_BEGIN(EnumType) enum class EnumType
#define BOOST_SCOPED_ENUM_DECLARE_END2()
#define BOOST_SCOPED_ENUM_DECLARE_END(EnumType) ;

#define BOOST_SCOPED_ENUM_NATIVE(EnumType) EnumType
#define BOOST_SCOPED_ENUM_FORWARD_DECLARE(EnumType) enum class EnumType

#endif  // BOOST_NO_CXX11_SCOPED_ENUMS

#define BOOST_SCOPED_ENUM_START(name) BOOST_SCOPED_ENUM_DECLARE_BEGIN(name)
#define BOOST_SCOPED_ENUM_END BOOST_SCOPED_ENUM_DECLARE_END2()
#define BOOST_SCOPED_ENUM(name) BOOST_SCOPED_ENUM_NATIVE(name)

//#ifdef BOOST_NO_CXX11_SCOPED_ENUMS
//
//# define BOOST_SCOPED_ENUM_START(name) struct name { enum enum_type
//# define BOOST_SCOPED_ENUM_END };
//# define BOOST_SCOPED_ENUM(name) name::enum_type
//
//#else
//
//# define BOOST_SCOPED_ENUM_START(name) enum class name
//# define BOOST_SCOPED_ENUM_END
//# define BOOST_SCOPED_ENUM(name) name
//
//#endif
#endif  // BOOST_SCOPED_ENUM_EMULATION_HPP
