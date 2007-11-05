#ifndef BOOST_ARCHIVE_CODECVT_NULL_HPP
#define BOOST_ARCHIVE_CODECVT_NULL_HPP

// MS compatible compilers support #pragma once
#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// codecvt_null.hpp:

// (C) Copyright 2004 Robert Ramey - http://www.rrsd.com .
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#include <locale>
#include <cstddef>

#include <boost/config.hpp>

namespace std{
    #if defined(__LIBCOMO__)
        using ::mbstate_t;
    #elif defined(__QNXNTO__)
        using std::mbstate_t;
    #elif defined(BOOST_DINKUMWARE_STDLIB) && BOOST_DINKUMWARE_STDLIB == 1
        using ::mbstate_t;
    #elif defined(__SGI_STL_PORT)
    #elif defined(BOOST_NO_STDC_NAMESPACE)
        using ::codecvt;
        using ::mbstate_t;
    #endif
} // namespace std

namespace boost {
namespace archive {

template<class Ch>
class codecvt_null;

template<>
class codecvt_null<char> : public std::codecvt<char, char, std::mbstate_t>
{
    virtual bool do_always_noconv() const throw() {
        return true;
    }
public:
    explicit codecvt_null(std::size_t no_locale_manage = 0) :
        std::codecvt<char, char, std::mbstate_t>(no_locale_manage)
    {}
};

template<>
class codecvt_null<wchar_t> : public std::codecvt<wchar_t, char, std::mbstate_t>
{
    virtual std::codecvt_base::result
    do_out(
        std::mbstate_t & state,
        const wchar_t * first1,
        const wchar_t * last1,
        const wchar_t * & next1,
        char * first2,
        char * last2,
        char * & next2
    ) const;
    virtual std::codecvt_base::result
    do_in(
        std::mbstate_t & state,
        const char * first1,
        const char * last1,
        const char * & next1,
        wchar_t * first2,
        wchar_t * last2,
        wchar_t * & next2
    ) const;
    virtual int do_encoding( ) const throw( ){
        return sizeof(wchar_t) / sizeof(char);
    }
    virtual int do_max_length( ) const throw( ){
        return do_encoding();
    }
};

} // namespace archive
} // namespace boost

#endif //BOOST_ARCHIVE_CODECVT_NULL_HPP
