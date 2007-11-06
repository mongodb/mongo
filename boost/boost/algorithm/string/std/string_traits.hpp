//  Boost string_algo library string_traits.hpp header file  ---------------------------//

//  Copyright Pavol Droba 2002-2003. Use, modification and
//  distribution is subject to the Boost Software License, Version
//  1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#ifndef BOOST_STRING_STD_STRING_TRAITS_HPP
#define BOOST_STRING_STD_STRING_TRAITS_HPP

#include <boost/algorithm/string/yes_no_type.hpp>
#include <string>
#include <boost/algorithm/string/sequence_traits.hpp>

namespace boost {
    namespace algorithm {

//  std::basic_string<> traits  -----------------------------------------------//

#ifdef BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

        // native replace tester
        template<typename T, typename TraitsT, typename AllocT>
        yes_type has_native_replace_tester( const std::basic_string<T, TraitsT, AllocT>* );

#else // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

    // native replace trait
        template<typename T, typename TraitsT, typename AllocT>
        class has_native_replace< std::basic_string<T, TraitsT, AllocT> >
        {
        public:
#if BOOST_WORKAROUND( __IBMCPP__, <= 600 )
            enum { value = true } ;
#else
            BOOST_STATIC_CONSTANT(bool, value=true);
#endif // BOOST_WORKAROUND( __IBMCPP__, <= 600 )

        typedef mpl::bool_<has_native_replace<T>::value> type;
        };


#endif // BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION

    } // namespace algorithm
} // namespace boost


#endif  // BOOST_STRING_LIST_TRAITS_HPP
