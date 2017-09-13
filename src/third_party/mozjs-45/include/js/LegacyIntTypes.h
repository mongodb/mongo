/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * This section typedefs the old 'native' types to the new <stdint.h> types.
 * These redefinitions are provided solely to allow JSAPI users to more easily
 * transition to <stdint.h> types.  They are not to be used in the JSAPI, and
 * new JSAPI user code should not use them.  This mapping file may eventually
 * be removed from SpiderMonkey, so don't depend on it in the long run.
 */

/*
 * BEWARE: Comity with other implementers of these types is not guaranteed.
 *         Indeed, if you use this header and third-party code defining these
 *         types, *expect* to encounter either compile errors or link errors,
 *         depending how these types are used and on the order of inclusion.
 *         It is safest to use only the <stdint.h> types.
 */
#ifndef js_LegacyIntTypes_h
#define js_LegacyIntTypes_h

#include <stdint.h>

#include "js-config.h"

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

/*
 * On AIX 4.3, sys/inttypes.h (which is included by sys/types.h, a very
 * common header file) defines the types int8, int16, int32, and int64.
 * So we don't define these four types here to avoid conflicts in case
 * the code also includes sys/types.h.
 */
#if defined(AIX) && defined(HAVE_SYS_INTTYPES_H)
#include <sys/inttypes.h>
#else
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
#endif /* AIX && HAVE_SYS_INTTYPES_H */

typedef uint8_t JSUint8;
typedef uint16_t JSUint16;
typedef uint32_t JSUint32;
typedef uint64_t JSUint64;

typedef int8_t JSInt8;
typedef int16_t JSInt16;
typedef int32_t JSInt32;
typedef int64_t JSInt64;

#endif /* js_LegacyIntTypes_h */
