// Copyright (C) 2004 Arkadiy Vertleyb
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_TYPEOF_TYPEOF_HPP_INCLUDED
#define BOOST_TYPEOF_TYPEOF_HPP_INCLUDED

#if defined(BOOST_TYPEOF_COMPLIANT)
#   define BOOST_TYPEOF_EMULATION
#endif

#if defined(BOOST_TYPEOF_EMULATION) && defined(BOOST_TYPEOF_NATIVE)
#   error both typeof emulation and native mode requested 
#endif

#if defined(__COMO__)
#   ifdef __GNUG__
#       ifndef(BOOST_TYPEOF_EMULATION)
#           ifndef BOOST_TYPEOF_NATIVE
#               define BOOST_TYPEOF_NATIVE
#           endif
#           define BOOST_TYPEOF_KEYWORD typeof
#       endif
#   else
#       ifndef BOOST_TYPEOF_NATIVE
#           ifndef BOOST_TYPEOF_EMULATION
#               define BOOST_TYPEOF_EMULATION
#           endif
#       else
#           error native typeof is not supported
#       endif
#   endif

#elif defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)
#   ifdef __GNUC__
#       ifndef BOOST_TYPEOF_EMULATION
#           ifndef BOOST_TYPEOF_NATIVE
#               define BOOST_TYPEOF_NATIVE
#           endif
#           define BOOST_TYPEOF_KEYWORD __typeof__
#       endif
#   else
#       ifndef BOOST_TYPEOF_NATIVE
#           ifndef BOOST_TYPEOF_EMULATION
#               define BOOST_TYPEOF_EMULATION
#           endif
#       else
#           error native typeof is not supported
#       endif
#   endif

#elif defined(__GNUC__)
#   ifndef BOOST_TYPEOF_EMULATION
#       ifndef BOOST_TYPEOF_NATIVE
#           define BOOST_TYPEOF_NATIVE
#       endif
#       define BOOST_TYPEOF_KEYWORD __typeof__
#   endif

#elif defined(__MWERKS__)
#   if(__MWERKS__ <= 0x3003)  // 8.x
#       ifndef BOOST_TYPEOF_EMULATION
#           ifndef BOOST_TYPEOF_NATIVE
#               define BOOST_TYPEOF_NATIVE
#           endif
#           define BOOST_TYPEOF_KEYWORD __typeof__
#       else
#           error typeof emulation is not supported
#       endif
#   else // 9.x
#       ifndef BOOST_TYPEOF_EMULATION
#           ifndef BOOST_TYPEOF_NATIVE
#               define BOOST_TYPEOF_NATIVE
#           endif
#           define BOOST_TYPEOF_KEYWORD __typeof__
#       endif
#   endif

#elif defined(_MSC_VER)
#   if (_MSC_VER <= 1300)  // 6.5, 7.0
#       ifndef BOOST_TYPEOF_EMULATION
#           ifndef BOOST_TYPEOF_NATIVE
#               define BOOST_TYPEOF_NATIVE
#           endif
#           include <boost/typeof/msvc/typeof_impl.hpp>
#           define MSVC_TYPEOF_HACK
#       else
#           error typeof emulation is not supported
#       endif
#   elif (_MSC_VER == 1310)  // 7.1
#       ifndef BOOST_TYPEOF_EMULATION
#           ifndef BOOST_TYPEOF_NATIVE
#               define BOOST_TYPEOF_NATIVE
#           endif
#           include <boost/typeof/msvc/typeof_impl.hpp>
#           define MSVC_TYPEOF_HACK
#       endif
#   else // 8.0
#       ifndef BOOST_TYPEOF_NATIVE
#           ifndef BOOST_TYPEOF_EMULATION
#               define BOOST_TYPEOF_EMULATION
#           endif
#       else
#           error native typeof is not supported
#       endif
#   endif

#else //unknown compiler
#   ifndef BOOST_TYPEOF_NATIVE
#       ifndef BOOST_TYPEOF_EMULATION
#           define BOOST_TYPEOF_EMULATION
#       endif
#   else
#       ifndef BOOST_TYPEOF_KEYWORD
#           define BOOST_TYPEOF_KEYWORD typeof
#       endif
#   endif

#endif

#define BOOST_TYPEOF_UNIQUE_ID()\
     BOOST_TYPEOF_REGISTRATION_GROUP * 0x10000 + __LINE__

#define BOOST_TYPEOF_INCREMENT_REGISTRATION_GROUP()\
     <boost/typeof/incr_registration_group.hpp>

#ifdef BOOST_TYPEOF_EMULATION
#   define BOOST_TYPEOF_TEXT "using typeof emulation"
#   include <boost/typeof/message.hpp>
#   include <boost/typeof/typeof_impl.hpp>
#   include <boost/typeof/type_encoding.hpp>
#   include <boost/typeof/template_encoding.hpp>
#   include <boost/typeof/modifiers.hpp>
#   include <boost/typeof/pointers_data_members.hpp>
#   include <boost/typeof/register_functions.hpp>
#   include <boost/typeof/register_fundamental.hpp>

#elif defined(BOOST_TYPEOF_NATIVE)
#   define BOOST_TYPEOF_TEXT "using native typeof"
#   include <boost/typeof/message.hpp>
#   include <boost/typeof/native.hpp>
#else
#   error typeof configuration error
#endif

// auto
#define BOOST_AUTO(Var, Expr) BOOST_TYPEOF(Expr) Var = Expr
#define BOOST_AUTO_TPL(Var, Expr) BOOST_TYPEOF_TPL(Expr) Var = Expr

#endif//BOOST_TYPEOF_TYPEOF_HPP_INCLUDED
