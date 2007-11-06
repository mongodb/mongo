/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_DYNAMIC_TYPEOF_HPP)
#define BOOST_SPIRIT_DYNAMIC_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/core/typeof.hpp>

#include <boost/spirit/dynamic/stored_rule_fwd.hpp>

namespace boost { namespace spirit {

    // if.hpp
    template <class ParsableT, typename CondT> struct if_parser;
    template <class ParsableTrueT, class ParsableFalseT, typename CondT>
    struct if_else_parser;

    // for.hpp
    namespace impl {
    template<typename InitF, typename CondT, typename StepF, class ParsableT>
    struct for_parser;
    }

    // while.hpp
    template<typename ParsableT, typename CondT, bool is_do_parser>
    struct while_parser;

    // lazy.hpp
    template<typename ActorT> struct lazy_parser;

    // rule_alias.hpp
    template <typename ParserT> class rule_alias; 

    // switch.hpp
    template <typename CaseT, typename CondT>       struct switch_parser;
    template <int N, class ParserT, bool IsDefault> struct case_parser;

    // select.hpp
    template <typename TupleT, typename BehaviourT, typename T> 
    struct select_parser;

}} // namespace boost::spirit


#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()

// if.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::if_parser,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::if_else_parser,3)

// for.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::impl::for_parser,4)

// while.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::while_parser,(class)(class)(bool))

// lazy.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::lazy_parser,1)

// stored_rule.hpp (has forward header)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::stored_rule,(typename)(typename)(typename)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::stored_rule,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::stored_rule,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::stored_rule,1)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::stored_rule<>)

// rule_alias.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::rule_alias,1)

// switch.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::switch_parser,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::case_parser,(int)(class)(bool))

// select.hpp
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::select_parser,3)

#endif

