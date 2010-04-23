#undef MONGO_EXPOSE_MACROS

#include "../client/dbclient.h"

#ifdef malloc
# error malloc defined 0
#endif

#ifdef assert
# error assert defined 1
#endif

#include "../client/parallel.h" //uses assert

#ifdef assert
# error assert defined 2
#endif

#include "../client/redef_macros.h"

#ifndef assert
# error assert not defined 3
#endif

#include "../client/undef_macros.h"

#ifdef assert
# error assert defined 3
#endif


