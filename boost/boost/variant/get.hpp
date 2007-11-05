//-----------------------------------------------------------------------------
// boost variant/get.hpp header file
// See http://www.boost.org for updates, documentation, and revision history.
//-----------------------------------------------------------------------------
//
// Copyright (c) 2003
// Eric Friedman, Itay Maman
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_VARIANT_GET_HPP
#define BOOST_VARIANT_GET_HPP

#include <exception>

#include "boost/config.hpp"
#include "boost/detail/workaround.hpp"
#include "boost/utility/addressof.hpp"
#include "boost/variant/variant_fwd.hpp"

#include "boost/type_traits/add_reference.hpp"
#include "boost/type_traits/add_pointer.hpp"

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
#   include "boost/mpl/bool.hpp"
#   include "boost/mpl/or.hpp"
#   include "boost/type_traits/is_same.hpp"
#endif

namespace boost {

//////////////////////////////////////////////////////////////////////////
// class bad_get
//
// The exception thrown in the event of a failed get of a value.
//
class bad_get
    : public std::exception
{
public: // std::exception implementation

    virtual const char * what() const throw()
    {
        return "boost::bad_get: "
               "failed value get using boost::get";
    }

};

//////////////////////////////////////////////////////////////////////////
// function template get<T>
//
// Retrieves content of given variant object if content is of type T.
// Otherwise: pointer ver. returns 0; reference ver. throws bad_get.
//

namespace detail { namespace variant {

// (detail) class template get_visitor
//
// Generic static visitor that: if the value is of the specified type,
// returns a pointer to the value it visits; else a null pointer.
//
template <typename T>
struct get_visitor
{
private: // private typedefs

    typedef typename add_pointer<T>::type pointer;
    typedef typename add_reference<T>::type reference;

public: // visitor typedefs

    typedef pointer result_type;

public: // visitor interfaces

#if !BOOST_WORKAROUND(BOOST_MSVC, < 1300)

    pointer operator()(reference operand) const
    {
        return boost::addressof(operand);
    }

    template <typename U>
    pointer operator()(const U&) const
    {
        return static_cast<pointer>(0);
    }

#else // MSVC6

private: // helpers, for visitor interfaces (below)

    pointer execute_impl(reference operand, mpl::true_) const
    {
        return boost::addressof(operand);
    }

    template <typename U>
    pointer execute_impl(const U& operand, mpl::false_) const
    {
        return static_cast<pointer>(0);
    }

public: // visitor interfaces

    template <typename U>
    pointer operator()(U& operand) const
    {
        // MSVC6 finds normal implementation (above) ambiguous,
        // so we must explicitly disambiguate

        typedef typename mpl::or_<
              is_same<U, T>
            , is_same<const U, T>
            >::type U_is_T;

        return execute_impl(operand, U_is_T());
    }

#endif // MSVC6 workaround

};

}} // namespace detail::variant

#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x0551))
#   define BOOST_VARIANT_AUX_GET_EXPLICIT_TEMPLATE_TYPE(t)  \
    BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(t)
#else
#   define BOOST_VARIANT_AUX_GET_EXPLICIT_TEMPLATE_TYPE(t)  \
    , t* = 0
#endif

template <typename U, BOOST_VARIANT_ENUM_PARAMS(typename T) >
inline
    typename add_pointer<U>::type
get(
      boost::variant< BOOST_VARIANT_ENUM_PARAMS(T) >* operand
      BOOST_VARIANT_AUX_GET_EXPLICIT_TEMPLATE_TYPE(U)
    )
{
    typedef typename add_pointer<U>::type U_ptr;
    if (!operand) return static_cast<U_ptr>(0);

    detail::variant::get_visitor<U> v;
    return operand->apply_visitor(v);
}

template <typename U, BOOST_VARIANT_ENUM_PARAMS(typename T) >
inline
    typename add_pointer<const U>::type
get(
      const boost::variant< BOOST_VARIANT_ENUM_PARAMS(T) >* operand
      BOOST_VARIANT_AUX_GET_EXPLICIT_TEMPLATE_TYPE(U)
    )
{
    typedef typename add_pointer<const U>::type U_ptr;
    if (!operand) return static_cast<U_ptr>(0);

    detail::variant::get_visitor<const U> v;
    return operand->apply_visitor(v);
}

template <typename U, BOOST_VARIANT_ENUM_PARAMS(typename T) >
inline
    typename add_reference<U>::type
get(
      boost::variant< BOOST_VARIANT_ENUM_PARAMS(T) >& operand
      BOOST_VARIANT_AUX_GET_EXPLICIT_TEMPLATE_TYPE(U)
    )
{
    typedef typename add_pointer<U>::type U_ptr;
    U_ptr result = get<U>(&operand);

    if (!result)
        throw bad_get();
    return *result;
}

template <typename U, BOOST_VARIANT_ENUM_PARAMS(typename T) >
inline
    typename add_reference<const U>::type
get(
      const boost::variant< BOOST_VARIANT_ENUM_PARAMS(T) >& operand
      BOOST_VARIANT_AUX_GET_EXPLICIT_TEMPLATE_TYPE(U)
    )
{
    typedef typename add_pointer<const U>::type U_ptr;
    U_ptr result = get<const U>(&operand);

    if (!result)
        throw bad_get();
    return *result;
}

} // namespace boost

#endif // BOOST_VARIANT_GET_HPP
