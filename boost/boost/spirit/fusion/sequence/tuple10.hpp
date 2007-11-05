/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_TUPLE10_HPP)
#define FUSION_SEQUENCE_TUPLE10_HPP

#include <boost/spirit/fusion/detail/config.hpp>
#include <boost/spirit/fusion/sequence/detail/tuple10.hpp>
#include <boost/spirit/fusion/sequence/detail/sequence_base.hpp>
#include <boost/spirit/fusion/detail/access.hpp>
#include <utility> // for std::pair
#include <boost/mpl/int.hpp>
#include <boost/mpl/vector/vector10.hpp>
#include <boost/mpl/if.hpp>
#include <boost/utility/addressof.hpp>

namespace boost { namespace fusion
{
    namespace meta
    {
        template <typename Iterator>
        struct next;
    }

    struct tuple_tag;

    struct tuple0 : sequence_base<tuple0>
    {
        typedef mpl::void_ types;
        typedef tuple_tag tag;
        typedef mpl::int_<0> size;
        typedef tuple0 identity_type;

        tuple0() {}

        template <typename Iterator>
        tuple0(Iterator const& i) {}
    };

    template <typename T0>
    struct tuple1 : sequence_base<tuple1<T0> >
    {
        typedef mpl::vector1<T0> types;
        typedef tuple_tag tag;
        typedef mpl::int_<1> size;
        typedef tuple1 identity_type;

        tuple1()
            : m0(T0())
        {}

        template <typename X>
        explicit tuple1(X const& x)
            : m0(construct(x, detail::disambiguate<X, T0>::call()))
        {}

        tuple1(typename detail::call_param<T0>::type _0)
            : m0(_0)
        {}

        template <typename U0>
        tuple1& operator=(tuple1<U0> const& t)
        {
            m0 = t.m0;
            return *this;
        }

        tuple1& operator=(tuple1 const& t)
        {
            m0 = t.m0;
            return *this;
        }

        T0 m0;

    private:

        template <typename Iterator>
        static T0
        construct(Iterator const& i, detail::disambiguate_as_iterator)
        {
            return *i;
        }

        template <typename Tuple>
        static T0
        construct(Tuple const& t, detail::disambiguate_as_tuple)
        {
            return t.m0;
        }

        template <typename X>
        static T0
        construct(X const& v, detail::disambiguate_as_data)
        {
            return v;
        }
    };

    template <typename T0, typename T1>
    struct tuple2;

    template <typename T0, typename T1>
    struct tuple_data2 : sequence_base<tuple2<T0, T1> >
    {
        typedef mpl::vector2<T0, T1> types;
        typedef tuple_tag tag;
        typedef mpl::int_<2> size;
        typedef tuple_data2 identity_type;

        tuple_data2()
            : m0(T0())
            , m1(T1())
        {}

        tuple_data2(
            typename detail::call_param<T0>::type _0
          , typename detail::call_param<T1>::type _1
        )
            : m0(_0)
            , m1(_1)
        {}

        template <typename A0, typename A1>
        tuple_data2(detail::disambiguate_as_iterator, A0& _0, A1& _1)
            : m0(*_0)
            , m1(*_1)
        {}

        T0 m0;
        T1 m1;
    };

    template <typename T0, typename T1, typename T2>
    struct tuple3;

    template <typename T0, typename T1, typename T2>
    struct tuple_data3 : sequence_base<tuple3<T0, T1, T2> >
    {
        typedef mpl::vector3<T0, T1, T2> types;
        typedef tuple_tag tag;
        typedef mpl::int_<3> size;
        typedef tuple_data3 identity_type;

        tuple_data3()
            : m0(T0())
            , m1(T1())
            , m2(T2())
        {}

        tuple_data3(
            typename detail::call_param<T0>::type _0
          , typename detail::call_param<T1>::type _1
          , typename detail::call_param<T2>::type _2
        )
            : m0(_0)
            , m1(_1)
            , m2(_2)
        {}

        template <typename A0, typename A1, typename A2>
        tuple_data3(detail::disambiguate_as_iterator, A0& _0, A1& _1, A2& _2)
            : m0(*_0)
            , m1(*_1)
            , m2(*_2)
        {}
        
        T0 m0;
        T1 m1;
        T2 m2;
    };

    template <typename T0, typename T1>
    struct tuple2 : tuple_data2<T0, T1>
    {
        tuple2()
            : tuple_data2<T0, T1>()
        {}

        tuple2(
            typename detail::call_param<T0>::type _0
          , typename detail::call_param<T1>::type _1
        )
            : tuple_data2<T0, T1>(_0, _1)
        {}

        template <typename X>
        explicit tuple2(X const& x)
            : tuple_data2<T0, T1>(construct(x, addressof(x)))
        {}

        template <typename U0, typename U1>
        tuple2& operator=(tuple2<U0, U1> const& t)
        {
            this->m0 = t.m0;
            this->m1 = t.m1;
            return *this;
        }

        template <typename First, typename Second>
        tuple2& operator=(std::pair<First, Second> const& p)
        {
            this->m0 = p.first;
            this->m1 = p.second;
            return *this;
        }

    private:

        template <typename Iterator>
        static tuple_data2<T0, T1>
        construct(Iterator const& i, void const*)
        {
            typedef typename meta::next<Iterator>::type i1_type;
            i1_type i1(fusion::next(i));
            return tuple_data2<T0, T1>(detail::disambiguate_as_iterator(), i, i1);
        }

        template <typename Tuple>
        static tuple_data2<T0, T1>
        construct(Tuple const& t, sequence_root const*)
        {
            return tuple_data2<T0, T1>(t.m0, t.m1);
        }

        template <typename U0, typename U1>
        static tuple_data2<T0, T1>
        construct(std::pair<U0, U1> const& p, std::pair<U0, U1> const*)
        {
            return tuple_data2<T0, T1>(p.first, p.second);
        }
    };

#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
    namespace detail
    {
        template <typename Iterator>
        struct next_iter3
        {
            typedef typename meta::next<Iterator>::type i1_type;
            typedef typename meta::next<i1_type>::type i2_type;
        };
    }
#endif

    template <typename T0, typename T1, typename T2>
    struct tuple3 : tuple_data3<T0, T1, T2>
    {
        tuple3()
            : tuple_data3<T0, T1, T2>()
        {}

        tuple3(
            typename detail::call_param<T0>::type _0
          , typename detail::call_param<T1>::type _1
          , typename detail::call_param<T2>::type _2
        )
            : tuple_data3<T0, T1, T2>(_0, _1, _2)
        {}

        template <typename X>
        explicit tuple3(X const& x)
            : tuple_data3<T0, T1, T2>(construct(x, &x))
        {}

        template <typename U0, typename U1, typename U2>
        tuple3& operator=(tuple3<U0, U1, U2> const& t)
        {
            this->m0 = t.m0;
            this->m1 = t.m1;
            this->m2 = t.m2;
            return *this;
        }

    private:

        template <typename Iterator>
        static tuple_data3<T0, T1, T2>
        construct(Iterator const& i, void const*)
        {
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
            typedef detail::next_iter3<Iterator> next_iter;
            next_iter::i1_type i1(fusion::next(i));
            next_iter::i2_type i2(fusion::next(i1));
#else
            typedef typename meta::next<Iterator>::type i1_type;
            typedef typename meta::next<i1_type>::type i2_type;
            i1_type i1(fusion::next(i));
            i2_type i2(fusion::next(i1));
#endif
            return tuple_data3<T0, T1, T2>(detail::disambiguate_as_iterator(), i, i1, i2);
        }

        template <typename Tuple>
        static tuple_data3<T0, T1, T2>
        construct(Tuple const& t, sequence_root const*)
        {
            return tuple_data3<T0, T1, T2>(t.m0, t.m1, t.m2);
        }
    };
}}

#endif
