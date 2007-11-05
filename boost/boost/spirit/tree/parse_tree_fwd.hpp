/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_TREE_PARSE_TREE_FWD_HPP)
#define BOOST_SPIRIT_TREE_PARSE_TREE_FWD_HPP

namespace boost { namespace spirit {

    template <typename MatchPolicyT, typename NodeFactoryT>
    struct pt_tree_policy;

    template <
        typename IteratorT,
        typename NodeFactoryT = node_val_data_factory<nil_t>
    >
    struct pt_match_policy;

    template <typename T>
    struct gen_pt_node_parser;

}} // namespace boost::spirit

#endif

