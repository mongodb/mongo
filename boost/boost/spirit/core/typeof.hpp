/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_CORE_TYPEOF_HPP)
#define BOOST_SPIRIT_CORE_TYPEOF_HPP

#include <boost/config.hpp>
#include <boost/cstdint.hpp>

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/core/nil.hpp>
#include <boost/spirit/core/primitives/numerics_fwd.hpp>
#include <boost/spirit/core/scanner/scanner_fwd.hpp>
#include <boost/spirit/core/scanner/skipper_fwd.hpp>
#include <boost/spirit/core/non_terminal/subrule_fwd.hpp>

namespace boost { namespace spirit {

    // parser.hpp
    template <typename IteratorT> struct parse_info;
    struct plain_parser_category;
    struct binary_parser_category;
    struct unary_parser_category;
    struct action_parser_category;

    // match.hpp
    template<typename T> class match; 

    // primitives/primitives.hpp
    template<class ParserT> struct negated_char_parser;
    template<typename CharT> struct chlit;
    template<typename CharT> struct range;
    template<typename IteratorT> class chseq;
    template<typename IteratorT> class strlit;
    struct nothing_parser;
    struct anychar_parser;
    struct alnum_parser;
    struct alpha_parser;
    struct cntrl_parser;
    struct digit_parser;
    struct xdigit_parser;
    struct graph_parser;
    struct upper_parser;
    struct lower_parser;
    struct print_parser;
    struct punct_parser;
    struct blank_parser;
    struct space_parser;
    struct eol_parser;
    struct end_parser; 

    // non_terminal/parser_context.hpp
    template<typename T> struct parser_context;

    // non_terminal/parser_id.hpp
    class parser_id;
    template<int N> struct parser_tag;
    class dynamic_parser_tag;
    struct parser_address_tag;

    // non_terminal/rule.hpp
    template<typename T0, typename T1, typename T2> class rule; 

    // non_terminal/grammar.hpp
    template<class DerivedT, typename ContextT> struct grammar; 

    // composite.hpp
    template<class ParserT, typename ActionT> class action;
    template<class A, class B> struct alternative;
    template<class A, class B> struct difference;
    template<class A, class B> struct exclusive_or;
    template<class A, class B> struct intersection;
    template<class a, class b> struct sequence;
    template<class A, class B> struct sequential_or;
    template<class S> struct kleene_star;
    template<class S> struct positive;
    template<class S> struct optional;
    // composite/directives.hpp
    template<class ParserT> struct contiguous;
    template<class ParserT> struct inhibit_case;
    template<class BaseT> struct inhibit_case_iteration_policy;
    template<class A, class B> struct longest_alternative;
    template<class A, class B> struct shortest_alternative;
    template<class ParserT, typename BoundsT> struct min_bounded;
    template<class ParserT, typename BoundsT> struct max_bounded;
    template<class ParserT, typename BoundsT> struct bounded;
    // composite/no_actions.hpp
    template<class Parser> struct no_actions_parser;
    template<class Base> struct no_actions_action_policy;
    // composite/epsilon.hpp
    struct epsilon_parser;
    template<typename CondT, bool positive> struct condition_parser;
    template<typename SubjectT> struct empty_match_parser;
    template<typename SubjectT> struct negated_empty_match_parser;

    // deprecated assign/push_back actor -- they live somewhere else, now
    struct assign_action;
    struct push_back_action;
    template<typename T, typename ActionT> class ref_value_actor;
    template<typename T, typename ValueT, typename ActionT> 
    class ref_const_ref_actor;

}} // namespace boost::spirit



#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


// parser.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::parse_info,1)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::plain_parser_category)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::binary_parser_category)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::unary_parser_category)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::action_parser_category)


// nil.hpp (included directly)

#if !defined(BOOST_SPIRIT_NIL_T_TYPEOF_REGISTERED)
// registration guard to decouple the iterators from the core
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::nil_t)
#   define BOOST_SPIRIT_NIL_T_TYPEOF_REGISTERED
#endif

// match.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::match, 1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::match<boost::spirit::nil_t>)


// primitives/primitives.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::negated_char_parser, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::chlit, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::range, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::chseq, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::strlit, 1)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::nothing_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::anychar_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::alnum_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::alpha_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::cntrl_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::digit_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::xdigit_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::graph_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::upper_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::lower_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::print_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::punct_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::blank_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::space_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::eol_parser)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::end_parser)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::chlit<char>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::chlit<wchar_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::range<char>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::range<wchar_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::chseq<char const *>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::chseq<wchar_t const *>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::strlit<char const *>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::strlit<wchar_t const *>)


// primitives/numerics.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::int_parser, (class)(int)(unsigned)(int))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::uint_parser, (class)(int)(unsigned)(int))
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::sign_parser)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::real_parser, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::real_parser_policies, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ureal_parser_policies, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::strict_real_parser_policies, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::strict_ureal_parser_policies, 1)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::int_parser, (class)(int))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::uint_parser, (class)(int))
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::int_parser<boost::int32_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::uint_parser<boost::uint32_t>)
#if !defined(BOOST_NO_INT64_T)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::int_parser<boost::int64_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::uint_parser<boost::uint64_t>)
#endif
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::real_parser_policies<float>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::real_parser_policies<double>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::ureal_parser_policies<float>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::ureal_parser_policies<double>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::strict_real_parser_policies<float>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::strict_real_parser_policies<double>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::strict_ureal_parser_policies<float>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::strict_ureal_parser_policies<double>)


// scanner/scanner.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner_policies,3)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::iteration_policy)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::action_policy)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::match_policy)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner_policies,2)


// scanner/skipper.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::skipper_iteration_policy,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::no_skipper_iteration_policy,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::skip_parser_iteration_policy,2)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::skipper_iteration_policy<>)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::skip_parser_iteration_policy,1)


// non_terminal/parser_context.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::parser_context,1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::parser_context<boost::spirit::nil_t>)


// non_terminal/parser_id.hpp

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::parser_id)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::parser_tag, (int))
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::dynamic_parser_tag)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::parser_address_tag)


// non_terminal/subrule.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::subrule,(int)(class))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::subrule_parser,(int)(class)(class))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::subrule_list,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::subrules_scanner,2)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::subrule,(int))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::subrule_parser,(int)(class))
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<0>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<1>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<2>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<3>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<4>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<5>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<6>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::subrule<7>)


// non_terminal/rule.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::rule,3)
#if BOOST_SPIRIT_RULE_SCANNERTYPE_LIMIT > 1
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner_list,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::scanner_list,BOOST_SPIRIT_RULE_SCANNERTYPE_LIMIT)
#endif


// non_terminal/grammar.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::grammar,2)


// composite.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::action, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::alternative, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::difference, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::exclusive_or, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::intersection, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::sequence, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::sequential_or, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::kleene_star, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::positive, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::optional, 1)


// composite/directives.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::contiguous, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::inhibit_case, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::inhibit_case_iteration_policy,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::longest_alternative, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::shortest_alternative, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::min_bounded, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::max_bounded, 2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::bounded, 2)


// composite/no_actions.hpp

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::no_actions_parser, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::no_actions_action_policy, 1)

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::no_actions_action_policy<boost::spirit::action_policy>)


// composite/epsilon.hpp

BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::epsilon_parser)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::condition_parser, (class)(bool))
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::empty_match_parser, 1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::negated_empty_match_parser, 1)


#if !defined(BOOST_SPIRIT_ACTOR_TYPEOF_HPP)
// deprecated assign/push_back actor -- they live somewhere else, now
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_value_actor,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ref_const_ref_actor,3)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::assign_action)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::push_back_action)
#endif


#if BOOST_WORKAROUND(BOOST_MSVC,BOOST_TESTED_AT(1400)) && BOOST_MSVC >= 1400
namespace boost { namespace spirit {

    nil_t & operator* (nil_t);
    nil_t & operator+ (nil_t);

} } // namespace ::boost::spirit::type_of
#endif


#endif
 
