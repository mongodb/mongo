/** @file server.h

    This file contains includes commonly needed in the server files (mongod, mongos, test).  It is *NOT* included in the C++ client; i.e. 
    this is a very good place for global-ish things that you don't need to be in the client lib.

    Over time we should move more here, and more out of pch.h.  And get rid of pch.h at some point.
*/

#pragma once

#if !defined(MONGO_EXPOSE_MACROS)
# error this file is for mongo server programs not client lib
#endif

#if defined(_DEBUG)
# define BOOST_ENABLE_ASSERT_HANDLER 1
#else
# define BOOST_DISABLE_ASSERTS 1
#endif

#include <map>
#include <vector>
#include <set>

#include "bson/inline_decls.h"

//using namespace std;
//using namespace bson;

/* Note: do not clutter code with these -- ONLY use in hot spots / significant loops. */

// branch prediction.  indicate we expect to be true
#define likely MONGO_likely

// branch prediction.  indicate we expect to be false
#define unlikely MONGO_unlikely

// prefetch data from memory
//#define PREFETCH MONGOPREFETCH

#if defined(__GNUC__)

#define CACHEALIGN __attribute__((aligned(64))

#elif defined(_MSC_VER)

#define CACHEALIGN __declspec(align(64)) 

#else

#define CACHEALIGN 

#endif

// log but not too fast.  this is rather simplistic we can do something fancier later
#define LOGSOME static time_t __last; time_t __now=time(0); if(__last+5<__now) {} else log() 
