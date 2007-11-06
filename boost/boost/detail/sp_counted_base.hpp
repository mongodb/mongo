#ifndef BOOST_DETAIL_SP_COUNTED_BASE_HPP_INCLUDED
#define BOOST_DETAIL_SP_COUNTED_BASE_HPP_INCLUDED

// MS compatible compilers support #pragma once

#if defined(_MSC_VER) && (_MSC_VER >= 1020)
# pragma once
#endif

//
//  detail/sp_counted_base.hpp
//
//  Copyright 2005 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#include <boost/config.hpp>

#if defined( BOOST_SP_DISABLE_THREADS )

# include <boost/detail/sp_counted_base_nt.hpp>

#elif defined( BOOST_SP_USE_PTHREADS )

# include <boost/detail/sp_counted_base_pt.hpp>

#elif defined( __GNUC__ ) && ( defined( __i386__ ) || defined( __x86_64__ ) )

# include <boost/detail/sp_counted_base_gcc_x86.hpp>

//~ #elif defined( __MWERKS__ ) && ( defined( __i386__ ) || defined( __x86_64__ ) )

//~ # include <boost/detail/sp_counted_base_cw_x86.hpp>

#elif defined( __GNUC__ ) && defined( __ia64__ ) && !defined( __INTEL_COMPILER )

# include <boost/detail/sp_counted_base_gcc_ia64.hpp>

#elif defined( __MWERKS__ ) && defined( __POWERPC__ )

# include <boost/detail/sp_counted_base_cw_ppc.hpp>

#elif defined( __GNUC__ ) && ( defined( __powerpc__ ) || defined( __ppc__ ) )

# include <boost/detail/sp_counted_base_gcc_ppc.hpp>

#elif defined( WIN32 ) || defined( _WIN32 ) || defined( __WIN32__ )

# include <boost/detail/sp_counted_base_w32.hpp>

#elif !defined( BOOST_HAS_THREADS )

# include <boost/detail/sp_counted_base_nt.hpp>

#elif defined( BOOST_HAS_PTHREADS )

# include <boost/detail/sp_counted_base_pt.hpp>

#else

// Use #define BOOST_DISABLE_THREADS to avoid the error
# error Unrecognized threading platform

#endif

#endif  // #ifndef BOOST_DETAIL_SP_COUNTED_BASE_HPP_INCLUDED
