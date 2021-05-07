//
//  Copyright (c) 2009-2011 Artyom Beilis (Tonkikh)
//
//  Distributed under the Boost Software License, Version 1.0. (See
//  accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
#ifndef BOOST_LOCALE_IMPL_ICU_LOCALIZATION_BACKEND_HPP
#define BOOST_LOCALE_IMPL_ICU_LOCALIZATION_BACKEND_HPP
namespace boost {
    namespace locale {
        class localization_backend;
        namespace impl_icu { 
            localization_backend *create_localization_backend();
        } // impl_icu
    } // locale 
} // boost
#endif
// vim: tabstop=4 expandtab shiftwidth=4 softtabstop=4 

