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
# ifndef BOOST_PREPROCESSOR_SEQ_REST_N_HPP
# define BOOST_PREPROCESSOR_SEQ_REST_N_HPP
#
# include <boost/preprocessor/arithmetic/inc.hpp>
# include <boost/preprocessor/config/config.hpp>
# include <boost/preprocessor/facilities/empty.hpp>
# include <boost/preprocessor/seq/detail/split.hpp>
# include <boost/preprocessor/tuple/elem.hpp>
#
# /* BOOST_PP_SEQ_REST_N */
#
# if ~BOOST_PP_CONFIG_FLAGS() & BOOST_PP_CONFIG_EDG()
#    define BOOST_PP_SEQ_REST_N(n, seq) BOOST_PP_TUPLE_ELEM(2, 1, BOOST_PP_SEQ_SPLIT(BOOST_PP_INC(n), (nil) seq BOOST_PP_EMPTY))()
# else
#    define BOOST_PP_SEQ_REST_N(n, seq) BOOST_PP_SEQ_REST_N_I(n, seq)
#    define BOOST_PP_SEQ_REST_N_I(n, seq) BOOST_PP_TUPLE_ELEM(2, 1, BOOST_PP_SEQ_SPLIT(BOOST_PP_INC(n), (nil) seq BOOST_PP_EMPTY))()
# endif
#
# endif
