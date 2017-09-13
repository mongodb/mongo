/////////1/////////2/////////3/////////4/////////5/////////6/////////7/////////8
// stl_port.cpp: implementation of run-time casting of void pointers

// (C) Copyright 2005 Robert Ramey - http://www.rrsd.com . 
// Use, modification and distribution is subject to the Boost Software
// License, Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for updates, documentation, and revision history.

#if (defined _MSC_VER) && (_MSC_VER == 1200)
# pragma warning (disable : 4786) // too long name, harmless warning
#endif

// this befuddles the msvc 6 compiler so we can't use it
#if ! ((defined _MSC_VER) && (_MSC_VER <= 1300)) 

#include <boost/config.hpp>

#if defined(__SGI_STL_PORT) && (__SGI_STL_PORT < 0x500)

#include <boost/archive/codecvt_null.hpp>

// explicit instantiation

namespace std {

template
locale::locale(
    const locale& __loc, boost::archive::codecvt_null<char> * __f
);

template
locale::locale(
    const locale& __loc, boost::archive::codecvt_null<wchar_t> * __f
);

} // namespace std

#endif

#endif
