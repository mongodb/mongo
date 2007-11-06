// Copyright (C) 2004 Arkadiy Vertleyb
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_TYPEOF_INT_ENCODING_HPP_INCLUDED
#define BOOST_TYPEOF_INT_ENCODING_HPP_INCLUDED

#include <boost/mpl/if.hpp>
#include <boost/mpl/size_t.hpp>

namespace boost { namespace type_of {

    template<class T> struct get_unsigned
    {
        typedef T type;
    };
    template<> struct get_unsigned<signed char>
    {
        typedef unsigned char type;
    };
    template<> struct get_unsigned<char>
    {
        typedef unsigned char type;
    };
    template<> struct get_unsigned<short>
    {
        typedef unsigned short type;
    };
    template<> struct get_unsigned<int>
    {
        typedef unsigned int type;
    };
    template<> struct get_unsigned<long>
    {
        typedef unsigned long type;
    };

    //////////////////////////

    template<std::size_t n, bool Overflow> 
    struct pack
    {
        enum {value = (n + 1) * 2 + (Overflow ? 1 : 0)};
    };

    template<std::size_t m> 
    struct unpack
    {
        enum {value = (m / 2) - 1};
        enum {overflow = (m % 2 == 1)};
    };

    ////////////////////////////////

    template<class V, std::size_t n, bool overflow = (n >= 0x3fffffff)>
    struct encode_size_t : push_back<
        V, 
        boost::mpl::size_t<pack<n, false>::value> 
    >
    {};

    template<class V, std::size_t n>
    struct encode_size_t<V, n, true> : push_back<typename push_back<
        V,
        boost::mpl::size_t<pack<n % 0x3ffffffe, true>::value> >::type,
        boost::mpl::size_t<n / 0x3ffffffe>
    >
    {};

    template<class V, class T, T n>
    struct encode_integral : encode_size_t< V, (typename get_unsigned<T>::type)n,(((typename get_unsigned<T>::type)n)>=0x3fffffff) > 
    {};

    template<class V, bool b>
    struct encode_integral<V, bool, b> : encode_size_t< V, b?1:0, false>
    {};
    ///////////////////////////

    template<std::size_t n, class Iter, bool overflow> 
    struct decode_size_t;

    template<std::size_t n, class Iter> 
    struct decode_size_t<n, Iter, false>
    {
        enum {value = n};
        typedef Iter iter;
    };

    template<std::size_t n, class Iter> 
    struct decode_size_t<n, Iter, true>
    {
        enum {m = Iter::type::value};

        enum {value = (std::size_t)m * 0x3ffffffe + n};
        typedef typename Iter::next iter;
    };

    template<class T, class Iter>
    struct decode_integral
    {
        enum {m = Iter::type::value};

        enum {n = unpack<m>::value};

        enum {overflow = unpack<m>::overflow};

        typedef typename Iter::next nextpos;
        
        static const T value = (T)(std::size_t)decode_size_t<n, nextpos, overflow>::value;

        typedef typename decode_size_t<n, nextpos, overflow>::iter iter;
    };

}}//namespace 

#endif//BOOST_TYPEOF_INT_ENCODING_HPP_INCLUDED
