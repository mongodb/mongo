// Copyright David Abrahams 2003. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// !!No include guards, intentionally!!

#define N BOOST_PP_ITERATION()

template <BOOST_PP_ENUM_PARAMS(N, typename T)>
struct BOOST_PP_CAT(tuple, N);

template <BOOST_PP_ENUM_PARAMS(N, typename T)>
struct BOOST_PP_CAT(tuple_data, N)
    : sequence_base<BOOST_PP_CAT(tuple, N)
        <BOOST_PP_ENUM_PARAMS(N, T)> >
{
    typedef mpl::BOOST_PP_CAT(vector, N)<BOOST_PP_ENUM_PARAMS(N, T)> types;
    typedef tuple_tag tag;
    typedef mpl::int_<N> size;
    typedef BOOST_PP_CAT(tuple_data, N) identity_type;

    BOOST_PP_CAT(tuple_data, N)()
        : BOOST_PP_ENUM(N, FUSION_TUPLE_MEMBER_DEFAULT_INIT, _) {}

    BOOST_PP_CAT(tuple_data, N)(BOOST_PP_ENUM_BINARY_PARAMS(
        N, typename detail::call_param<T, >::type _))
        : BOOST_PP_ENUM(N, FUSION_TUPLE_MEMBER_INIT, _) {}

    template <BOOST_PP_ENUM_PARAMS(N, typename A)>
    BOOST_PP_CAT(tuple_data, N)(detail::disambiguate_as_iterator,
        BOOST_PP_ENUM_BINARY_PARAMS(N, A, & _))
        : BOOST_PP_ENUM(N, FUSION_TUPLE_MEMBER_ITERATOR_INIT, _) {}

    BOOST_PP_REPEAT(N, FUSION_TUPLE_MEMBER, _)
};

template <BOOST_PP_ENUM_PARAMS(N, typename T)>
struct BOOST_PP_CAT(tuple, N)
    : BOOST_PP_CAT(tuple_data, N)<BOOST_PP_ENUM_PARAMS(N, T)>
{
    typedef BOOST_PP_CAT(tuple_data, N)<
        BOOST_PP_ENUM_PARAMS(N, T)> base_type;

    BOOST_PP_CAT(tuple, N)()
        : base_type()
    {}

    BOOST_PP_CAT(tuple, N)(BOOST_PP_ENUM_BINARY_PARAMS(
        N, typename detail::call_param<T, >::type _))
        : base_type(BOOST_PP_ENUM_PARAMS(N, _))
    {}

    template <typename X>
    explicit BOOST_PP_CAT(tuple, N)(X const& x)
        : base_type(construct(x, &x))
    {}

    template <BOOST_PP_ENUM_PARAMS(N, typename U)>
    BOOST_PP_CAT(tuple, N)&
    operator=(BOOST_PP_CAT(tuple, N)<BOOST_PP_ENUM_PARAMS(N, U)> const& t)
    {
        BOOST_PP_REPEAT(N, FUSION_TUPLE_MEMBER_ASSIGN, _)
        return *this;
    }

private:

    template <typename i0_type>
    static base_type
    construct(i0_type const& i0, void const*)
    {
        FUSION_TUPLE_CONSTRUCT_FROM_ITER(N)
        return base_type(
            detail::disambiguate_as_iterator(), BOOST_PP_ENUM_PARAMS(N, i));
    }

    template <typename Tuple>
    static base_type
    construct(Tuple const& t, sequence_root const*)
    {
        return base_type(BOOST_PP_ENUM_PARAMS(N, t.m));
    }
};

#undef N

