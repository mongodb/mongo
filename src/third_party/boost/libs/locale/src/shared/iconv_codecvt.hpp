//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_ICONV_CODECVT_HPP
#define BOOST_LOCALE_ICONV_CODECVT_HPP

#include <boost/locale/util.hpp>
#include <memory>
#include <string>

namespace boost { namespace locale {
    BOOST_LOCALE_DECL
    std::unique_ptr<util::base_converter> create_iconv_converter(const std::string& encoding);

}} // namespace boost::locale

#endif
