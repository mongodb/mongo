//  GetLastError.hpp  --------------------------------------------------------------//

//  Copyright 2010 Vicente J. Botet Escriba

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt


#ifndef BOOST_DETAIL_WINAPI_GETLASTERROR_HPP
#define BOOST_DETAIL_WINAPI_GETLASTERROR_HPP

#include <boost/detail/winapi/basic_types.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace detail {
namespace winapi {
#if defined( BOOST_USE_WINDOWS_H )
    using ::GetLastError;
#else
    extern "C" __declspec(dllimport) DWORD_ WINAPI
        GetLastError();
#endif
}
}
}

#endif // BOOST_DETAIL_WINAPI_GETLASTERROR_HPP
