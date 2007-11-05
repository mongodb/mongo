/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman
    Copyright (c) 2004 Peder Holt

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_DETAIL_CONFIG_HPP)
#define FUSION_DETAIL_CONFIG_HPP

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/preprocessor/cat.hpp>
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
#include <boost/mpl/bool.hpp>
#endif

#if     (defined(BOOST_MSVC) && (BOOST_MSVC < 1310))                            \
    ||  (defined(__BORLANDC__) && (__BORLANDC__ <= 0x570))                      \
    ||  (defined(__GNUC__) && (__GNUC__ < 3))                                   \
    ||  (defined(__GNUC__) && (__GNUC__ == 3) && (__GNUC_MINOR__ < 1))
#else
# define FUSION_COMFORMING_COMPILER
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  BOOST_NO_TEMPLATED_STREAMS macro. This ought to be in boost.config
//
///////////////////////////////////////////////////////////////////////////////
#if defined __GNUC__ && __GNUC__ == 2 && __GNUC_MINOR__ <= 97
#define BOOST_NO_TEMPLATED_STREAMS
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  Before including MPL, we define these dummy template functions. Borland
//  complains when a template class has the same name as a template function,
//  regardless if they are in different namespaces. This is a workaround to
//  this Borland quirk.
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
namespace boost { namespace fusion { namespace borland_only {

    template <typename T> void begin(T) {}
    template <typename T> void end(T) {}
    template <typename T> void next(T) {}
    template <typename T> void prior(T) {}
    template <typename T> void find(T) {}
    template <typename T> void find_if(T) {}

}}}
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  MSVC, even with VC7.1 has problems with returning a default constructed
//  value of a given type: return type(); This only happens on debug builds.
//  It seems to be a return value optimization bug.
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1301) && !defined(NDEBUG)
# define FUSION_RETURN_DEFAULT_CONSTRUCTED type r=type(); return r
#else
# define FUSION_RETURN_DEFAULT_CONSTRUCTED return type()
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  Borland does not like the T::value syntax. Instead, we use a metafunction
//  get_value<T>::value. The explicit qualification (::boost::fusion::detail::)
//  also makes Borland happy.
//
//  VC6/7 on the other hand chokes with ETI (early instantiation bug). So we
//  forward the call to get_value<T>::value and fix the ETI bug there (see
//  get_value below).
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)                                    \
    || BOOST_WORKAROUND(BOOST_MSVC, < 1300)
namespace boost { namespace fusion { namespace detail
{
    template <typename T>
    struct get_value
    {
        BOOST_STATIC_CONSTANT(int, value = T::value);
    };

#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)

    // VC6 ETI (early template instantiation) bug workaround.
    template <>
    struct get_value<int>
    {
        BOOST_STATIC_CONSTANT(int, value = 0);
    };
#endif
}}}
#endif

#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)                                    \
    || BOOST_WORKAROUND(BOOST_MSVC, < 1300)
# define FUSION_GET_VALUE(T) ::boost::fusion::detail::get_value<T>::value
#else
# define FUSION_GET_VALUE(T) T::value
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  Borland does not like returning a const reference from a tuple member.
//  We do the cast explicitly.
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)
# define FUSION_RETURN_TUPLE_MEMBER(n)                                          \
    typedef typename tuple_access_result<n, Tuple>::type type;                  \
    return type(t.BOOST_PP_CAT(m, n))
#else
# define FUSION_RETURN_TUPLE_MEMBER(n)                                          \
    return t.BOOST_PP_CAT(m, n)
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  See get.hpp. In function get<N>(t), mpl::int_<N>* = 0 is a function
//  parameter that defaults to 0. This is a hack to make VC6 happy, otherwise,
//  VC6 will return the wrong result from a wrong index!
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
# define FUSION_GET_MSVC_WORKAROUND , mpl::int_<N>* = 0
#else
# define FUSION_GET_MSVC_WORKAROUND
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  FUSION_MSVC_ETI_WRAPPER (VC6 and VC7)
//
//  VC6/VC7 chokes with ETI (early instantiation bug) with typename T::name.
//  So, we forward the call to get_name<T>::type and fix the ETI bug.
//
///////////////////////////////////////////////////////////////////////////////

// VC6 ETI (early template instantiation) bug workaround.
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
#define FUSION_MSVC_ETI_WRAPPER(name)                                           \
namespace boost { namespace fusion { namespace detail                           \
{                                                                               \
    template <typename T>                                                       \
    struct BOOST_PP_CAT(get_, name)                                             \
    {                                                                           \
        typedef typename T::name type;                                          \
    };                                                                          \
                                                                                \
    template <>                                                                 \
    struct BOOST_PP_CAT(get_, name)<int>                                        \
    {                                                                           \
        typedef int type;                                                       \
    };                                                                          \
}}}
#endif
/*
//  is_msvc_70_ETI_arg: Detect a VC7 ETI arg
#if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
namespace boost { namespace fusion { namespace detail
{
    struct int_convertible_
    {
        int_convertible_(int);
    };

    template< typename T >
    struct is_msvc_70_ETI_arg
    {
        typedef char (&no_tag)[1];
        typedef char (&yes_tag)[2];

        static no_tag test(...);
        static yes_tag test(int_convertible_);
        static T get();

        BOOST_STATIC_CONSTANT(bool, value =
              sizeof(test(get())) == sizeof(yes_tag)
            );
    };
}}}
#endif

// VC7 ETI (early template instantiation) bug workaround.
#if BOOST_WORKAROUND(BOOST_MSVC, == 1300)
#define FUSION_MSVC_ETI_WRAPPER(name)                                           \
namespace boost { namespace fusion { namespace detail                           \
{                                                                               \
    template <bool>                                                             \
    struct BOOST_PP_CAT(get_impl_, name)                                        \
    {                                                                           \
        template <typename T>                                                   \
        struct result                                                           \
        {                                                                       \
            typedef int type;                                                   \
        };                                                                      \
    };                                                                          \
                                                                                \
    struct BOOST_PP_CAT(get_impl_, name)<false>                                 \
    {                                                                           \
        template <typename T>                                                   \
        struct result                                                           \
        {                                                                       \
            typedef typename T::name type;                                      \
        };                                                                      \
    };                                                                          \
                                                                                \
    template <typename T>                                                       \
    struct BOOST_PP_CAT(get_, name)                                             \
        : BOOST_PP_CAT(get_impl_, name)<is_msvc_70_ETI_arg<T>::value>           \
            ::template result<T> {};                                            \
}}}
#endif
*/
///////////////////////////////////////////////////////////////////////////////
//
//  T::tag wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
FUSION_MSVC_ETI_WRAPPER(tag)
# define FUSION_GET_TAG(T) ::boost::fusion::detail::get_tag<T>::type
#else
# define FUSION_GET_TAG(T) typename T::tag
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  T::type wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)
FUSION_MSVC_ETI_WRAPPER(type)
# define FUSION_GET_TYPE(T) ::boost::fusion::detail::get_type<T>::type
#else
# define FUSION_GET_TYPE(T) typename T::type
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  T::types wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
FUSION_MSVC_ETI_WRAPPER(types)
# define FUSION_GET_TYPES(T) ::boost::fusion::detail::get_types<T>::type
#else
# define FUSION_GET_TYPES(T) typename T::types
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  T::index wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
FUSION_MSVC_ETI_WRAPPER(index)
# define FUSION_GET_INDEX(T) ::boost::fusion::detail::get_index<T>::type
#else
# define FUSION_GET_INDEX(T) typename T::index
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  T::tuple wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
FUSION_MSVC_ETI_WRAPPER(tuple)
# define FUSION_GET_TUPLE(T) ::boost::fusion::detail::get_tuple<T>::type
#else
# define FUSION_GET_TUPLE(T) typename T::tuple
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  T::size wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
FUSION_MSVC_ETI_WRAPPER(size)
# define FUSION_GET_SIZE(T) ::boost::fusion::detail::get_size<T>::type
#else
# define FUSION_GET_SIZE(T) typename T::size
#endif

///////////////////////////////////////////////////////////////////////////////
//
//  T::value_type wrapper
//
///////////////////////////////////////////////////////////////////////////////
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
FUSION_MSVC_ETI_WRAPPER(value_type)
# define FUSION_GET_VALUE_TYPE(T) ::boost::fusion::detail::get_value_type<T>::type
#else
# define FUSION_GET_VALUE_TYPE(T) typename T::value_type
#endif

#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)

namespace boost {namespace fusion { namespace aux {
template< typename T >
struct msvc_never_true
{
    enum { value = false };
};
}}} //namespace boost::fusion::aux

#endif

namespace boost {namespace fusion { 
#if BOOST_WORKAROUND(BOOST_MSVC, <= 1300)

namespace aux {
    // msvc_apply
#define AUX778076_MSVC_DTW_NAME msvc_apply1
#define AUX778076_MSVC_DTW_ORIGINAL_NAME apply
#define AUX778076_MSVC_DTW_ARITY 1
#include "boost/mpl/aux_/msvc_dtw.hpp"

#define AUX778076_MSVC_DTW_NAME msvc_apply2
#define AUX778076_MSVC_DTW_ORIGINAL_NAME apply
#define AUX778076_MSVC_DTW_ARITY 2
#include "boost/mpl/aux_/msvc_dtw.hpp"

} //namespace aux

template<typename A,typename B>
struct fusion_apply1
{
    typedef typename aux::msvc_apply1<A>::template result_<B>::type type;
};

template<typename A,typename B,typename C>
struct fusion_apply2
{
    typedef typename aux::msvc_apply2<A>::template result_<B,C>::type type;
};

#else 
template<typename A,typename B>
struct fusion_apply1
{
    typedef typename A::template apply<B>::type type;
};
template<typename A,typename B,typename C>
struct fusion_apply2
{
    typedef typename A::template apply<B,C>::type type;
};
#endif
}} //namespace boost::fusion

namespace boost {namespace fusion {namespace detail {
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
    template<typename T>
    struct bool_base {};
    template<>
    struct bool_base<mpl::bool_<true> > : boost::mpl::bool_<true>{};
    template<>
    struct bool_base<mpl::bool_<false> > : boost::mpl::bool_<false>{};
#else
    template<typename T>
    struct bool_base : T {};
#endif
}}}

//VC 6 has serious problems with mpl::int_ in tuple_iterator_base. 
//It ICEs because operator int() const on mpl::int_ is inlined. 
//At the same time, another test using integral_c<T,N> ICEs because operator int() is not inlined. 
//Only solution seems to be to define a special msvc_fusion_int for VC 6 to be used in tuple_iterator_base
#if BOOST_WORKAROUND(BOOST_MSVC, < 1300)
namespace boost {namespace fusion {namespace detail{

template<int N>
struct msvc_fusion_int
{
    BOOST_STATIC_CONSTANT(int, value = N);
    typedef msvc_fusion_int<N> type;
    typedef int value_type;
    typedef boost::mpl::integral_c_tag tag;

    typedef msvc_fusion_int<value + 1> next;
    typedef msvc_fusion_int<value - 1> prior;

    operator int() const;
};

template<int N>
msvc_fusion_int<N>::operator int() const
{
    return static_cast<int>(this->value); 
}

}}}
#define FUSION_INT(N) boost::fusion::detail::msvc_fusion_int<N>
#else
#define FUSION_INT(N) boost::mpl::int_<N>
#endif



///////////////////////////////////////////////////////////////////////////////
//
//   Borland is so flaky with const correctness of iterators. It's getting
//   confused with tuple_iterator<N, T> where T is a const tuple. We cast
//   what Borland thinks is a const reference to a true reference.
//
///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace fusion { namespace detail
{
#if BOOST_WORKAROUND(__BORLANDC__, <= 0x551)

    template <typename T>
    T& ref(T const& r)
    {
        return const_cast<T&>(r);
    }

#else

    template <typename T>
    T& ref(T& r)
    {
        return r;
    }

#endif

}}}

#endif


