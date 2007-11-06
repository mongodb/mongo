# /* Copyright (C) 2001
#  * Housemarque Oy
#  * http://www.housemarque.com
#  *
#  * Distributed under the Boost Software License, Version 1.0. (See
#  * accompanying file LICENSE_1_0.txt or copy at
#  * http://www.boost.org/LICENSE_1_0.txt)
#  */
#
# /* Revised by Paul Mensonides (2002) */
#
# /* See http://www.boost.org for most recent version. */
#
# ifndef BOOST_PREPROCESSOR_TUPLE_EAT_HPP
# define BOOST_PREPROCESSOR_TUPLE_EAT_HPP
#
# include <boost/preprocessor/config/config.hpp>
#
# /* BOOST_PP_TUPLE_EAT */
#
# if ~BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_MWCC()
#    define BOOST_PP_TUPLE_EAT(size) BOOST_PP_TUPLE_EAT_I(size)
# else
#    define BOOST_PP_TUPLE_EAT(size) BOOST_PP_TUPLE_EAT_OO((size))
#    define BOOST_PP_TUPLE_EAT_OO(par) BOOST_PP_TUPLE_EAT_I ## par
# endif
#
# define BOOST_PP_TUPLE_EAT_I(size) BOOST_PP_TUPLE_EAT_ ## size
#
# define BOOST_PP_TUPLE_EAT_0()
# define BOOST_PP_TUPLE_EAT_1(a)
# define BOOST_PP_TUPLE_EAT_2(a, b)
# define BOOST_PP_TUPLE_EAT_3(a, b, c)
# define BOOST_PP_TUPLE_EAT_4(a, b, c, d)
# define BOOST_PP_TUPLE_EAT_5(a, b, c, d, e)
# define BOOST_PP_TUPLE_EAT_6(a, b, c, d, e, f)
# define BOOST_PP_TUPLE_EAT_7(a, b, c, d, e, f, g)
# define BOOST_PP_TUPLE_EAT_8(a, b, c, d, e, f, g, h)
# define BOOST_PP_TUPLE_EAT_9(a, b, c, d, e, f, g, h, i)
# define BOOST_PP_TUPLE_EAT_10(a, b, c, d, e, f, g, h, i, j)
# define BOOST_PP_TUPLE_EAT_11(a, b, c, d, e, f, g, h, i, j, k)
# define BOOST_PP_TUPLE_EAT_12(a, b, c, d, e, f, g, h, i, j, k, l)
# define BOOST_PP_TUPLE_EAT_13(a, b, c, d, e, f, g, h, i, j, k, l, m)
# define BOOST_PP_TUPLE_EAT_14(a, b, c, d, e, f, g, h, i, j, k, l, m, n)
# define BOOST_PP_TUPLE_EAT_15(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o)
# define BOOST_PP_TUPLE_EAT_16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p)
# define BOOST_PP_TUPLE_EAT_17(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q)
# define BOOST_PP_TUPLE_EAT_18(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r)
# define BOOST_PP_TUPLE_EAT_19(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s)
# define BOOST_PP_TUPLE_EAT_20(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t)
# define BOOST_PP_TUPLE_EAT_21(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u)
# define BOOST_PP_TUPLE_EAT_22(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v)
# define BOOST_PP_TUPLE_EAT_23(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w)
# define BOOST_PP_TUPLE_EAT_24(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x)
# define BOOST_PP_TUPLE_EAT_25(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y)
#
# endif
