//  Copyright (c) 2001-2011 Hartmut Kaiser
//  Copyright (c) 2001-2011 Joel de Guzman
// 
//  Distributed under the Boost Software License, Version 1.0. (See accompanying 
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_SPIRIT_KARMA_DETAIL_FAIL_FUNCTION_HPP
#define BOOST_SPIRIT_KARMA_DETAIL_FAIL_FUNCTION_HPP

#if defined(_MSC_VER)
#pragma once
#endif

#include <boost/spirit/home/support/unused.hpp>
#include <boost/config.hpp>

namespace boost { namespace spirit { namespace karma { namespace detail
{
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4512) // assignment operator could not be generated.
#endif
    template <typename OutputIterator, typename Context, typename Delimiter>
    struct fail_function
    {
        typedef Context context_type;

        fail_function(OutputIterator& sink_, Context& context_
            , Delimiter const& delim_)
          : sink(sink_), ctx(context_), delim(delim_) 
        {}

        template <typename Component, typename Attribute>
        bool operator()(Component const& component, Attribute const& attr) const
        {
#if BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1600))  
            (void)component; // suppresses warning: C4100: 'component' : unreferenced formal parameter
#endif
            // return true if any of the generators fail
            return !component.generate(sink, ctx, delim, attr);
        }

        template <typename Component>
        bool operator()(Component const& component) const
        {
#if BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1600))  
            (void)component; // suppresses warning: C4100: 'component' : unreferenced formal parameter
#endif
            // return true if any of the generators fail
            return !component.generate(sink, ctx, delim, unused);
        }

        OutputIterator& sink;
        Context& ctx;
        Delimiter const& delim;
    };
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

}}}}

#endif
