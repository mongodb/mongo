#ifndef BOOST_SMART_PTR_DETAIL_SP_NOEXCEPT_HPP_INCLUDED
#define BOOST_SMART_PTR_DETAIL_SP_NOEXCEPT_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//  detail/sp_noexcept.hpp
//
//  Copyright 2016, 2017 Peter Dimov
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt


// BOOST_SP_NOEXCEPT (obsolete, only retained for compatibility)

#define BOOST_SP_NOEXCEPT noexcept

// BOOST_SP_NOEXCEPT_WITH_ASSERT (noexcept, unless a user assertion handler is present)

#if defined(BOOST_DISABLE_ASSERTS) || ( defined(BOOST_ENABLE_ASSERT_DEBUG_HANDLER) && defined(NDEBUG) )

#  define BOOST_SP_NOEXCEPT_WITH_ASSERT noexcept

#elif defined(BOOST_ENABLE_ASSERT_HANDLER) || ( defined(BOOST_ENABLE_ASSERT_DEBUG_HANDLER) && !defined(NDEBUG) )

#  define BOOST_SP_NOEXCEPT_WITH_ASSERT

#else

#  define BOOST_SP_NOEXCEPT_WITH_ASSERT noexcept

#endif

#endif  // #ifndef BOOST_SMART_PTR_DETAIL_SP_NOEXCEPT_HPP_INCLUDED
