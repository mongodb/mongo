#ifndef BOOST_DESCRIBE_OPERATORS_HPP_INCLUDED
#define BOOST_DESCRIBE_OPERATORS_HPP_INCLUDED

// Copyright 2020, 2021 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/describe/detail/config.hpp>

#if defined(BOOST_DESCRIBE_CXX14)

#include <boost/describe/bases.hpp>
#include <boost/describe/members.hpp>
#include <boost/describe/modifiers.hpp>
#include <boost/mp11/algorithm.hpp>
#include <type_traits>
#include <iosfwd>

#if defined(_MSC_VER) && _MSC_VER == 1900
# pragma warning(push)
# pragma warning(disable: 4100) // unreferenced formal parameter
#endif

namespace boost
{
namespace describe
{

namespace detail
{

template<class T,
    class Bd = describe::describe_bases<T, mod_any_access>,
    class Md = describe::describe_members<T, mod_any_access>>
bool eq( T const& t1, T const& t2 )
{
    bool r = true;

    mp11::mp_for_each<Bd>([&](auto D){

        using B = typename decltype(D)::type;
        r = r && (B const&)t1 == (B const&)t2;

    });

    mp11::mp_for_each<Md>([&](auto D){

        r = r && t1.*D.pointer == t2.*D.pointer;

    });

    return r;
}

template<class T,
    class Bd = describe::describe_bases<T, mod_any_access>,
    class Md = describe::describe_members<T, mod_any_access>>
bool lt( T const& t1, T const& t2 )
{
    int r = 0;

    mp11::mp_for_each<Bd>([&](auto D){

        using B = typename decltype(D)::type;
        if( r == 0 && (B const&)t1 < (B const&)t2 ) r = -1;
        if( r == 0 && (B const&)t2 < (B const&)t1 ) r = +1;

    });

    mp11::mp_for_each<Md>([&](auto D){

        if( r == 0 && t1.*D.pointer < t2.*D.pointer ) r = -1;
        if( r == 0 && t2.*D.pointer < t1.*D.pointer ) r = +1;

    });

    return r < 0;
}

template<class Os, class T,
    class Bd = describe::describe_bases<T, mod_any_access>,
    class Md = describe::describe_members<T, mod_any_access>>
void print( Os& os, T const& t )
{
    os << "{";

    bool first = true;

    mp11::mp_for_each<Bd>([&](auto D){

        if( !first ) { os << ", "; }
        first = false;

        using B = typename decltype(D)::type;
        os << (B const&)t;

    });

    mp11::mp_for_each<Md>([&](auto D){

        if( !first ) { os << ", "; }
        first = false;

        os << "." << D.name << " = " << t.*D.pointer;

    });

    os << "}";
}

} // namespace detail

namespace operators
{

template<class T> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value, bool>
    operator==( T const& t1, T const& t2 )
{
    return detail::eq( t1, t2 );
}

template<class T> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value, bool>
    operator!=( T const& t1, T const& t2 )
{
    return !detail::eq( t1, t2 );
}

template<class T> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value, bool>
    operator<( T const& t1, T const& t2 )
{
    return detail::lt( t1, t2 );
}

template<class T> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value, bool>
    operator>=( T const& t1, T const& t2 )
{
    return !detail::lt( t1, t2 );
}

template<class T> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value, bool>
    operator>( T const& t1, T const& t2 )
{
    return detail::lt( t2, t1 );
}

template<class T> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value, bool>
    operator<=( T const& t1, T const& t2 )
{
    return !detail::lt( t2, t1 );
}

template<class T, class Ch, class Tr> std::enable_if_t<
    has_describe_bases<T>::value && has_describe_members<T>::value && !std::is_union<T>::value,
    std::basic_ostream<Ch, Tr>&>
    operator<<( std::basic_ostream<Ch, Tr>& os, T const& t )
{
    os.width( 0 );
    detail::print( os, t );
    return os;
}

} // namespace operators

} // namespace describe
} // namespace boost

#if defined(_MSC_VER) && _MSC_VER == 1900
# pragma warning(pop)
#endif

#endif // defined(BOOST_DESCRIBE_CXX14)

#endif // #ifndef BOOST_DESCRIBE_OPERATORS_HPP_INCLUDED
