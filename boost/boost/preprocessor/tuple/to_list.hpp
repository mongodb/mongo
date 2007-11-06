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
# ifndef BOOST_PREPROCESSOR_TUPLE_TO_LIST_HPP
# define BOOST_PREPROCESSOR_TUPLE_TO_LIST_HPP
#
# include <boost/preprocessor/config/config.hpp>
#
# /* BOOST_PP_TUPLE_TO_LIST */
#
# if ~BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_MWCC()
#    define BOOST_PP_TUPLE_TO_LIST(size, tuple) BOOST_PP_TUPLE_TO_LIST_I(size, tuple)
#    if ~BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_MSVC()
#        define BOOST_PP_TUPLE_TO_LIST_I(s, t) BOOST_PP_TUPLE_TO_LIST_ ## s t
#    else
#        define BOOST_PP_TUPLE_TO_LIST_I(s, t) BOOST_PP_TUPLE_TO_LIST_II(BOOST_PP_TUPLE_TO_LIST_ ## s t)
#        define BOOST_PP_TUPLE_TO_LIST_II(res) res
#    endif
# else
#    define BOOST_PP_TUPLE_TO_LIST(size, tuple) BOOST_PP_TUPLE_TO_LIST_OO((size, tuple))
#    define BOOST_PP_TUPLE_TO_LIST_OO(par) BOOST_PP_TUPLE_TO_LIST_I ## par
#    define BOOST_PP_TUPLE_TO_LIST_I(s, t) BOOST_PP_TUPLE_TO_LIST_ ## s ## t
# endif
#
# define BOOST_PP_TUPLE_TO_LIST_0() BOOST_PP_NIL
# define BOOST_PP_TUPLE_TO_LIST_1(a) (a, BOOST_PP_NIL)
# define BOOST_PP_TUPLE_TO_LIST_2(a, b) (a, (b, BOOST_PP_NIL))
# define BOOST_PP_TUPLE_TO_LIST_3(a, b, c) (a, (b, (c, BOOST_PP_NIL)))
# define BOOST_PP_TUPLE_TO_LIST_4(a, b, c, d) (a, (b, (c, (d, BOOST_PP_NIL))))
# define BOOST_PP_TUPLE_TO_LIST_5(a, b, c, d, e) (a, (b, (c, (d, (e, BOOST_PP_NIL)))))
# define BOOST_PP_TUPLE_TO_LIST_6(a, b, c, d, e, f) (a, (b, (c, (d, (e, (f, BOOST_PP_NIL))))))
# define BOOST_PP_TUPLE_TO_LIST_7(a, b, c, d, e, f, g) (a, (b, (c, (d, (e, (f, (g, BOOST_PP_NIL)))))))
# define BOOST_PP_TUPLE_TO_LIST_8(a, b, c, d, e, f, g, h) (a, (b, (c, (d, (e, (f, (g, (h, BOOST_PP_NIL))))))))
# define BOOST_PP_TUPLE_TO_LIST_9(a, b, c, d, e, f, g, h, i) (a, (b, (c, (d, (e, (f, (g, (h, (i, BOOST_PP_NIL)))))))))
# define BOOST_PP_TUPLE_TO_LIST_10(a, b, c, d, e, f, g, h, i, j) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, BOOST_PP_NIL))))))))))
# define BOOST_PP_TUPLE_TO_LIST_11(a, b, c, d, e, f, g, h, i, j, k) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, BOOST_PP_NIL)))))))))))
# define BOOST_PP_TUPLE_TO_LIST_12(a, b, c, d, e, f, g, h, i, j, k, l) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, BOOST_PP_NIL))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_13(a, b, c, d, e, f, g, h, i, j, k, l, m) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, BOOST_PP_NIL)))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_14(a, b, c, d, e, f, g, h, i, j, k, l, m, n) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, BOOST_PP_NIL))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_15(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, BOOST_PP_NIL)))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, BOOST_PP_NIL))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_17(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, BOOST_PP_NIL)))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_18(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, BOOST_PP_NIL))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_19(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, BOOST_PP_NIL)))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_20(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, (t, BOOST_PP_NIL))))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_21(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, (t, (u, BOOST_PP_NIL)))))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_22(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, (t, (u, (v, BOOST_PP_NIL))))))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_23(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, (t, (u, (v, (w, BOOST_PP_NIL)))))))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_24(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, (t, (u, (v, (w, (x, BOOST_PP_NIL))))))))))))))))))))))))
# define BOOST_PP_TUPLE_TO_LIST_25(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r, s, t, u, v, w, x, y) (a, (b, (c, (d, (e, (f, (g, (h, (i, (j, (k, (l, (m, (n, (o, (p, (q, (r, (s, (t, (u, (v, (w, (x, (y, BOOST_PP_NIL)))))))))))))))))))))))))
#
# endif
