/*=============================================================================
    Copyright (c) 2006 Tobias Schwinger
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_SUBRULE_FWD_HPP)
#define BOOST_SPIRIT_SUBRULE_FWD_HPP

#include <boost/spirit/core/non_terminal/parser_context.hpp>

namespace boost { namespace spirit  {

    template <int ID, typename ContextT = parser_context<> >
    struct subrule; 

    template <int ID, typename DefT, typename ContextT = parser_context<> >
    struct subrule_parser;

    template <typename ScannerT, typename ListT>
    struct subrules_scanner;

    template <typename FirstT, typename RestT>
    struct subrule_list; 

}} // namespace boost::spirit

#endif

