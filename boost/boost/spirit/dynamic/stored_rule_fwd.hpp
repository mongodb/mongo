/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_STORED_RULE_FWD_HPP)
#define BOOST_SPIRIT_STORED_RULE_FWD_HPP

#include <boost/spirit/core/nil.hpp>

namespace boost { namespace spirit {

    template <
        typename T0 = nil_t
      , typename T1 = nil_t
      , typename T2 = nil_t
      , bool EmbedByValue = false
    >
    class stored_rule;

}} // namespace boost::spirit

#endif

