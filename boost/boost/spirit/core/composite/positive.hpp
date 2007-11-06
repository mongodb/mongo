/*=============================================================================
    Copyright (c) 1998-2003 Joel de Guzman
    Copyright (c) 2001 Daniel Nuffer
    Copyright (c) 2002 Hartmut Kaiser
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_POSITIVE_HPP)
#define BOOST_SPIRIT_POSITIVE_HPP

#include <boost/spirit/core/parser.hpp>
#include <boost/spirit/core/primitives/primitives.hpp>
#include <boost/spirit/core/composite/composite.hpp>
#include <boost/spirit/meta/as_parser.hpp>

namespace boost { namespace spirit {

    ///////////////////////////////////////////////////////////////////////////
    //
    //  positive class
    //
    //      Handles expressions of the form:
    //
    //          +a
    //
    //      where a is a parser. The expression returns a composite
    //      parser that matches its subject one (1) or more times.
    //
    ///////////////////////////////////////////////////////////////////////////
    struct positive_parser_gen;
    
    template <typename S>
    struct positive
    :   public unary<S, parser<positive<S> > >
    {
        typedef positive<S>                 self_t;
        typedef unary_parser_category       parser_category_t;
        typedef positive_parser_gen         parser_generator_t;
        typedef unary<S, parser<self_t> >   base_t;
    
        positive(S const& a)
        : base_t(a) {}
    
        template <typename ScannerT>
        typename parser_result<self_t, ScannerT>::type
        parse(ScannerT const& scan) const
        {
            typedef typename parser_result<self_t, ScannerT>::type result_t;
            typedef typename ScannerT::iterator_t iterator_t;
            result_t hit = this->subject().parse(scan);
    
            if (hit)
            {
                for (;;)
                {
                    iterator_t save = scan.first;
                    if (result_t next = this->subject().parse(scan))
                    {
                        scan.concat_match(hit, next);
                    }
                    else
                    {
                        scan.first = save;
                        break;
                    }
                }
            }
            return hit;
        }
    };
    
    struct positive_parser_gen
    {
        template <typename S>
        struct result 
        {
            typedef positive<S> type;
        };
    
        template <typename S>
        static positive<S>
        generate(parser<S> const& a)
        {
            return positive<S>(a.derived());
        }
    };
    
    template <typename S>
    inline positive<S>
    operator+(parser<S> const& a);

}} // namespace boost::spirit

#endif

#include <boost/spirit/core/composite/impl/positive.ipp>
