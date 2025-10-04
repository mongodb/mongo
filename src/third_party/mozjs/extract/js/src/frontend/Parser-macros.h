/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_Parser_macros_h
#define frontend_Parser_macros_h

#include "mozilla/Likely.h"  // MOZ_UNLIKELY

#define MOZ_TRY_VAR_OR_RETURN(target, expr, returnValue) \
  do {                                                   \
    auto parserTryVarTempResult_ = (expr);               \
    if (MOZ_UNLIKELY(parserTryVarTempResult_.isErr())) { \
      return (returnValue);                              \
    }                                                    \
    (target) = parserTryVarTempResult_.unwrap();         \
  } while (0)

#endif /* frontend_Parser_macros_h */
