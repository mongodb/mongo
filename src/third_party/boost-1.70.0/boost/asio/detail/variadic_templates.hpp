//
// detail/variadic_templates.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_VARIADIC_TEMPLATES_HPP
#define BOOST_ASIO_DETAIL_VARIADIC_TEMPLATES_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include <boost/asio/detail/config.hpp>

#if !defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

# define BOOST_ASIO_VARIADIC_TPARAMS(n) BOOST_ASIO_VARIADIC_TPARAMS_##n

# define BOOST_ASIO_VARIADIC_TPARAMS_1 \
  typename T1
# define BOOST_ASIO_VARIADIC_TPARAMS_2 \
  typename T1, typename T2
# define BOOST_ASIO_VARIADIC_TPARAMS_3 \
  typename T1, typename T2, typename T3
# define BOOST_ASIO_VARIADIC_TPARAMS_4 \
  typename T1, typename T2, typename T3, typename T4
# define BOOST_ASIO_VARIADIC_TPARAMS_5 \
  typename T1, typename T2, typename T3, typename T4, typename T5

# define BOOST_ASIO_VARIADIC_TARGS(n) BOOST_ASIO_VARIADIC_TARGS_##n

# define BOOST_ASIO_VARIADIC_TARGS_1 T1
# define BOOST_ASIO_VARIADIC_TARGS_2 T1, T2
# define BOOST_ASIO_VARIADIC_TARGS_3 T1, T2, T3
# define BOOST_ASIO_VARIADIC_TARGS_4 T1, T2, T3, T4
# define BOOST_ASIO_VARIADIC_TARGS_5 T1, T2, T3, T4, T5

# define BOOST_ASIO_VARIADIC_BYVAL_PARAMS(n) \
  BOOST_ASIO_VARIADIC_BYVAL_PARAMS_##n

# define BOOST_ASIO_VARIADIC_BYVAL_PARAMS_1 T1 x1
# define BOOST_ASIO_VARIADIC_BYVAL_PARAMS_2 T1 x1, T2 x2
# define BOOST_ASIO_VARIADIC_BYVAL_PARAMS_3 T1 x1, T2 x2, T3 x3
# define BOOST_ASIO_VARIADIC_BYVAL_PARAMS_4 T1 x1, T2 x2, T3 x3, T4 x4
# define BOOST_ASIO_VARIADIC_BYVAL_PARAMS_5 T1 x1, T2 x2, T3 x3, T4 x4, T5 x5

# define BOOST_ASIO_VARIADIC_BYVAL_ARGS(n) \
  BOOST_ASIO_VARIADIC_BYVAL_ARGS_##n

# define BOOST_ASIO_VARIADIC_BYVAL_ARGS_1 x1
# define BOOST_ASIO_VARIADIC_BYVAL_ARGS_2 x1, x2
# define BOOST_ASIO_VARIADIC_BYVAL_ARGS_3 x1, x2, x3
# define BOOST_ASIO_VARIADIC_BYVAL_ARGS_4 x1, x2, x3, x4
# define BOOST_ASIO_VARIADIC_BYVAL_ARGS_5 x1, x2, x3, x4, x5

# define BOOST_ASIO_VARIADIC_CONSTREF_PARAMS(n) \
  BOOST_ASIO_VARIADIC_CONSTREF_PARAMS_##n

# define BOOST_ASIO_VARIADIC_CONSTREF_PARAMS_1 \
  const T1& x1
# define BOOST_ASIO_VARIADIC_CONSTREF_PARAMS_2 \
  const T1& x1, const T2& x2
# define BOOST_ASIO_VARIADIC_CONSTREF_PARAMS_3 \
  const T1& x1, const T2& x2, const T3& x3
# define BOOST_ASIO_VARIADIC_CONSTREF_PARAMS_4 \
  const T1& x1, const T2& x2, const T3& x3, const T4& x4
# define BOOST_ASIO_VARIADIC_CONSTREF_PARAMS_5 \
  const T1& x1, const T2& x2, const T3& x3, const T4& x4, const T5& x5

# define BOOST_ASIO_VARIADIC_MOVE_PARAMS(n) \
  BOOST_ASIO_VARIADIC_MOVE_PARAMS_##n

# define BOOST_ASIO_VARIADIC_MOVE_PARAMS_1 \
  BOOST_ASIO_MOVE_ARG(T1) x1
# define BOOST_ASIO_VARIADIC_MOVE_PARAMS_2 \
  BOOST_ASIO_MOVE_ARG(T1) x1, BOOST_ASIO_MOVE_ARG(T2) x2
# define BOOST_ASIO_VARIADIC_MOVE_PARAMS_3 \
  BOOST_ASIO_MOVE_ARG(T1) x1, BOOST_ASIO_MOVE_ARG(T2) x2, \
  BOOST_ASIO_MOVE_ARG(T3) x3
# define BOOST_ASIO_VARIADIC_MOVE_PARAMS_4 \
  BOOST_ASIO_MOVE_ARG(T1) x1, BOOST_ASIO_MOVE_ARG(T2) x2, \
  BOOST_ASIO_MOVE_ARG(T3) x3, BOOST_ASIO_MOVE_ARG(T4) x4
# define BOOST_ASIO_VARIADIC_MOVE_PARAMS_5 \
  BOOST_ASIO_MOVE_ARG(T1) x1, BOOST_ASIO_MOVE_ARG(T2) x2, \
  BOOST_ASIO_MOVE_ARG(T3) x3, BOOST_ASIO_MOVE_ARG(T4) x4, \
  BOOST_ASIO_MOVE_ARG(T5) x5

# define BOOST_ASIO_VARIADIC_MOVE_ARGS(n) \
  BOOST_ASIO_VARIADIC_MOVE_ARGS_##n

# define BOOST_ASIO_VARIADIC_MOVE_ARGS_1 \
  BOOST_ASIO_MOVE_CAST(T1)(x1)
# define BOOST_ASIO_VARIADIC_MOVE_ARGS_2 \
  BOOST_ASIO_MOVE_CAST(T1)(x1), BOOST_ASIO_MOVE_CAST(T2)(x2)
# define BOOST_ASIO_VARIADIC_MOVE_ARGS_3 \
  BOOST_ASIO_MOVE_CAST(T1)(x1), BOOST_ASIO_MOVE_CAST(T2)(x2), \
  BOOST_ASIO_MOVE_CAST(T3)(x3)
# define BOOST_ASIO_VARIADIC_MOVE_ARGS_4 \
  BOOST_ASIO_MOVE_CAST(T1)(x1), BOOST_ASIO_MOVE_CAST(T2)(x2), \
  BOOST_ASIO_MOVE_CAST(T3)(x3), BOOST_ASIO_MOVE_CAST(T4)(x4)
# define BOOST_ASIO_VARIADIC_MOVE_ARGS_5 \
  BOOST_ASIO_MOVE_CAST(T1)(x1), BOOST_ASIO_MOVE_CAST(T2)(x2), \
  BOOST_ASIO_MOVE_CAST(T3)(x3), BOOST_ASIO_MOVE_CAST(T4)(x4), \
  BOOST_ASIO_MOVE_CAST(T5)(x5)

# define BOOST_ASIO_VARIADIC_DECAY(n) \
  BOOST_ASIO_VARIADIC_DECAY_##n

# define BOOST_ASIO_VARIADIC_DECAY_1 \
  typename decay<T1>::type
# define BOOST_ASIO_VARIADIC_DECAY_2 \
  typename decay<T1>::type, typename decay<T2>::type
# define BOOST_ASIO_VARIADIC_DECAY_3 \
  typename decay<T1>::type, typename decay<T2>::type, \
  typename decay<T3>::type
# define BOOST_ASIO_VARIADIC_DECAY_4 \
  typename decay<T1>::type, typename decay<T2>::type, \
  typename decay<T3>::type, typename decay<T4>::type
# define BOOST_ASIO_VARIADIC_DECAY_5 \
  typename decay<T1>::type, typename decay<T2>::type, \
  typename decay<T3>::type, typename decay<T4>::type, \
  typename decay<T5>::type

# define BOOST_ASIO_VARIADIC_GENERATE(m) m(1) m(2) m(3) m(4) m(5)

#endif // !defined(BOOST_ASIO_HAS_VARIADIC_TEMPLATES)

#endif // BOOST_ASIO_DETAIL_VARIADIC_TEMPLATES_HPP
