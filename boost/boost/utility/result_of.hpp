// Boost result_of library

//  Copyright Douglas Gregor 2004. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

// For more information, see http://www.boost.org/libs/utility
#ifndef BOOST_RESULT_OF_HPP
#define BOOST_RESULT_OF_HPP

#include <boost/config.hpp>
#include <boost/type_traits/ice.hpp>
#include <boost/type.hpp>
#include <boost/preprocessor.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/mpl/has_xxx.hpp>

#ifndef BOOST_RESULT_OF_NUM_ARGS
#  define BOOST_RESULT_OF_NUM_ARGS 10
#endif

namespace boost {

template<typename F> struct result_of;

#if !defined(BOOST_NO_SFINAE) && !defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION)
namespace detail {

BOOST_MPL_HAS_XXX_TRAIT_DEF(result_type)

template<typename F, typename FArgs, bool HasResultType> struct get_result_of;

template<typename F, typename FArgs>
struct get_result_of<F, FArgs, true>
{
  typedef typename F::result_type type;
};

template<typename F, typename FArgs>
struct get_result_of<F, FArgs, false>
{
  typedef typename F::template result<FArgs>::type type;
};

template<typename F>
struct get_result_of<F, F(void), false>
{
  typedef void type;
};

template<typename F, typename FArgs>
struct result_of : get_result_of<F, FArgs, (has_result_type<F>::value)> {};

} // end namespace detail

#define BOOST_PP_ITERATION_PARAMS_1 (3,(0,BOOST_RESULT_OF_NUM_ARGS,<boost/utility/detail/result_of_iterate.hpp>))
#include BOOST_PP_ITERATE()

#else
#  define BOOST_NO_RESULT_OF 1
#endif

}

#endif // BOOST_RESULT_OF_HPP
