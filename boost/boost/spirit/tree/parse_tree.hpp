/*=============================================================================
    Copyright (c) 2001-2003 Daniel Nuffer
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef BOOST_SPIRIT_TREE_PARSE_TREE_HPP
#define BOOST_SPIRIT_TREE_PARSE_TREE_HPP

#include <boost/spirit/tree/common.hpp>
#include <boost/spirit/core/scanner/scanner.hpp>

#include <boost/spirit/tree/parse_tree_fwd.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace spirit {


//////////////////////////////////
// pt_match_policy is simply an id so the correct specialization of tree_policy can be found.
template <
    typename IteratorT,
    typename NodeFactoryT 
>
struct pt_match_policy :
    public common_tree_match_policy<
        pt_match_policy<IteratorT, NodeFactoryT>,
        IteratorT,
        NodeFactoryT,
        pt_tree_policy<
            pt_match_policy<IteratorT, NodeFactoryT>,
            NodeFactoryT
        >
    >
{
    typedef
        common_tree_match_policy<
            pt_match_policy<IteratorT, NodeFactoryT>,
            IteratorT,
            NodeFactoryT,
            pt_tree_policy<
                pt_match_policy<IteratorT, NodeFactoryT>,
                NodeFactoryT
            >
        >
    common_tree_match_policy_;

    pt_match_policy()
    {
    }

    template <typename PolicyT>
    pt_match_policy(PolicyT const & policies)
        : common_tree_match_policy_(policies)
    {
    }
};

//////////////////////////////////
template <typename MatchPolicyT, typename NodeFactoryT>
struct pt_tree_policy :
    public common_tree_tree_policy<MatchPolicyT, NodeFactoryT>
{
    typedef
        typename common_tree_tree_policy<MatchPolicyT, NodeFactoryT>::match_t
        match_t;
    typedef typename MatchPolicyT::iterator_t iterator_t;

    static void concat(match_t& a, match_t const& b)
    {
        typedef typename match_t::attr_t attr_t;
        BOOST_SPIRIT_ASSERT(a && b);

        std::copy(b.trees.begin(), b.trees.end(),
            std::back_insert_iterator<typename match_t::container_t>(a.trees));
    }

    template <typename MatchT, typename Iterator1T, typename Iterator2T>
    static void group_match(MatchT& m, parser_id const& id,
            Iterator1T const& first, Iterator2T const& last)
    {
        if (!m)
            return;

        typedef typename NodeFactoryT::template factory<iterator_t> factory_t;
        typedef typename tree_match<iterator_t, NodeFactoryT>::container_t
            container_t;
        typedef typename container_t::iterator cont_iterator_t;

        match_t newmatch(m.length(),
                factory_t::create_node(first, last, false));

        std::swap(newmatch.trees.begin()->children, m.trees);
        // set this node and all it's unset children's rule_id
        newmatch.trees.begin()->value.id(id);
        for (cont_iterator_t i = newmatch.trees.begin()->children.begin();
                i != newmatch.trees.begin()->children.end();
                ++i)
        {
            if (i->value.id() == 0)
                i->value.id(id);
        }
        m = newmatch;
    }

    template <typename FunctorT>
    static void apply_op_to_match(FunctorT const& op, match_t& m)
    {
        op(m);
    }
};

namespace impl {

    template <typename IteratorT, typename NodeFactoryT>
    struct tree_policy_selector<pt_match_policy<IteratorT, NodeFactoryT> >
    {
        typedef pt_tree_policy<
            pt_match_policy<IteratorT, NodeFactoryT>, NodeFactoryT> type;
    };

} // namespace impl


//////////////////////////////////
struct gen_pt_node_parser_gen;

template <typename T>
struct gen_pt_node_parser
:   public unary<T, parser<gen_pt_node_parser<T> > >
{
    typedef gen_pt_node_parser<T> self_t;
    typedef gen_pt_node_parser_gen parser_generator_t;
    typedef unary_parser_category parser_category_t;
//    typedef gen_pt_node_parser<T> const &embed_t;

    gen_pt_node_parser(T const& a)
    : unary<T, parser<gen_pt_node_parser<T> > >(a) {}

    template <typename ScannerT>
    typename parser_result<self_t, ScannerT>::type
    parse(ScannerT const& scan) const
    {
        typedef typename ScannerT::iteration_policy_t iteration_policy_t;
        typedef typename ScannerT::match_policy_t::iterator_t iterator_t;
        typedef typename ScannerT::match_policy_t::factory_t factory_t;
        typedef pt_match_policy<iterator_t, factory_t> match_policy_t;
        typedef typename ScannerT::action_policy_t action_policy_t;
        typedef scanner_policies<
            iteration_policy_t,
            match_policy_t,
            action_policy_t
        > policies_t;

        return this->subject().parse(scan.change_policies(policies_t(scan)));
    }
};

//////////////////////////////////
struct gen_pt_node_parser_gen
{
    template <typename T>
    struct result {

        typedef gen_pt_node_parser<T> type;
    };

    template <typename T>
    static gen_pt_node_parser<T>
    generate(parser<T> const& s)
    {
        return gen_pt_node_parser<T>(s.derived());
    }

    template <typename T>
    gen_pt_node_parser<T>
    operator[](parser<T> const& s) const
    {
        return gen_pt_node_parser<T>(s.derived());
    }
};

//////////////////////////////////
const gen_pt_node_parser_gen gen_pt_node_d = gen_pt_node_parser_gen();


///////////////////////////////////////////////////////////////////////////////
//
//  Parse functions for parse trees
//
///////////////////////////////////////////////////////////////////////////////
template <
    typename NodeFactoryT, typename IteratorT, typename ParserT, 
    typename SkipT
>
inline tree_parse_info<IteratorT, NodeFactoryT>
pt_parse(
    IteratorT const&        first_,
    IteratorT const&        last,
    parser<ParserT> const&  p,
    SkipT const&            skip,
    NodeFactoryT const&   /*dummy_*/ = NodeFactoryT())
{
    typedef skip_parser_iteration_policy<SkipT> iter_policy_t;
    typedef pt_match_policy<IteratorT, NodeFactoryT> pt_match_policy_t;
    typedef
        scanner_policies<iter_policy_t, pt_match_policy_t>
        scanner_policies_t;
    typedef scanner<IteratorT, scanner_policies_t> scanner_t;

    iter_policy_t iter_policy(skip);
    scanner_policies_t policies(iter_policy);
    IteratorT first = first_;
    scanner_t scan(first, last, policies);
    tree_match<IteratorT, NodeFactoryT> hit = p.derived().parse(scan);
    return tree_parse_info<IteratorT, NodeFactoryT>(
        first, hit, hit && (first == last), hit.length(), hit.trees);
}

template <typename IteratorT, typename ParserT, typename SkipT>
inline tree_parse_info<IteratorT>
pt_parse(
    IteratorT const&        first,
    IteratorT const&        last,
    parser<ParserT> const&  p,
    SkipT const&            skip)
{
    typedef node_val_data_factory<nil_t> default_node_factory_t;
    return pt_parse(first, last, p, skip, default_node_factory_t());
}

//////////////////////////////////
template <typename IteratorT, typename ParserT>
inline tree_parse_info<IteratorT>
pt_parse(
    IteratorT const&        first_,
    IteratorT const&        last,
    parser<ParserT> const&  parser)
{
    typedef pt_match_policy<IteratorT> pt_match_policy_t;
    IteratorT first = first_;
    scanner<
        IteratorT,
        scanner_policies<iteration_policy, pt_match_policy_t>
    > scan(first, last);
    tree_match<IteratorT> hit = parser.derived().parse(scan);
    return tree_parse_info<IteratorT>(
        first, hit, hit && (first == last), hit.length(), hit.trees);
}

//////////////////////////////////
template <typename CharT, typename ParserT, typename SkipT>
inline tree_parse_info<CharT const*>
pt_parse(
    CharT const*            str,
    parser<ParserT> const&  p,
    SkipT const&            skip)
{
    CharT const* last = str;
    while (*last)
        last++;
    return pt_parse(str, last, p, skip);
}

//////////////////////////////////
template <typename CharT, typename ParserT>
inline tree_parse_info<CharT const*>
pt_parse(
    CharT const*            str,
    parser<ParserT> const&  parser)
{
    CharT const* last = str;
    while (*last)
    {
        last++;
    }
    return pt_parse(str, last, parser);
}

///////////////////////////////////////////////////////////////////////////////
}} // namespace boost::spirit

#endif

