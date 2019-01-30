/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineICList_h
#define jit_BaselineICList_h

namespace js {
namespace jit {

// List of IC stub kinds that can only run in Baseline.
#define IC_BASELINE_STUB_KIND_LIST(_)            \
    _(WarmUpCounter_Fallback)                    \
                                                 \
    _(TypeMonitor_Fallback)                      \
    _(TypeMonitor_SingleObject)                  \
    _(TypeMonitor_ObjectGroup)                   \
    _(TypeMonitor_PrimitiveSet)                  \
    _(TypeMonitor_AnyValue)                      \
                                                 \
    _(TypeUpdate_Fallback)                       \
    _(TypeUpdate_SingleObject)                   \
    _(TypeUpdate_ObjectGroup)                    \
    _(TypeUpdate_PrimitiveSet)                   \
    _(TypeUpdate_AnyValue)                       \
                                                 \
    _(NewArray_Fallback)                         \
    _(NewObject_Fallback)                        \
    _(NewObject_WithTemplate)                    \
                                                 \
    _(ToBool_Fallback)                           \
                                                 \
    _(ToNumber_Fallback)                         \
                                                 \
    _(Call_Fallback)                             \
    _(Call_Scripted)                             \
    _(Call_AnyScripted)                          \
    _(Call_Native)                               \
    _(Call_ClassHook)                            \
    _(Call_ScriptedApplyArray)                   \
    _(Call_ScriptedApplyArguments)               \
    _(Call_ScriptedFunCall)                      \
    _(Call_ConstStringSplit)                     \
    _(Call_IsSuspendedGenerator)                 \
                                                 \
    _(GetElem_Fallback)                          \
    _(SetElem_Fallback)                          \
                                                 \
    _(In_Fallback)                               \
    _(HasOwn_Fallback)                           \
                                                 \
    _(GetName_Fallback)                          \
                                                 \
    _(BindName_Fallback)                         \
                                                 \
    _(GetIntrinsic_Fallback)                     \
                                                 \
    _(SetProp_Fallback)                          \
                                                 \
    _(TableSwitch)                               \
                                                 \
    _(GetIterator_Fallback)                      \
    _(IteratorMore_Fallback)                     \
    _(IteratorMore_Native)                       \
    _(IteratorClose_Fallback)                    \
                                                 \
    _(InstanceOf_Fallback)                       \
    _(InstanceOf_Function)                       \
                                                 \
    _(TypeOf_Fallback)                           \
                                                 \
    _(Rest_Fallback)                             \
                                                 \
    _(RetSub_Fallback)                           \
    _(RetSub_Resume)

} // namespace jit
} // namespace js
 
#endif /* jit_BaselineICList_h */
