//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_STD_ALL_GENERATOR_HPP
#define BOOST_LOCALE_IMPL_STD_ALL_GENERATOR_HPP

#include <boost/locale/generator.hpp>
#include <locale>
#include <string>

namespace boost { namespace locale { namespace impl_std {
    /// UTF-8 support of the standard library for the requested locale
    enum class utf8_support {
        /// No UTF-8 requested or required (e.g. other narrow encoding)
        none,
        /// UTF-8 encoding supported by the std-locale
        native,
        /// UTF-8 encoding has to be emulated using wchar_t
        from_wide
    };

    std::locale
    create_convert(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf);

    std::locale
    create_collate(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf);

    std::locale
    create_formatting(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf);

    std::locale
    create_parsing(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf);

    std::locale
    create_codecvt(const std::locale& in, const std::string& locale_name, char_facet_t type, utf8_support utf);

}}} // namespace boost::locale::impl_std

#endif
