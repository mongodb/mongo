/*=============================================================================
    Copyright (c) 1998-2003 Joel de Guzman
    Copyright (c) 2001-2003 Daniel Nuffer
    Copyright (c) 2001-2003 Hartmut Kaiser
    Copyright (c) 2002-2003 Martin Wille
    Copyright (c) 2002 Raghavendra Satish
    Copyright (c) 2001 Bruce Florman
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_CORE_MAIN_HPP)
#define BOOST_SPIRIT_CORE_MAIN_HPP

#include <boost/spirit/version.hpp>
#include <boost/spirit/debug.hpp>

///////////////////////////////////////////////////////////////////////////////
//
//  Spirit.Core includes
//
///////////////////////////////////////////////////////////////////////////////

//  Spirit.Core.Kernel
#include <boost/spirit/core/config.hpp>
#include <boost/spirit/core/nil.hpp>
#include <boost/spirit/core/match.hpp>
#include <boost/spirit/core/parser.hpp>

//  Spirit.Core.Primitives
#include <boost/spirit/core/primitives/primitives.hpp>
#include <boost/spirit/core/primitives/numerics.hpp>

//  Spirit.Core.Scanner
#include <boost/spirit/core/scanner/scanner.hpp>
#include <boost/spirit/core/scanner/skipper.hpp>

//  Spirit.Core.NonTerminal
#include <boost/spirit/core/non_terminal/subrule.hpp>
#include <boost/spirit/core/non_terminal/rule.hpp>
#include <boost/spirit/core/non_terminal/grammar.hpp>

//  Spirit.Core.Composite
#include <boost/spirit/core/composite/actions.hpp>
#include <boost/spirit/core/composite/composite.hpp>
#include <boost/spirit/core/composite/directives.hpp>
#include <boost/spirit/core/composite/epsilon.hpp>
#include <boost/spirit/core/composite/sequence.hpp>
#include <boost/spirit/core/composite/sequential_and.hpp>
#include <boost/spirit/core/composite/sequential_or.hpp>
#include <boost/spirit/core/composite/alternative.hpp>
#include <boost/spirit/core/composite/difference.hpp>
#include <boost/spirit/core/composite/intersection.hpp>
#include <boost/spirit/core/composite/exclusive_or.hpp>
#include <boost/spirit/core/composite/kleene_star.hpp>
#include <boost/spirit/core/composite/positive.hpp>
#include <boost/spirit/core/composite/optional.hpp>
#include <boost/spirit/core/composite/list.hpp>
#include <boost/spirit/core/composite/no_actions.hpp>

//  Deprecated interface includes
#include <boost/spirit/actor/assign_actor.hpp>
#include <boost/spirit/actor/push_back_actor.hpp>

#if defined(BOOST_SPIRIT_DEBUG)
    //////////////////////////////////
    #include <boost/spirit/debug/parser_names.hpp>

#endif // BOOST_SPIRIT_DEBUG

#endif // BOOST_SPIRIT_CORE_MAIN_HPP

