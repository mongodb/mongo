// Boost.Units - A C++ library for zero-overhead dimensional analysis and 
// unit/quantity manipulation and conversion
//
// Copyright (C) 2003-2008 Matthias Christian Schabel
// Copyright (C) 2008 Steven Watanabe
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_UNITS_UTILITY_HPP
#define BOOST_UNITS_UTILITY_HPP

#include <cstdlib>
#include <typeinfo>
#include <string>

#if defined(__GLIBCXX__) || defined(__GLIBCPP__)
#define BOOST_UNITS_USE_DEMANGLING
#include <cxxabi.h>
#endif // __GNUC__

#ifdef BOOST_UNITS_USE_DEMANGLING

#include <boost/algorithm/string/replace.hpp>

namespace boost {

namespace units {

namespace detail {

inline
std::string
demangle(const char* name)
{
    // need to demangle C++ symbols
    char*       realname;
    std::size_t len; 
    int         stat;
     
    realname = abi::__cxa_demangle(name,NULL,&len,&stat);
    
    if (realname != NULL)
    {
        std::string   out(realname);
        
        std::free(realname);
        
        boost::replace_all(out,"boost::units::","");
        
        return out;
    }
    
    return std::string("demangle :: error - unable to demangle specified symbol");
}

} // namespace detail

template<class L>
std::string simplify_typename(const L& /*source*/)
{
    const std::string   demangled = detail::demangle(typeid(L).name());

    return demangled;
}

} // namespace units

} // namespace boost

#else // BOOST_UNITS_USE_DEMANGLING

namespace boost {

namespace units {

namespace detail {

inline
std::string
demangle(const char* name)
{
    return name;
}

} // namespace detail

template<class L>
std::string simplify_typename(const L& /*source*/)
{
    return std::string(typeid(L).name());
}

} // namespace units

} // namespace boost

// To get system-specific predefined macros:
// gcc -arch ppc -dM -E - < /dev/null | sort 

#endif // BOOST_UNITS_USE_DEMANGLING

#endif // BOOST_UNITS_UTILITY_HPP
