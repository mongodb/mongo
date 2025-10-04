#ifndef BOOST_BIND_DETAIL_TUPLE_FOR_EACH_HPP_INCLUDED
#define BOOST_BIND_DETAIL_TUPLE_FOR_EACH_HPP_INCLUDED

//  Copyright 2015-2020, 2024 Peter Dimov.
//
//  Distributed under the Boost Software License, Version 1.0.
//
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include <boost/bind/detail/integer_sequence.hpp>
#include <boost/config.hpp>
#include <utility>
#include <type_traits>
#include <cstddef>

#if defined(BOOST_MSVC)
# pragma warning( push )
# pragma warning( disable: 4100 ) // unreferenced formal parameter 'tp'
#endif

namespace boost
{
namespace _bi
{

// tuple_for_each( f, tp )

template<class F, class Tp, std::size_t... J> F tuple_for_each_impl( F&& f, Tp&& tp, integer_sequence<std::size_t, J...> )
{
    using A = int[ 1 + sizeof...(J) ];
    using std::get;
    return (void)A{ 0, ((void)f(get<J>(std::forward<Tp>(tp))), 0)... }, std::forward<F>(f);
}

template<class F, class Tp> F tuple_for_each( F&& f, Tp&& tp )
{
    using seq = make_index_sequence<std::tuple_size<typename std::remove_reference<Tp>::type>::value>;
    return _bi::tuple_for_each_impl( std::forward<F>(f), std::forward<Tp>(tp), seq() );
}

// tuple_for_each( f, tp1, tp2 )

template<class F, class Tp1, class Tp2, std::size_t... J> F tuple_for_each_impl( F&& f, Tp1&& tp1, Tp2&& tp2, integer_sequence<std::size_t, J...> )
{
    using A = int[ 1 + sizeof...(J) ];
    using std::get;
    return (void)A{ 0, ((void)f( get<J>(std::forward<Tp1>(tp1)), get<J>(std::forward<Tp2>(tp2)) ), 0)... }, std::forward<F>(f);
}

template<class F, class Tp1, class Tp2> F tuple_for_each( F&& f, Tp1&& tp1, Tp2&& tp2 )
{
    using seq = make_index_sequence<std::tuple_size<typename std::remove_reference<Tp1>::type>::value>;
    return _bi::tuple_for_each_impl( std::forward<F>(f), std::forward<Tp1>(tp1), std::forward<Tp2>(tp2), seq() );
}

} // namespace _bi
} // namespace boost

#if defined(BOOST_MSVC)
# pragma warning( pop )
#endif

#endif // #ifndef BOOST_BIND_DETAIL_TUPLE_FOR_EACH_HPP_INCLUDED
