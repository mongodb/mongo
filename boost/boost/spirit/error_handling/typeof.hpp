/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_ERROR_HANDLING_TYPEOF_HPP)
#define BOOST_SPIRIT_ERROR_HANDLING_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/core/typeof.hpp>

#include <boost/spirit/error_handling/exceptions_fwd.hpp>


#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


// exceptions.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::parser_error,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::assertive_parser,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::error_status,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::fallback_parser,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::guard,1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::error_status<>)


#endif

