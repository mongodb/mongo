///////////////////////////////////////////////////////////////////////////////
// domain_tags.hpp
//
//  Copyright 2004 Eric Niebler. Distributed under the Boost
//  Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_DOMAIN_TAGS_HPP_EAN_10_04_2005
#define BOOST_XPRESSIVE_DETAIL_STATIC_PRODUCTIONS_DOMAIN_TAGS_HPP_EAN_10_04_2005

namespace boost { namespace xpressive { namespace detail
{

    ///////////////////////////////////////////////////////////////////////////////
    // regex domain tags
    struct seq_tag {};
    struct alt_tag {};
    struct lst_tag {};
    struct set_tag {};
    struct ind_tag {};

}}}

#endif
