/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_AtomicOperations_feeling_lucky_h
#define jit_shared_AtomicOperations_feeling_lucky_h

#if defined(__clang__) || defined(__GNUC__)
#  include "jit/shared/AtomicOperations-feeling-lucky-gcc.h"
#elif defined(_MSC_VER)
#  include "jit/shared/AtomicOperations-feeling-lucky-msvc.h"
#else
#  error "No AtomicOperations support for this platform+compiler combination"
#endif

#endif  // jit_shared_AtomicOperations_feeling_lucky_h
