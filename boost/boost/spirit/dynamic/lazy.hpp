/*=============================================================================
    Copyright (c) 2003 Joel de Guzman
    Copyright (c) 2003 Vaclav Vesely
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef BOOST_SPIRIT_LAZY_HPP
#define BOOST_SPIRIT_LAZY_HPP

////////////////////////////////////////////////////////////////////////////////
#include <boost/spirit/core/parser.hpp>
#include <boost/spirit/phoenix/actor.hpp>

////////////////////////////////////////////////////////////////////////////////

namespace boost { namespace spirit
{
    ////////////////////////////////////////////////////////////////////////////
    //
    // lazy_parser, holds phoenix actor which returns a spirit parser.
    //
    ////////////////////////////////////////////////////////////////////////////

    template<class ActorT>
    struct lazy_parser : parser<lazy_parser<ActorT> >
    {
        typedef lazy_parser<ActorT> self_t;
        typedef typename phoenix::actor_result<
            ActorT, phoenix::tuple<> >::plain_type actor_result_t;

        template<typename ScannerT>
        struct result
        {
            typedef typename
                parser_result<actor_result_t, ScannerT>::type
            type;
        };

        lazy_parser(ActorT const& actor_)
        : actor(actor_) {}

        template<typename ScannerT>
        typename parser_result<self_t, ScannerT>::type
        parse(ScannerT const& scan) const
        { return actor().parse(scan); }

        ActorT actor;
    };

    //////////////////////////////
    // lazy_p, returns lazy_parser
    // Usage: lazy_p(actor)
    template<class ActorT>
    lazy_parser<ActorT> lazy_p(ActorT const& actor)
    { return lazy_parser<ActorT>(actor); }

}} // namespace boost::spirit

#endif // BOOST_SPIRIT_LAZY_HPP
