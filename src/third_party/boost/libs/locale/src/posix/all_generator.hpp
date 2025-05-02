//
// Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_IMPL_POSIX_ALL_GENERATOR_HPP
#define BOOST_LOCALE_IMPL_POSIX_ALL_GENERATOR_HPP

#include <boost/locale/generator.hpp>
#include <clocale>
#include <locale>
#include <memory>

#ifdef __APPLE__
#    include <xlocale.h>
#endif

namespace boost { namespace locale { namespace impl_posix {

    std::locale create_convert(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type);

    std::locale create_collate(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type);

    std::locale create_formatting(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type);

    std::locale create_parsing(const std::locale& in, std::shared_ptr<locale_t> lc, char_facet_t type);
    std::locale create_codecvt(const std::locale& in, const std::string& encoding, char_facet_t type);

}}} // namespace boost::locale::impl_posix

#endif
