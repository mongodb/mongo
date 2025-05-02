//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_WIN32_LCID_HPP
#define BOOST_LOCALE_IMPL_WIN32_LCID_HPP

#include <boost/locale/config.hpp>
#include <string>

namespace boost { namespace locale { namespace impl_win {

    BOOST_LOCALE_DECL unsigned locale_to_lcid(const std::string& locale_name);

}}} // namespace boost::locale::impl_win

#endif
