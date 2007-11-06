/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_TREE_TYPEOF_HPP)
#define BOOST_SPIRIT_TREE_TYPEOF_HPP

#include <boost/typeof/typeof.hpp>

#include <boost/spirit/core/typeof.hpp>

#include <boost/spirit/tree/common_fwd.hpp>
#include <boost/spirit/tree/parse_tree_fwd.hpp>
#include <boost/spirit/tree/ast_fwd.hpp>


#include BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()


// common.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_node,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::node_iter_data,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::node_iter_data_factory,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::node_val_data_factory,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::node_all_val_data_factory,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_match,3)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::tree_policy)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::common_tree_match_policy,4)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::common_tree_tree_policy,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::no_tree_gen_node_parser,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::node_parser,2)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::discard_node_op)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::leaf_node_op)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::infix_node_op)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::discard_first_node_op)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::discard_last_node_op)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::inner_node_op)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::action_directive_parser,2)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::access_match_action)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::access_match_action::action,2)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::access_node_action)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::access_node_action::action,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_parse_info,3)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::node_iter_data,1)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::node_iter_data_factory<boost::spirit::nil_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::node_val_data_factory<boost::spirit::nil_t>)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::node_all_val_data_factory<boost::spirit::nil_t>)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_match,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_match,1)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_parse_info,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::tree_parse_info,1)


// parse_tree.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::pt_tree_policy,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::pt_match_policy,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::gen_pt_node_parser,1)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::pt_match_policy,1)


// ast.hpp (has forward header)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ast_tree_policy,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ast_match_policy,2)
BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::gen_ast_node_parser,1)
BOOST_TYPEOF_REGISTER_TYPE(boost::spirit::root_node_op)

BOOST_TYPEOF_REGISTER_TEMPLATE(boost::spirit::ast_match_policy,1)


#endif

