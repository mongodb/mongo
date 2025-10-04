//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_ALL_GENERATOR_HPP
#define BOOST_LOCALE_IMPL_ALL_GENERATOR_HPP

#include <boost/locale/generator.hpp>

namespace boost { namespace locale { namespace impl_icu {
    class cdata;
    std::locale create_convert(const std::locale&, const cdata&, char_facet_t);
    std::locale create_collate(const std::locale&, const cdata&, char_facet_t);
    std::locale create_formatting(const std::locale&, const cdata&, char_facet_t);
    std::locale create_parsing(const std::locale&, const cdata&, char_facet_t);
    std::locale create_codecvt(const std::locale&, const std::string& encoding, char_facet_t);
    std::locale create_boundary(const std::locale&, const cdata&, char_facet_t);
    std::locale create_calendar(const std::locale&, const cdata&);

}}} // namespace boost::locale::impl_icu

#endif
