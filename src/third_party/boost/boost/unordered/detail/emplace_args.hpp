
// Copyright (C) 2011 Daniel James.
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/libs/unordered for documentation

#ifndef BOOST_UNORDERED_EMPLACE_ARGS_HPP
#define BOOST_UNORDERED_EMPLACE_ARGS_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

#include <boost/move/move.hpp>
#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/inc.hpp>
#include <boost/preprocessor/dec.hpp>
#include <boost/preprocessor/repetition/enum.hpp>
#include <boost/preprocessor/repetition/enum_params.hpp>
#include <boost/preprocessor/repetition/enum_binary_params.hpp>
#include <boost/preprocessor/repetition/repeat_from_to.hpp>
#include <boost/type_traits/is_class.hpp>
#include <boost/tuple/tuple.hpp>
#include <utility>

#if !defined(BOOST_NO_0X_HDR_TUPLE)
#include <tuple>
#endif

#if defined(BOOST_MSVC)
#pragma warning(push)
#pragma warning(disable:4512) // assignment operator could not be generated.
#pragma warning(disable:4345) // behavior change: an object of POD type
                              // constructed with an initializer of the form ()
                              // will be default-initialized.
#endif

#define BOOST_UNORDERED_EMPLACE_LIMIT 10

#if !defined(BOOST_NO_RVALUE_REFERENCES) && \
        !defined(BOOST_NO_VARIADIC_TEMPLATES)
#define BOOST_UNORDERED_VARIADIC_MOVE
#endif

namespace boost { namespace unordered { namespace detail {

    ////////////////////////////////////////////////////////////////////////////
    // emplace_args
    //
    // Either forwarding variadic arguments, or storing the arguments in
    // emplace_args##n

#if defined(BOOST_UNORDERED_VARIADIC_MOVE)

#define BOOST_UNORDERED_EMPLACE_TEMPLATE typename... Args
#define BOOST_UNORDERED_EMPLACE_ARGS Args&&... args
#define BOOST_UNORDERED_EMPLACE_FORWARD boost::forward<Args>(args)...

#else

#define BOOST_UNORDERED_EMPLACE_TEMPLATE typename Args
#define BOOST_UNORDERED_EMPLACE_ARGS Args const& args
#define BOOST_UNORDERED_EMPLACE_FORWARD args

#define BOOST_UNORDERED_FWD_PARAM(z, n, a) \
    BOOST_FWD_REF(BOOST_PP_CAT(A, n)) BOOST_PP_CAT(a, n)

#define BOOST_UNORDERED_CALL_FORWARD(z, i, a) \
    boost::forward<BOOST_PP_CAT(A,i)>(BOOST_PP_CAT(a,i))

#define BOOST_UNORDERED_EARGS(z, n, _)                                      \
    template <BOOST_PP_ENUM_PARAMS_Z(z, n, typename A)>                     \
    struct BOOST_PP_CAT(emplace_args, n)                                    \
    {                                                                       \
        BOOST_PP_REPEAT_##z(n, BOOST_UNORDERED_EARGS_MEMBER, _)             \
        BOOST_PP_CAT(emplace_args, n) (                                     \
            BOOST_PP_ENUM_BINARY_PARAMS_Z(z, n, Arg, a)                     \
        ) : BOOST_PP_ENUM_##z(n, BOOST_UNORDERED_EARGS_INIT, _)             \
        {}                                                                  \
                                                                            \
    };                                                                      \
                                                                            \
    template <BOOST_PP_ENUM_PARAMS_Z(z, n, typename A)>                     \
    inline BOOST_PP_CAT(emplace_args, n) <                                  \
        BOOST_PP_ENUM_PARAMS_Z(z, n, A)                                     \
    > create_emplace_args(                                                  \
        BOOST_PP_ENUM_##z(n, BOOST_UNORDERED_FWD_PARAM, a)                  \
    )                                                                       \
    {                                                                       \
        BOOST_PP_CAT(emplace_args, n) <                                     \
            BOOST_PP_ENUM_PARAMS_Z(z, n, A)                                 \
        > e(BOOST_PP_ENUM_PARAMS_Z(z, n, a));                               \
        return e;                                                           \
    }

#if defined(BOOST_NO_RVALUE_REFERENCES)

#define BOOST_UNORDERED_EARGS_MEMBER(z, n, _)                               \
    typedef BOOST_FWD_REF(BOOST_PP_CAT(A, n)) BOOST_PP_CAT(Arg, n);         \
    BOOST_PP_CAT(Arg, n) BOOST_PP_CAT(a, n);

#define BOOST_UNORDERED_EARGS_INIT(z, n, _)                                 \
    BOOST_PP_CAT(a, n)(                                                     \
        boost::forward<BOOST_PP_CAT(A,n)>(BOOST_PP_CAT(a, n)))

#else

#define BOOST_UNORDERED_EARGS_MEMBER(z, n, _)                               \
    typedef typename boost::add_lvalue_reference<BOOST_PP_CAT(A, n)>::type  \
        BOOST_PP_CAT(Arg, n);                                               \
    BOOST_PP_CAT(Arg, n) BOOST_PP_CAT(a, n);

#define BOOST_UNORDERED_EARGS_INIT(z, n, _)                                 \
    BOOST_PP_CAT(a, n)(BOOST_PP_CAT(a, n))

#endif

BOOST_PP_REPEAT_FROM_TO(1, BOOST_UNORDERED_EMPLACE_LIMIT, BOOST_UNORDERED_EARGS,
    _)

#undef BOOST_UNORDERED_DEFINE_EMPLACE_ARGS
#undef BOOST_UNORDERED_EARGS_MEMBER
#undef BOOST_UNORDERED_EARGS_INIT

#endif

    ////////////////////////////////////////////////////////////////////////////
    // rvalue parameters when type can't be a BOOST_RV_REF(T) parameter
    // e.g. for int

#if !defined(BOOST_NO_RVALUE_REFERENCES)
#   define BOOST_UNORDERED_RV_REF(T) BOOST_RV_REF(T)
#else
    struct please_ignore_this_overload {
        typedef please_ignore_this_overload type;
    };

    template <typename T>
    struct rv_ref_impl {
        typedef BOOST_RV_REF(T) type;
    };

    template <typename T>
    struct rv_ref :
        boost::detail::if_true<
            boost::is_class<T>::value
        >::BOOST_NESTED_TEMPLATE then <
            boost::unordered::detail::rv_ref_impl<T>,
            please_ignore_this_overload
        >::type
    {};

#   define BOOST_UNORDERED_RV_REF(T) \
        typename boost::unordered::detail::rv_ref<T>::type
#endif

    ////////////////////////////////////////////////////////////////////////////
    // Construct from tuple
    //
    // Used for piecewise construction.

#if !BOOST_WORKAROUND(__SUNPRO_CC, <= 0x590)

#define BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE(n, namespace_)                 \
    template<typename T>                                                    \
    void construct_from_tuple(T* ptr, namespace_::tuple<>)                  \
    {                                                                       \
        new ((void*) ptr) T();                                              \
    }                                                                       \
                                                                            \
    BOOST_PP_REPEAT_FROM_TO(1, n,                                           \
        BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE_IMPL, namespace_)

#define BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE_IMPL(z, n, namespace_)         \
    template<typename T, BOOST_PP_ENUM_PARAMS_Z(z, n, typename A)>          \
    void construct_from_tuple(T* ptr,                                       \
            namespace_::tuple<BOOST_PP_ENUM_PARAMS_Z(z, n, A)> const& x)    \
    {                                                                       \
        new ((void*) ptr) T(                                                \
            BOOST_PP_ENUM_##z(n, BOOST_UNORDERED_GET_TUPLE_ARG, namespace_) \
        );                                                                  \
    }

#define BOOST_UNORDERED_GET_TUPLE_ARG(z, n, namespace_)                     \
    namespace_::get<n>(x)

BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE(10, boost)

#if !defined(BOOST_NO_0X_HDR_TUPLE)
BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE(10, std)
#endif

#undef BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE
#undef BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE_IMPL
#undef BOOST_UNORDERED_GET_TUPLE_ARG

#else

    template <int N> struct length {};

    template<typename T>
    void construct_from_tuple_impl(
            boost::unordered::detail::length<0>, T* ptr,
            boost::tuple<>)
    {
        new ((void*) ptr) T();
    }

#define BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE_IMPL(z, n, _)                  \
    template<typename T, BOOST_PP_ENUM_PARAMS_Z(z, n, typename A)>          \
    void construct_from_tuple_impl(                                         \
            boost::unordered::detail::length<n>, T* ptr,                    \
            namespace_::tuple<BOOST_PP_ENUM_PARAMS_Z(z, n, A)> const& x)    \
    {                                                                       \
        new ((void*) ptr) T(                                                \
            BOOST_PP_ENUM_##z(n, BOOST_UNORDERED_GET_TUPLE_ARG, namespace_) \
        );                                                                  \
    }

#define BOOST_UNORDERED_GET_TUPLE_ARG(z, n, _)                              \
    boost::get<n>(x)

    BOOST_PP_REPEAT_FROM_TO(1, 10,                                          \
        BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE_IMPL, _)

    template <typename T, typename Tuple>
    void construct_from_tuple(T* ptr, Tuple const& x)
    {
        construct_from_tuple_impl(
            boost::unordered::detail::length<
                boost::tuples::length<Tuple>::value>(),
            ptr, x);
    }

#undef BOOST_UNORDERED_CONSTRUCT_FROM_TUPLE_IMPL
#undef BOOST_UNORDERED_GET_TUPLE_ARG

#endif

    ////////////////////////////////////////////////////////////////////////////
    // SFINAE traits for construction.

    // Decide which construction method to use for a three argument
    // call. Note that this is difficult to do using overloads because
    // the arguments are packed into 'emplace_args3'.
    //
    // The decision is made on the first argument.


#if defined(BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT)
    template <typename A, typename B, typename A0>
    struct emulation1 {
        static choice1::type test(choice1, std::pair<A, B> const&);
        static choice2::type test(choice2, A const&);
        static choice3::type test(choice3, convert_from_anything const&);

        enum { value =
            sizeof(test(choose(), boost::unordered::detail::make<A0>())) ==
                sizeof(choice2::type) };
    };
#endif

    template <typename A, typename B, typename A0>
    struct check3_base {
        static choice1::type test(choice1,
            boost::unordered::piecewise_construct_t);

#if defined(BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT)
        static choice2::type test(choice2, A const&);
#endif

        static choice3::type test(choice3, ...);

        enum { value =
            sizeof(test(choose(), boost::unordered::detail::make<A0>())) };
    };

    template <typename A, typename B, typename A0>
    struct piecewise3 {
        enum { value = check3_base<A,B,A0>::value == sizeof(choice1::type) };
    };

#if defined(BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT)
    template <typename A, typename B, typename A0>
    struct emulation3 {
        enum { value = check3_base<A,B,A0>::value == sizeof(choice2::type) };
    };

#endif

#if defined(BOOST_UNORDERED_VARIADIC_MOVE)

    ////////////////////////////////////////////////////////////////////////////
    // Construct from variadic parameters

    template <typename T, typename... Args>
    inline void construct_impl(T* address, Args&&... args)
    {
        new((void*) address) T(boost::forward<Args>(args)...);
    }

    template <typename A, typename B, typename A0, typename A1, typename A2>
    inline typename enable_if<piecewise3<A, B, A0>, void>::type
        construct_impl(std::pair<A, B>* address, A0&&, A1&& a1, A2&& a2)
    {
        boost::unordered::detail::construct_from_tuple(
            boost::addressof(address->first), a1);
        boost::unordered::detail::construct_from_tuple(
            boost::addressof(address->second), a2);
    }

#if defined(BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT)

    template <typename A, typename B, typename A0>
    inline typename enable_if<emulation1<A, B, A0>, void>::type
        construct_impl(std::pair<A, B>* address, A0&& a0)
    {
        new((void*) boost::addressof(address->first)) A(boost::forward<A0>(a0));
        new((void*) boost::addressof(address->second)) B();
   }

    template <typename A, typename B, typename A0, typename A1, typename A2>
    inline typename enable_if<emulation3<A, B, A0>, void>::type
        construct_impl(std::pair<A, B>* address, A0&& a0, A1&& a1, A2&& a2)
    {
        new((void*) boost::addressof(address->first)) A(boost::forward<A0>(a0));
        new((void*) boost::addressof(address->second)) B(
            boost::forward<A1>(a1),
            boost::forward<A2>(a2));
    }

    template <typename A, typename B,
            typename A0, typename A1, typename A2, typename A3,
            typename... Args>
    inline void construct_impl(std::pair<A, B>* address,
            A0&& a0, A1&& a1, A2&& a2, A3&& a3, Args&&... args)
    {
        new((void*) boost::addressof(address->first)) A(boost::forward<A0>(a0));

        new((void*) boost::addressof(address->second)) B(
            boost::forward<A1>(a1),
            boost::forward<A2>(a2),
            boost::forward<A3>(a3),
            boost::forward<Args>(args)...);
    }

#endif // BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT
#else // BOOST_UNORDERED_VARIADIC_MOVE

////////////////////////////////////////////////////////////////////////////////
// Construct from emplace_args

#define BOOST_UNORDERED_CONSTRUCT_IMPL(z, num_params, _)                    \
    template <                                                              \
        typename T,                                                         \
        BOOST_PP_ENUM_PARAMS_Z(z, num_params, typename A)                   \
    >                                                                       \
    inline void construct_impl(T* address,                                  \
        boost::unordered::detail::BOOST_PP_CAT(emplace_args,num_params) <   \
            BOOST_PP_ENUM_PARAMS_Z(z, num_params, A)                        \
        > const& args)                                                      \
    {                                                                       \
        new((void*) address) T(                                             \
            BOOST_PP_ENUM_##z(num_params, BOOST_UNORDERED_CALL_FORWARD,     \
                args.a));                                                   \
    }

    template <typename T, typename A0>
    inline void construct_impl(T* address, emplace_args1<A0> const& args)
    {
        new((void*) address) T(boost::forward<A0>(args.a0));
    }

    template <typename T, typename A0, typename A1>
    inline void construct_impl(T* address, emplace_args2<A0, A1> const& args)
    {
        new((void*) address) T(
            boost::forward<A0>(args.a0),
            boost::forward<A1>(args.a1)
        );
    }

    template <typename T, typename A0, typename A1, typename A2>
    inline void construct_impl(T* address, emplace_args3<A0, A1, A2> const& args)
    {
        new((void*) address) T(
            boost::forward<A0>(args.a0),
            boost::forward<A1>(args.a1),
            boost::forward<A2>(args.a2)
        );
    }

    BOOST_PP_REPEAT_FROM_TO(4, BOOST_UNORDERED_EMPLACE_LIMIT,
        BOOST_UNORDERED_CONSTRUCT_IMPL, _)

#undef BOOST_UNORDERED_CONSTRUCT_IMPL

    template <typename A, typename B, typename A0, typename A1, typename A2>
    inline typename enable_if<piecewise3<A, B, A0>, void>::type
        construct_impl(std::pair<A, B>* address,
            boost::unordered::detail::emplace_args3<A0, A1, A2> const& args)
    {
        boost::unordered::detail::construct_from_tuple(
            boost::addressof(address->first), args.a1);
        boost::unordered::detail::construct_from_tuple(
            boost::addressof(address->second), args.a2);
    }

#if defined(BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT)

    template <typename A, typename B, typename A0>
    inline typename enable_if<emulation1<A, B, A0>, void>::type
        construct_impl(std::pair<A, B>* address,
            boost::unordered::detail::emplace_args1<A0> const& args)
    {
        new((void*) boost::addressof(address->first)) A(
            boost::forward<A0>(args.a0));
        new((void*) boost::addressof(address->second)) B();
    }

    template <typename A, typename B, typename A0, typename A1, typename A2>
    inline typename enable_if<emulation3<A, B, A0>, void>::type
        construct_impl(std::pair<A, B>* address,
            boost::unordered::detail::emplace_args3<A0, A1, A2> const& args)
    {
        new((void*) boost::addressof(address->first)) A(
            boost::forward<A0>(args.a0));
        new((void*) boost::addressof(address->second)) B(
            boost::forward<A1>(args.a1),
            boost::forward<A2>(args.a2));
    }

#define BOOST_UNORDERED_CONSTRUCT_PAIR_IMPL(z, num_params, _)               \
    template <typename A, typename B,                                       \
        BOOST_PP_ENUM_PARAMS_Z(z, num_params, typename A)                   \
    >                                                                       \
    inline void construct_impl(std::pair<A, B>* address,                    \
        boost::unordered::detail::BOOST_PP_CAT(emplace_args, num_params) <  \
                BOOST_PP_ENUM_PARAMS_Z(z, num_params, A)                    \
            > const& args)                                                  \
    {                                                                       \
        new((void*) boost::addressof(address->first)) A(                    \
            boost::forward<A0>(args.a0));                                   \
        new((void*) boost::addressof(address->second)) B(                   \
            BOOST_PP_ENUM_##z(BOOST_PP_DEC(num_params),                     \
                BOOST_UNORDERED_CALL_FORWARD2, args.a));                    \
    }

#define BOOST_UNORDERED_CALL_FORWARD2(z, i, a) \
    BOOST_UNORDERED_CALL_FORWARD(z, BOOST_PP_INC(i), a)

    BOOST_UNORDERED_CONSTRUCT_PAIR_IMPL(1, 2, _)
    BOOST_PP_REPEAT_FROM_TO(4, BOOST_UNORDERED_EMPLACE_LIMIT,
        BOOST_UNORDERED_CONSTRUCT_PAIR_IMPL, _)

#undef BOOST_UNORDERED_CONSTRUCT_PAIR_IMPL
#undef BOOST_UNORDERED_CALL_FORWARD2

#endif // BOOST_UNORDERED_DEPRECATED_PAIR_CONSTRUCT
#endif // BOOST_UNORDERED_VARIADIC_MOVE

    ////////////////////////////////////////////////////////////////////////////
    // Construct without using the emplace args mechanism.

    template <typename T, typename A0>
    inline void construct_impl2(T* address, BOOST_FWD_REF(A0) a0)
    {
        new((void*) address) T(
            boost::forward<A0>(a0)
        );
    }

}}}

#if defined(BOOST_MSVC)
#pragma warning(pop)
#endif

#endif
