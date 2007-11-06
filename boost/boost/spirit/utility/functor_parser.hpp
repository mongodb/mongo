/*=============================================================================
    Copyright (c) 2002-2003 Joel de Guzman
    Copyright (c) 2002-2003 Juan Carlos Arevalo-Baeza
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef BOOST_SPIRIT_FUNCTOR_PARSER_HPP
#define BOOST_SPIRIT_FUNCTOR_PARSER_HPP

///////////////////////////////////////////////////////////////////////////////
#include <boost/spirit/core/parser.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace spirit {

    ///////////////////////////////////////////////////////////////////////////
    //
    //  functor_parser class
    //
    //      Once a functor parser has been defined, you can build a real
    //      parser from it by passing it to this class as the template
    //      parameter.
    //
    ///////////////////////////////////////////////////////////////////////////
    template < class FunctorT >
    struct functor_parser : public parser<functor_parser<FunctorT> >
    {
        FunctorT functor;

        functor_parser(): functor() {}
        functor_parser(FunctorT const& functor_): functor(functor_) {}

        typedef typename FunctorT::result_t functor_result_t;
        typedef functor_parser<FunctorT> self_t;

        template <typename ScannerT>
        struct result
        {
            typedef typename match_result<ScannerT, functor_result_t>::type
            type;
        };

        template <typename ScannerT>
        typename parser_result<self_t, ScannerT>::type
        parse(ScannerT const& scan) const
        {
            typedef typename parser_result<self_t, ScannerT>::type result_t;
            typedef typename ScannerT::value_t      value_t;
            typedef typename ScannerT::iterator_t   iterator_t;

            iterator_t const s(scan.first);
            functor_result_t result;
            std::ptrdiff_t len = functor(scan, result);

            if (len < 0)
                return scan.no_match();
            else
                return scan.create_match(std::size_t(len), result, s, scan.first);
        }
    };

}} // namespace boost::spirit

#endif
