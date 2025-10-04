/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitCommon_h
#define jit_JitCommon_h

// Various macros used by all JITs.

#include "jit/Simulator.h"

#ifdef JS_SIMULATOR
// Call into cross-jitted code by following the ABI of the simulated
// architecture.
#  define CALL_GENERATED_CODE(entry, p0, p1, p2, p3, p4, p5, p6, p7)          \
    (js::jit::Simulator::Current()->call(                                     \
        JS_FUNC_TO_DATA_PTR(uint8_t*, entry), 8, intptr_t(p0), intptr_t(p1),  \
        intptr_t(p2), intptr_t(p3), intptr_t(p4), intptr_t(p5), intptr_t(p6), \
        intptr_t(p7)))

#  define CALL_GENERATED_0(entry)                                              \
    (js::jit::Simulator::Current()->call(JS_FUNC_TO_DATA_PTR(uint8_t*, entry), \
                                         0))

#  define CALL_GENERATED_1(entry, p0)                                          \
    (js::jit::Simulator::Current()->call(JS_FUNC_TO_DATA_PTR(uint8_t*, entry), \
                                         1, intptr_t(p0)))

#  define CALL_GENERATED_2(entry, p0, p1)                                      \
    (js::jit::Simulator::Current()->call(JS_FUNC_TO_DATA_PTR(uint8_t*, entry), \
                                         2, intptr_t(p0), intptr_t(p1)))

#  define CALL_GENERATED_3(entry, p0, p1, p2)                                  \
    (js::jit::Simulator::Current()->call(JS_FUNC_TO_DATA_PTR(uint8_t*, entry), \
                                         3, intptr_t(p0), intptr_t(p1),        \
                                         intptr_t(p2)))

#else

// Call into jitted code by following the ABI of the native architecture.
#  define CALL_GENERATED_CODE(entry, p0, p1, p2, p3, p4, p5, p6, p7) \
    entry(p0, p1, p2, p3, p4, p5, p6, p7)

#  define CALL_GENERATED_0(entry) entry()
#  define CALL_GENERATED_1(entry, p0) entry(p0)
#  define CALL_GENERATED_2(entry, p0, p1) entry(p0, p1)
#  define CALL_GENERATED_3(entry, p0, p1, p2) entry(p0, p1, p2)

#endif

#endif  // jit_JitCommon_h
