/** @file server.h

    This file contains includes commonly needed in the server files (mongod, mongos, test).  It is NOT included in the C++ client.

    Over time we should move more here, and more out of pch.h.  And get rid of pch.h at some point.
*/

// todo is there a boost  thign for this already?

#pragma once

#include "bson/inline_decls.h"

/* Note: do not clutter code with these -- ONLY use in hot spots / significant loops. */

// branch prediction.  indicate we expect to be true
#define likely MONGO_likely

// branch prediction.  indicate we expect to be false
#define unlikely MONGO_unlikely

#if defined(__GNUC__)

#define CACHEALIGN __attribute__((aligned(128))

#elif defined(_MSC_VER)

#define CACHEALIGN __declspec(align(128)) 

#else

#define CACHEALIGN 

#endif
