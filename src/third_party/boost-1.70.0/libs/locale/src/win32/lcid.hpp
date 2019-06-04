//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_WIN32_LCID_HPP
#define BOOST_LOCALE_IMPL_WIN32_LCID_HPP

#include <string>
#include <boost/locale/config.hpp>

namespace boost {
    namespace locale {
        namespace impl_win {

            BOOST_LOCALE_DECL unsigned locale_to_lcid(std::string const &locale_name);

        } // impl_win
    } // locale
} // boost

#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
