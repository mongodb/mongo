// Copyright (C) 2004 Peder Holt
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_TYPETRAITS_MSVC_TYPEOF_HPP
#define BOOST_TYPETRAITS_MSVC_TYPEOF_HPP

#include <boost/config.hpp>
#include <boost/detail/workaround.hpp>

namespace boost { namespace detail {
# if BOOST_WORKAROUND(BOOST_MSVC,==1300)
        template<typename ID>
        struct msvc_extract_type
        {
            template<bool>
            struct id2type_impl;

            typedef id2type_impl<true> id2type;
        };

        template<typename T, typename ID>
        struct msvc_register_type : public msvc_extract_type<ID>
        {
            template<>
            struct id2type_impl<true>  //VC7.0 specific bugfeature
            {
                typedef T type;
            };
        };
# else 
        template<typename ID>
        struct msvc_extract_type
        {
            struct id2type;
        };

        template<typename T, typename ID>
        struct msvc_register_type : public msvc_extract_type<ID>
        {
            typedef msvc_extract_type<ID> base_type;
            struct base_type::id2type // This uses nice VC6.5 and VC7.1 bugfeature
            {
                typedef T type;
            };
        };
# endif
}}

#endif //BOOST_TYPETRAITS_MSVC_TYPEOF_IMPL_HPP
