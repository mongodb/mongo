#ifndef BOOST_CORE_DETAIL_LWT_UNATTENDED_HPP_INCLUDED
#define BOOST_CORE_DETAIL_LWT_UNATTENDED_HPP_INCLUDED

// Copyright 2014, 2022 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <cstdlib>
#if defined(_MSC_VER) && defined(_CPPLIB_VER) && defined(_DEBUG)
# include <crtdbg.h>
#endif

namespace boost
{
namespace core
{
namespace detail
{

// Setup unattended mode by disabling interactive popups on
// assertion failures

inline void lwt_unattended()
{
#if defined(_MSC_VER) && (_MSC_VER > 1310)

    // disable message boxes on assert(), abort()
    ::_set_abort_behavior( 0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT );

# pragma warning(push)
# pragma warning(disable: 4996)

# if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
# endif

#if !defined(WINAPI_FAMILY) || WINAPI_FAMILY == 100 /*WINAPI_FAMILY_DESKTOP_APP*/

    // disable message box on crash
    ::_seterrormode( /*SEM_NOGPFAULTERRORBOX*/ 0x0002 );

#endif

# if defined(__clang__)
#  pragma clang diagnostic pop
# endif

# pragma warning(pop)

#endif

#if defined(_MSC_VER) && defined(_CPPLIB_VER) && defined(_DEBUG)

    // disable message boxes on iterator debugging violations
    _CrtSetReportMode( _CRT_ASSERT, _CRTDBG_MODE_FILE );
    _CrtSetReportFile( _CRT_ASSERT, _CRTDBG_FILE_STDERR );

#endif
}

} // namespace detail
} // namespace core
} // namespace boost

#endif // #ifndef BOOST_CORE_DETAIL_LWT_UNATTENDED_HPP_INCLUDED
