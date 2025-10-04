/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InlineScriptTree_inl_h
#define jit_InlineScriptTree_inl_h

#include "jit/InlineScriptTree.h"

#include "mozilla/Assertions.h"

#include "jit/JitAllocPolicy.h"
#include "js/TypeDecls.h"
#include "vm/JSScript.h"

namespace js {
namespace jit {

InlineScriptTree* InlineScriptTree::New(TempAllocator* allocator,
                                        InlineScriptTree* callerTree,
                                        jsbytecode* callerPc, JSScript* script,
                                        bool isMonomorphicallyInlined) {
  MOZ_ASSERT_IF(!callerTree, !callerPc);
  MOZ_ASSERT_IF(callerTree, callerTree->script()->containsPC(callerPc));

  // Allocate a new InlineScriptTree
  void* treeMem = allocator->allocate(sizeof(InlineScriptTree));
  if (!treeMem) {
    return nullptr;
  }

  // Initialize it.
  return new (treeMem)
      InlineScriptTree(callerTree, callerPc, script, isMonomorphicallyInlined);
}

InlineScriptTree* InlineScriptTree::addCallee(TempAllocator* allocator,
                                              jsbytecode* callerPc,
                                              JSScript* calleeScript,
                                              bool isMonomorphicallyInlined) {
  MOZ_ASSERT(script_ && script_->containsPC(callerPc));
  InlineScriptTree* calleeTree =
      New(allocator, this, callerPc, calleeScript, isMonomorphicallyInlined);
  if (!calleeTree) {
    return nullptr;
  }

  calleeTree->nextCallee_ = children_;
  children_ = calleeTree;
  return calleeTree;
}

void InlineScriptTree::removeCallee(InlineScriptTree* callee) {
  InlineScriptTree** prevPtr = &children_;
  for (InlineScriptTree* child = children_; child; child = child->nextCallee_) {
    if (child == callee) {
      *prevPtr = child->nextCallee_;
      return;
    }
    prevPtr = &child->nextCallee_;
  }
  MOZ_CRASH("Callee not found");
}

}  // namespace jit
}  // namespace js

#endif /* jit_InlineScriptTree_inl_h */
