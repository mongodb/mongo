// Boost result_of library

//  Copyright Douglas Gregor 2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// For more information, see http://www.boost.org/libs/utility
#if !defined(BOOST_PP_IS_ITERATING)
# error Boost result_of - do not include this file!
#endif

// CWPro8 requires an argument in a function type specialization
#if BOOST_WORKAROUND(__MWERKS__, BOOST_TESTED_AT(0x3002)) && BOOST_PP_ITERATION() == 0
# define BOOST_RESULT_OF_ARGS void
#else
# define BOOST_RESULT_OF_ARGS BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),T)
#endif

#if !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x551))
template<typename F BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<F(BOOST_RESULT_OF_ARGS)>
    : detail::result_of<F, F(BOOST_RESULT_OF_ARGS)> {};
#endif

namespace detail {

template<typename R,  typename FArgs BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<R (*)(BOOST_RESULT_OF_ARGS), FArgs>
{
  typedef R type;
};

template<typename R,  typename FArgs BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<R (&)(BOOST_RESULT_OF_ARGS), FArgs>
{
  typedef R type;
};

#undef BOOST_RESULT_OF_ARGS

#if BOOST_PP_ITERATION() > 1 && !BOOST_WORKAROUND(__BORLANDC__, BOOST_TESTED_AT(0x551))
template<typename R, typename FArgs BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<R (T0::*)
                     (BOOST_PP_ENUM_SHIFTED_PARAMS(BOOST_PP_ITERATION(),T)),
                 FArgs>
{
  typedef R type;
};

template<typename R, typename FArgs BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<R (T0::*)
                     (BOOST_PP_ENUM_SHIFTED_PARAMS(BOOST_PP_ITERATION(),T))
                     const,
                 FArgs>
{
  typedef R type;
};

template<typename R, typename FArgs BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<R (T0::*)
                     (BOOST_PP_ENUM_SHIFTED_PARAMS(BOOST_PP_ITERATION(),T))
                     volatile,
                 FArgs>
{
  typedef R type;
};

template<typename R, typename FArgs BOOST_PP_COMMA_IF(BOOST_PP_ITERATION())
         BOOST_PP_ENUM_PARAMS(BOOST_PP_ITERATION(),typename T)>
struct result_of<R (T0::*)
                     (BOOST_PP_ENUM_SHIFTED_PARAMS(BOOST_PP_ITERATION(),T))
                     const volatile,
                 FArgs>
{
  typedef R type;
};
#endif

}
