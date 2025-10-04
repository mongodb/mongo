/*=============================================================================
    Copyright (c) 2001-2011 Joel de Guzman

    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef BOOST_SPIRIT_SUPPORT_DETAIL_WHAT_FUNCTION_HPP
#define BOOST_SPIRIT_SUPPORT_DETAIL_WHAT_FUNCTION_HPP

#if defined(_MSC_VER)
#pragma once
#endif

#include <string>
#include <boost/spirit/home/support/info.hpp>
#include <boost/detail/workaround.hpp>

namespace boost { namespace spirit { namespace detail
{
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4512) // assignment operator could not be generated.
#endif
    template <typename Context>
    struct what_function
    {
        what_function(info& what_, Context& context_)
          : what(what_), context(context_)
        {
            what.value = std::list<info>();
        }

        template <typename Component>
        void operator()(Component const& component) const
        {
#if BOOST_WORKAROUND(BOOST_MSVC, BOOST_TESTED_AT(1600))
            (void)component; // suppresses warning: C4100: 'component' : unreferenced formal parameter
#endif
            boost::get<std::list<info> >(what.value).
                push_back(component.what(context));
        }

        info& what;
        Context& context;
    };
#ifdef _MSC_VER
#  pragma warning(pop)
#endif
}}}

#endif
