#ifndef _json_strerror_override_h_
#define _json_strerror_override_h_

/**
 * @file
 * @brief Do not use, json-c internal, may be changed or removed at any time.
 */

#include "config.h"
#include <errno.h>

#include "json_object.h" /* for JSON_EXPORT */

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>

JSON_EXPORT char *_json_c_strerror(int errno_in);

#ifndef STRERROR_OVERRIDE_IMPL
#define strerror _json_c_strerror
#endif

#ifdef __cplusplus
}
#endif

#endif /* _json_strerror_override_h_ */
