/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineICList_h
#define jit_BaselineICList_h

namespace js {
namespace jit {

// List of trampolines for Baseline IC fallback stubs. Trampoline code is
// allocated as part of the JitRuntime.
#define IC_BASELINE_FALLBACK_CODE_KIND_LIST(_) \
  _(NewArray)                                  \
  _(NewObject)                                 \
  _(Lambda)                                    \
  _(ToBool)                                    \
  _(UnaryArith)                                \
  _(Call)                                      \
  _(CallConstructing)                          \
  _(SpreadCall)                                \
  _(SpreadCallConstructing)                    \
  _(GetElem)                                   \
  _(GetElemSuper)                              \
  _(SetElem)                                   \
  _(In)                                        \
  _(HasOwn)                                    \
  _(CheckPrivateField)                         \
  _(GetName)                                   \
  _(BindName)                                  \
  _(LazyConstant)                              \
  _(SetProp)                                   \
  _(GetIterator)                               \
  _(OptimizeSpreadCall)                        \
  _(InstanceOf)                                \
  _(TypeOf)                                    \
  _(TypeOfEq)                                  \
  _(ToPropertyKey)                             \
  _(Rest)                                      \
  _(BinaryArith)                               \
  _(Compare)                                   \
  _(GetProp)                                   \
  _(GetPropSuper)                              \
  _(CloseIter)                                 \
  _(OptimizeGetIterator)                       \
  _(GetImport)

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineICList_h */
