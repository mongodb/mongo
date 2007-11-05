/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_UTILITY_TYPEOF_HPP)
#define BOOST_SPIRIT_UTILITY_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/core/typeof.hpp>

#include <boost/spirit/utility/escape_char_fwd.hpp>
#include <boost/spirit/utility/confix_fwd.hpp>
#include <boost/spirit/utility/lists_fwd.hpp>
#include <boost/spirit/utility/distinct_fwd.hpp>
#include <boost/spirit/utility/grammar_def_fwd.hpp>

namespace boost { namespace spirit {

    // chset.hpp
    template<typename CharT> class chset;        

    // functor_parser.hpp
    template<typename FunctorT> struct functor_parser;

    // loops.hpp
    template<class ParserT, typename ExactT> class fixed_loop;
    template<class ParserT, typename MinT, typename MaxT> class finite_loop;
    template<class ParserT, typename MinT> class infinite_loop;

    // regex.hpp
    template<typename CharT> struct rxstrlit;

    // flush_multi_pass.hpp
    class flush_multi_pass_parser;  

    // scoped_lock.hpp
    template<class MutexT, class ParserT> struct scoped_lock_parser;

}} // namespace boost::spirit


#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


// chset.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::chset,1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::chset<char>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::chset<wchar_t>)


// escape_char.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::escape_char_parser,(BOOST_TYPEOF_INTEGRAL(unsigned long))(typename))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::escape_char_action,(class)(typename)(BOOST_TYPEOF_INTEGRAL(unsigned long))(typename))

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::escape_char_parser,(BOOST_TYPEOF_INTEGRAL(unsigned long)))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::escape_char_action,(class)(typename)(BOOST_TYPEOF_INTEGRAL(unsigned long)))


// functor_parser.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::functor_parser,1)


// loops.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::fixed_loop,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::finite_loop,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::infinite_loop,2)


// regex.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::rxstrlit,1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::rxstrlit<char>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::rxstrlit<wchar_t>)


// confix.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::confix_parser, 6)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::confix_parser, 5)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::confix_parser, 4)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::confix_parser, 3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::comment_nest_parser, 2)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::is_nested)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::non_nested)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::is_lexeme)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::non_lexeme)


// lists.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::list_parser,4)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::list_parser,3)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::list_parser,2)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::no_list_endtoken)


// distinct.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::distinct_parser,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::distinct_parser,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::dynamic_distinct_parser,1)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::distinct_parser<>)


// flush_multi_pass.hpp

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::flush_multi_pass_parser)


// scoped_lock.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scoped_lock_parser,2)


// grammar_gen.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::grammar_def,BOOST_SPIRIT_GRAMMAR_STARTRULE_TYPE_LIMIT)

#if BOOST_SPIRIT_GRAMMAR_STARTRULE_TYPE_LIMIT > 12
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::grammar_def,12)
#endif
#if BOOST_SPIRIT_GRAMMAR_STARTRULE_TYPE_LIMIT >  9
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::grammar_def, 9)
#endif
#if BOOST_SPIRIT_GRAMMAR_STARTRULE_TYPE_LIMIT >  6
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::grammar_def, 6)
#endif
#if BOOST_SPIRIT_GRAMMAR_STARTRULE_TYPE_LIMIT >  3
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::grammar_def, 3)
#endif


#endif


