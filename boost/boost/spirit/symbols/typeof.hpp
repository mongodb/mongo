/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_SYMBOLS_TYPEOF_HPP)
#define BOOST_SPIRIT_SYMBOLS_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/symbols/symbols_fwd.hpp>

#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::symbols,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::symbol_inserter,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::impl::tst,2)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::symbols,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::symbols,1)

#endif

