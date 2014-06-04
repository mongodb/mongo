/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

    This is an auto-generated file. Do not edit!
==============================================================================*/
namespace boost { namespace fusion
{
    template <typename T0 , typename T1 , typename T2 , typename T3 , typename T4 , typename T5 , typename T6 , typename T7 , typename T8 , typename T9>
    struct tuple : vector<T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9>
    {
        typedef vector<
            T0 , T1 , T2 , T3 , T4 , T5 , T6 , T7 , T8 , T9>
        base_type;
        tuple()
            : base_type() {}
        tuple(tuple const& rhs)
            : base_type(rhs) {}
        template <typename U1, typename U2>
        tuple(std::pair<U1, U2> const& rhs)
            : base_type(rhs) {}
    explicit
    tuple(typename detail::call_param<T0 >::type _0)
        : base_type(_0) {}
    template <typename U0>
    tuple(tuple<U0> const& rhs)
        : base_type(rhs) {}
    template <typename U0>
    tuple& operator=(tuple<U0> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1)
        : base_type(_0 , _1) {}
    template <typename U0 , typename U1>
    tuple(tuple<U0 , U1> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1>
    tuple& operator=(tuple<U0 , U1> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2)
        : base_type(_0 , _1 , _2) {}
    template <typename U0 , typename U1 , typename U2>
    tuple(tuple<U0 , U1 , U2> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2>
    tuple& operator=(tuple<U0 , U1 , U2> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3)
        : base_type(_0 , _1 , _2 , _3) {}
    template <typename U0 , typename U1 , typename U2 , typename U3>
    tuple(tuple<U0 , U1 , U2 , U3> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3>
    tuple& operator=(tuple<U0 , U1 , U2 , U3> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4)
        : base_type(_0 , _1 , _2 , _3 , _4) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4>
    tuple(tuple<U0 , U1 , U2 , U3 , U4> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4>
    tuple& operator=(tuple<U0 , U1 , U2 , U3 , U4> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5)
        : base_type(_0 , _1 , _2 , _3 , _4 , _5) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5>
    tuple(tuple<U0 , U1 , U2 , U3 , U4 , U5> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5>
    tuple& operator=(tuple<U0 , U1 , U2 , U3 , U4 , U5> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6)
        : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6>
    tuple(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6>
    tuple& operator=(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7)
        : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6 , _7) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7>
    tuple(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7>
    tuple& operator=(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7 , typename detail::call_param<T8 >::type _8)
        : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6 , _7 , _8) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8>
    tuple(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8>
    tuple& operator=(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
    tuple(typename detail::call_param<T0 >::type _0 , typename detail::call_param<T1 >::type _1 , typename detail::call_param<T2 >::type _2 , typename detail::call_param<T3 >::type _3 , typename detail::call_param<T4 >::type _4 , typename detail::call_param<T5 >::type _5 , typename detail::call_param<T6 >::type _6 , typename detail::call_param<T7 >::type _7 , typename detail::call_param<T8 >::type _8 , typename detail::call_param<T9 >::type _9)
        : base_type(_0 , _1 , _2 , _3 , _4 , _5 , _6 , _7 , _8 , _9) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8 , typename U9>
    tuple(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8 , U9> const& rhs)
        : base_type(rhs) {}
    template <typename U0 , typename U1 , typename U2 , typename U3 , typename U4 , typename U5 , typename U6 , typename U7 , typename U8 , typename U9>
    tuple& operator=(tuple<U0 , U1 , U2 , U3 , U4 , U5 , U6 , U7 , U8 , U9> const& rhs)
    {
        base_type::operator=(rhs);
        return *this;
    }
        template <typename T>
        tuple& operator=(T const& rhs)
        {
            base_type::operator=(rhs);
            return *this;
        }
        tuple& operator=(tuple const& rhs)
        {
            base_type::operator=(rhs);
            return *this;
        }
        template <typename U1, typename U2>
        tuple& operator=(std::pair<U1, U2> const& rhs)
        {
            base_type::operator=(rhs);
            return *this;
        }
    };
    template <typename Tuple>
    struct tuple_size : result_of::size<Tuple> {};
    template <int N, typename Tuple>
    struct tuple_element : result_of::value_at_c<Tuple, N> {};
    template <int N, typename Tuple>
    inline typename
        lazy_disable_if<
            is_const<Tuple>
          , result_of::at_c<Tuple, N>
        >::type
    get(Tuple& tup)
    {
        return at_c<N>(tup);
    }
    template <int N, typename Tuple>
    inline typename result_of::at_c<Tuple const, N>::type
    get(Tuple const& tup)
    {
        return at_c<N>(tup);
    }
}}
