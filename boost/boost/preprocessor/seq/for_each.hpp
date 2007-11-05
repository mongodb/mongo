# /* **************************************************************************
#  *                                                                          *
#  *     (C) Copyright Paul Mensonides 2002.
#  *     Distributed under the Boost Software License, Version 1.0. (See
#  *     accompanying file LICENSE_1_0.txt or copy at
#  *     http://www.boost.org/LICENSE_1_0.txt)
#  *                                                                          *
#  ************************************************************************** */
#
# /* See http://www.boost.org for most recent version. */
#
# ifndef BOOST_PREPROCESSOR_SEQ_FOR_EACH_HPP
# define BOOST_PREPROCESSOR_SEQ_FOR_EACH_HPP
#
# include <boost/preprocessor/arithmetic/dec.hpp>
# include <boost/preprocessor/config/config.hpp>
# include <boost/preprocessor/repetition/for.hpp>
# include <boost/preprocessor/seq/seq.hpp>
# include <boost/preprocessor/seq/size.hpp>
# include <boost/preprocessor/tuple/elem.hpp>
# include <boost/preprocessor/tuple/rem.hpp>
#
# /* BOOST_PP_SEQ_FOR_EACH */
#
# if ~BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_EDG()
#    define BOOST_PP_SEQ_FOR_EACH(macro, data, seq) BOOST_PP_FOR((macro, data, seq (nil)), BOOST_PP_SEQ_FOR_EACH_P, BOOST_PP_SEQ_FOR_EACH_O, BOOST_PP_SEQ_FOR_EACH_M)
# else
#    define BOOST_PP_SEQ_FOR_EACH(macro, data, seq) BOOST_PP_SEQ_FOR_EACH_D(macro, data, seq)
#    define BOOST_PP_SEQ_FOR_EACH_D(macro, data, seq) BOOST_PP_FOR((macro, data, seq (nil)), BOOST_PP_SEQ_FOR_EACH_P, BOOST_PP_SEQ_FOR_EACH_O, BOOST_PP_SEQ_FOR_EACH_M)
# endif
#
# define BOOST_PP_SEQ_FOR_EACH_P(r, x) BOOST_PP_DEC(BOOST_PP_SEQ_SIZE(BOOST_PP_TUPLE_ELEM(3, 2, x)))
#
# if BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_STRICT()
#    define BOOST_PP_SEQ_FOR_EACH_O(r, x) BOOST_PP_SEQ_FOR_EACH_O_I x
# else
#    define BOOST_PP_SEQ_FOR_EACH_O(r, x) BOOST_PP_SEQ_FOR_EACH_O_I(BOOST_PP_TUPLE_ELEM(3, 0, x), BOOST_PP_TUPLE_ELEM(3, 1, x), BOOST_PP_TUPLE_ELEM(3, 2, x))
# endif
#
# define BOOST_PP_SEQ_FOR_EACH_O_I(macro, data, seq) (macro, data, BOOST_PP_SEQ_TAIL(seq))
#
# if BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_STRICT()
#    define BOOST_PP_SEQ_FOR_EACH_M(r, x) BOOST_PP_SEQ_FOR_EACH_M_IM(r, BOOST_PP_TUPLE_REM_3 x)
#    define BOOST_PP_SEQ_FOR_EACH_M_IM(r, im) BOOST_PP_SEQ_FOR_EACH_M_I(r, im)
# else
#    define BOOST_PP_SEQ_FOR_EACH_M(r, x) BOOST_PP_SEQ_FOR_EACH_M_I(r, BOOST_PP_TUPLE_ELEM(3, 0, x), BOOST_PP_TUPLE_ELEM(3, 1, x), BOOST_PP_TUPLE_ELEM(3, 2, x))
# endif
#
# define BOOST_PP_SEQ_FOR_EACH_M_I(r, macro, data, seq) macro(r, data, BOOST_PP_SEQ_HEAD(seq))
#
# /* BOOST_PP_SEQ_FOR_EACH_R */
#
# if ~BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_EDG()
#    define BOOST_PP_SEQ_FOR_EACH_R(r, macro, data, seq) BOOST_PP_FOR_ ## r((macro, data, seq (nil)), BOOST_PP_SEQ_FOR_EACH_P, BOOST_PP_SEQ_FOR_EACH_O, BOOST_PP_SEQ_FOR_EACH_M)
# else
#    define BOOST_PP_SEQ_FOR_EACH_R(r, macro, data, seq) BOOST_PP_SEQ_FOR_EACH_R_I(r, macro, data, seq)
#    define BOOST_PP_SEQ_FOR_EACH_R_I(r, macro, data, seq) BOOST_PP_FOR_ ## r((macro, data, seq (nil)), BOOST_PP_SEQ_FOR_EACH_P, BOOST_PP_SEQ_FOR_EACH_O, BOOST_PP_SEQ_FOR_EACH_M)
# endif
#
# endif
