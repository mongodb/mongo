/** @file server.h

    This file contains includes commonly needed in the server files (mongod, mongos, test).  It is NOT included in the C++ client.

    Over time we should move more here, and more out of pch.h.  And get rid of pch.h at some point.
*/

// todo is there a boost  thign for this already?

#pragma once

#include "bson/inline_decls.h"

/* Note: do not clutter code with these -- ONLY use in hot spots / significant loops. */

// branch prediction.  indicate we expect to enter the if statement body
#define IF MONGOIF

// branch prediction.  indicate we expect to not enter the if statement body
#define _IF MONGO_IF

