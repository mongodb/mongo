/*=============================================================================
    Copyright (c) 1998-2003 Joel de Guzman
    Copyright (c) 2001 Daniel Nuffer
    Copyright (c) 2002 Hartmut Kaiser
    http://spirit.sourceforge.net/

    Use, modification and distribution is subject to the Boost Software
    License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
    http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#if !defined(BOOST_SPIRIT_OPTIONAL_IPP)
#define BOOST_SPIRIT_OPTIONAL_IPP

namespace boost { namespace spirit {

    ///////////////////////////////////////////////////////////////////////////
    //
    //  optional class implementation
    //
    ///////////////////////////////////////////////////////////////////////////
    template <typename S>
    optional<S>
    operator!(parser<S> const& a)
    {
        return optional<S>(a.derived());
    }

}} // namespace boost::spirit

#endif
