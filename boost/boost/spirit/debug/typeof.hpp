/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_DEBUG_TYPEOF_HPP)
#define BOOST_SPIRIT_DEBUG_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/core/typeof.hpp>


namespace boost { namespace spirit {

    // debug_node.hpp
    template<typename ContextT> struct parser_context_linker;
    template<typename ScannerT> struct scanner_context_linker;
    template<typename ContextT> struct closure_context_linker;

}} // namespace boost::spirit


#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


// debug_node.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::parser_context_linker,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner_context_linker,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::closure_context_linker,1)

#endif

