/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Various #defines required to build SpiderMonkey.  Embedders should add this
 * file to the start of the command line via -include or a similar mechanism,
 * or SpiderMonkey public headers may not work correctly.
 */

#ifndef js_RequiredDefines_h
#define js_RequiredDefines_h

/*
 * The c99 defining the limit macros (UINT32_MAX for example), says:
 *
 *   C++ implementations should define these macros only when
 *   __STDC_LIMIT_MACROS is defined before <stdint.h> is included.
 *
 * The same also occurs with __STDC_CONSTANT_MACROS for the constant macros
 * (INT8_C for example) used to specify a literal constant of the proper type,
 * and with __STDC_FORMAT_MACROS for the format macros (PRId32 for example) used
 * with the fprintf function family.
 */
#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS
#define __STDC_FORMAT_MACROS

/* Also define a char16_t type if not provided by the compiler. */
#include "mozilla/Char16.h"

#endif /* js_RequiredDefines_h */
