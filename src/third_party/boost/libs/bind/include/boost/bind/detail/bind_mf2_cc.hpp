//
//  bind/bind_mf2_cc.hpp - member functions, type<> syntax
//
//  Do not include this header directly.
//
//  Copyright (c) 2001 Peter Dimov and Multi Media Ltd.
//  Copyright (c) 2008 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt
//
//  See http://www.boost.org/libs/bind/bind.html for documentation.
//

// 0

template<class Rt2, class R, class T,
    class A1>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) () BOOST_BIND_MF_NOEXCEPT, A1 a1)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1 );
}

template<class Rt2, class R, class T,
    class A1>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) () const BOOST_BIND_MF_NOEXCEPT, A1 a1)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1 );
}

// 1

template<class Rt2, class R, class T,
    class B1,
    class A1, class A2>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2 );
}

template<class Rt2, class R, class T,
    class B1,
    class A1, class A2>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2 );
}

// 2

template<class Rt2, class R, class T,
    class B1, class B2,
    class A1, class A2, class A3>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3 );
}

template<class Rt2, class R, class T,
    class B1, class B2,
    class A1, class A2, class A3>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3 );
}

// 3

template<class Rt2, class R, class T,
    class B1, class B2, class B3,
    class A1, class A2, class A3, class A4>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4 );
}

template<class Rt2, class R, class T,
    class B1, class B2, class B3,
    class A1, class A2, class A3, class A4>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4 );
}

// 4

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4,
    class A1, class A2, class A3, class A4, class A5>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5 );
}

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4,
    class A1, class A2, class A3, class A4, class A5>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5 );
}

// 5

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5,
    class A1, class A2, class A3, class A4, class A5, class A6>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6 );
}

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5,
    class A1, class A2, class A3, class A4, class A5, class A6>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6 );
}

// 6

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5, class B6,
    class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5, B6) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7 );
}

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5, class B6,
    class A1, class A2, class A3, class A4, class A5, class A6, class A7>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5, B6) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7 );
}

// 7

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5, class B6, class B7,
    class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5, B6, B7) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8 );
}

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5, class B6, class B7,
    class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5, B6, B7) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8 );
}

// 8

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5, class B6, class B7, class B8,
    class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5, B6, B7, B8) BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8, a9 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8, a9 );
}

template<class Rt2, class R, class T,
    class B1, class B2, class B3, class B4, class B5, class B6, class B7, class B8,
    class A1, class A2, class A3, class A4, class A5, class A6, class A7, class A8, class A9>
    auto
    BOOST_BIND(boost::type<Rt2>, R (BOOST_BIND_MF_CC T::*f) (B1, B2, B3, B4, B5, B6, B7, B8) const BOOST_BIND_MF_NOEXCEPT, A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9)
    -> decltype( boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8, a9 ) )
{
    return boost::BOOST_BIND( boost::type<Rt2>(), boost::mem_fn( f ), a1, a2, a3, a4, a5, a6, a7, a8, a9 );
}
