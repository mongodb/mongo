//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_UTIL_LOCALE_DATA_HPP
#define BOOST_LOCALE_IMPL_UTIL_LOCALE_DATA_HPP

#include <string>

namespace boost {
    namespace locale {
        namespace util {
            
            class locale_data {
            public:
                locale_data() : 
                    language("C"),
                    encoding("us-ascii"),
                    utf8(false)
                {
                }

                std::string language;
                std::string country;
                std::string variant;
                std::string encoding;
                bool utf8;

                void parse(std::string const &locale_name);

            private:

                void parse_from_lang(std::string const &locale_name);
                void parse_from_country(std::string const &locale_name);
                void parse_from_encoding(std::string const &locale_name);
                void parse_from_variant(std::string const &locale_name);
            };

        } // util
    } // locale 
} // boost

#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4
