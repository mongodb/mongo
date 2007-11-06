/*=============================================================================
    Copyright (c) 2001-2003 Joel de Guzman

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
==============================================================================*/
#if !defined(FUSION_SEQUENCE_LIMITS_HPP)
#define FUSION_SEQUENCE_LIMITS_HPP

#if !defined(FUSION_MAX_TUPLE_SIZE)
# define FUSION_MAX_TUPLE_SIZE 10
#else
# if FUSION_MAX_TUPLE_SIZE < 3
#   undef FUSION_MAX_TUPLE_SIZE
#   define FUSION_MAX_TUPLE_SIZE 10
# endif
#endif

#endif
