/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SharedICList_h
#define jit_SharedICList_h

namespace js {
namespace jit {

// List of IC stub kinds that can run in Baseline and in IonMonkey
#define IC_SHARED_STUB_KIND_LIST(_)              \
    _(BinaryArith_Fallback)                      \
    _(BinaryArith_Int32)                         \
    _(BinaryArith_Double)                        \
    _(BinaryArith_StringConcat)                  \
    _(BinaryArith_StringObjectConcat)            \
    _(BinaryArith_BooleanWithInt32)              \
    _(BinaryArith_DoubleWithInt32)               \
                                                 \
    _(UnaryArith_Fallback)                       \
    _(UnaryArith_Int32)                          \
    _(UnaryArith_Double)                         \
                                                 \
    _(Compare_Fallback)                          \
    _(Compare_Int32)                             \
    _(Compare_Double)                            \
    _(Compare_NumberWithUndefined)               \
    _(Compare_String)                            \
    _(Compare_Symbol)                            \
    _(Compare_Boolean)                           \
    _(Compare_Object)                            \
    _(Compare_ObjectWithUndefined)               \
    _(Compare_Int32WithBoolean)                  \
                                                 \
    _(GetProp_Fallback)                          \
                                                 \
    _(CacheIR_Regular)                           \
    _(CacheIR_Monitored)                         \
    _(CacheIR_Updated)                           \
                                                 \

} // namespace jit
} // namespace js

#endif /* jit_SharedICList_h */
