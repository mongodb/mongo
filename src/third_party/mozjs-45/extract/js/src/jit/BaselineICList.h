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
                                                 \
    _(TypeUpdate_Fallback)                       \
    _(TypeUpdate_SingleObject)                   \
    _(TypeUpdate_ObjectGroup)                    \
    _(TypeUpdate_PrimitiveSet)                   \
                                                 \
    _(NewArray_Fallback)                         \
    _(NewObject_Fallback)                        \
    _(NewObject_WithTemplate)                    \
                                                 \
    _(ToBool_Fallback)                           \
    _(ToBool_Int32)                              \
    _(ToBool_String)                             \
    _(ToBool_NullUndefined)                      \
    _(ToBool_Double)                             \
    _(ToBool_Object)                             \
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
    _(Call_StringSplit)                          \
    _(Call_IsSuspendedStarGenerator)             \
                                                 \
    _(GetElem_Fallback)                          \
    _(GetElem_NativeSlotName)                    \
    _(GetElem_NativeSlotSymbol)                  \
    _(GetElem_NativePrototypeSlotName)           \
    _(GetElem_NativePrototypeSlotSymbol)         \
    _(GetElem_NativePrototypeCallNativeName)     \
    _(GetElem_NativePrototypeCallNativeSymbol)   \
    _(GetElem_NativePrototypeCallScriptedName)   \
    _(GetElem_NativePrototypeCallScriptedSymbol) \
    _(GetElem_UnboxedPropertyName)               \
    _(GetElem_String)                            \
    _(GetElem_Dense)                             \
    _(GetElem_UnboxedArray)                      \
    _(GetElem_TypedArray)                        \
    _(GetElem_Arguments)                         \
                                                 \
    _(SetElem_Fallback)                          \
    _(SetElem_DenseOrUnboxedArray)               \
    _(SetElem_DenseOrUnboxedArrayAdd)            \
    _(SetElem_TypedArray)                        \
                                                 \
    _(In_Fallback)                               \
    _(In_Native)                                 \
    _(In_NativePrototype)                        \
    _(In_NativeDoesNotExist)                     \
    _(In_Dense)                                  \
                                                 \
    _(GetName_Fallback)                          \
    _(GetName_GlobalLexical)                     \
    _(GetName_Global)                            \
    _(GetName_Scope0)                            \
    _(GetName_Scope1)                            \
    _(GetName_Scope2)                            \
    _(GetName_Scope3)                            \
    _(GetName_Scope4)                            \
    _(GetName_Scope5)                            \
    _(GetName_Scope6)                            \
                                                 \
    _(BindName_Fallback)                         \
                                                 \
    _(GetIntrinsic_Fallback)                     \
    _(GetIntrinsic_Constant)                     \
                                                 \
    _(SetProp_Fallback)                          \
    _(SetProp_Native)                            \
    _(SetProp_NativeAdd)                         \
    _(SetProp_Unboxed)                           \
    _(SetProp_TypedObject)                       \
    _(SetProp_CallScripted)                      \
    _(SetProp_CallNative)                        \
                                                 \
    _(TableSwitch)                               \
                                                 \
    _(IteratorNew_Fallback)                      \
    _(IteratorMore_Fallback)                     \
    _(IteratorMore_Native)                       \
    _(IteratorClose_Fallback)                    \
                                                 \
    _(InstanceOf_Fallback)                       \
    _(InstanceOf_Function)                       \
                                                 \
    _(TypeOf_Fallback)                           \
    _(TypeOf_Typed)                              \
                                                 \
    _(Rest_Fallback)                             \
                                                 \
    _(RetSub_Fallback)                           \
    _(RetSub_Resume)

} // namespace jit
} // namespace js
 
#endif /* jit_BaselineICList_h */
