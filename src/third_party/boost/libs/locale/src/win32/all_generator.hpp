//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_WIN32_ALL_GENERATOR_HPP
#define BOOST_LOCALE_IMPL_WIN32_ALL_GENERATOR_HPP

#include <boost/locale/generator.hpp>
#include <locale>

namespace boost { namespace locale { namespace impl_win {

    struct winlocale;

    std::locale create_convert(const std::locale& in, const winlocale& lc, char_facet_t type);

    std::locale create_collate(const std::locale& in, const winlocale& lc, char_facet_t type);

    std::locale create_formatting(const std::locale& in, const winlocale& lc, char_facet_t type);

    std::locale create_parsing(const std::locale& in, const winlocale& lc, char_facet_t type);

}}} // namespace boost::locale::impl_win

#endif
