
/**
 * @file
 * @brief Do not use, json-c internal, may be changed or removed at any time.
 */
#ifndef _json_inttypes_h_
#define _json_inttypes_h_

#include "json_config.h"

#ifdef JSON_C_HAVE_INTTYPES_H
/* inttypes.h includes stdint.h */
#include <inttypes.h>

#else
#ifdef JSON_C_HAVE_STDINT_H
#include <stdint.h>
#else
/* Really only valid for old MS compilers, VS2008 and earlier: */
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
#endif

#define PRId64 "I64d"
#define SCNd64 "I64d"
#define PRIu64 "I64u"

#endif

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#endif
