//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_ALL_GENERATOR_HPP
#define BOOST_LOCALE_IMPL_ALL_GENERATOR_HPP

#include <boost/locale/generator.hpp>

namespace boost {
    namespace locale {
        namespace impl_icu {
            struct cdata;
            std::locale create_convert(std::locale const &,cdata const &,character_facet_type); // ok
            std::locale create_collate(std::locale const &,cdata const &,character_facet_type); // ok 
            std::locale create_formatting(std::locale const &,cdata const &,character_facet_type); // ok
            std::locale create_parsing(std::locale const &,cdata const &,character_facet_type);  // ok
            std::locale create_codecvt(std::locale const &,std::string const &encoding,character_facet_type); // ok
            std::locale create_boundary(std::locale const &,cdata const &,character_facet_type); // ok
            std::locale create_calendar(std::locale const &,cdata const &); // ok

        }
    }
}

#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
