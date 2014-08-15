/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// codecvt_null.cpp

// Copyright (c) 2004 Robert Ramey, Indiana University (garcia@osl.iu.edu)
// Andrew Lumsdaine, Indiana University (lums@osl.iu.edu). 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

#define BOOST_WARCHIVE_SOURCE
#include <boost/archive/codecvt_null.hpp>

// codecvt implementation for passing wchar_t objects to char output
// without any translation whatever.  Used to implement binary output
// of wchar_t objects.

namespace boost {
namespace archive {

BOOST_WARCHIVE_DECL(std::codecvt_base::result)
codecvt_null<wchar_t>::do_out(
    std::mbstate_t & /*state*/,
    const wchar_t * first1, 
    const wchar_t * last1,
    const wchar_t * & next1,
    char * first2, 
    char * last2, 
    char * & next2
) const {
    while(first1 != last1){
        // Per std::22.2.1.5.2/2, we can store no more that
        // last2-first2 characters. If we need to more encode
        // next internal char type, return 'partial'.
        if(static_cast<int>(sizeof(wchar_t)) > (last2 - first2)){
            next1 = first1;
            next2 = first2;
            return std::codecvt_base::partial;
        }
        * reinterpret_cast<wchar_t *>(first2) = * first1++;
        first2 += sizeof(wchar_t);

    }
    next1 = first1;
    next2 = first2;
    return std::codecvt_base::ok;
}

BOOST_WARCHIVE_DECL(std::codecvt_base::result)
codecvt_null<wchar_t>::do_in(
    std::mbstate_t & /*state*/,
    const char * first1, 
    const char * last1, 
    const char * & next1,
    wchar_t * first2,
    wchar_t * last2,
    wchar_t * & next2
) const {
    // Process input characters until we've run of them,
    // or the number of remaining characters is not
    // enough to construct another output character,
    // or we've run out of place for output characters.
    while(first2 != last2){
        // Have we converted all input characters? 
        // Return with 'ok', if so.
        if (first1 == last1)
             break;
        // Do we have less input characters than needed
        // for a single output character?        
        if(static_cast<int>(sizeof(wchar_t)) > (last1 - first1)){
            next1 = first1;
            next2 = first2;
            return std::codecvt_base::partial; 
        }
        *first2++ = * reinterpret_cast<const wchar_t *>(first1);
        first1 += sizeof(wchar_t);
    }
    next1 = first1;
    next2 = first2;
    return std::codecvt_base::ok;
}

} // namespace archive
} // namespace boost
