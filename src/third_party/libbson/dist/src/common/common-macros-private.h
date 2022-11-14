
#include "common-prelude.h"

#ifndef MONGO_C_DRIVER_COMMON_MACROS_H
#define MONGO_C_DRIVER_COMMON_MACROS_H

/* Test only assert. Is a noop unless -DENABLE_DEBUG_ASSERTIONS=ON is set
 * during configuration */
#if defined(MONGOC_ENABLE_DEBUG_ASSERTIONS) && defined(BSON_OS_UNIX)
#define MONGOC_DEBUG_ASSERT(statement) BSON_ASSERT (statement)
#else
#define MONGOC_DEBUG_ASSERT(statement) ((void) 0)
#endif

#endif
