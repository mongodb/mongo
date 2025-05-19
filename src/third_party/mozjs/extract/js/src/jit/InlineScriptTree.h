/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_InlineScriptTree_h
#define jit_InlineScriptTree_h

#include "mozilla/Assertions.h"

#include "jit/JitAllocPolicy.h"
#include "js/TypeDecls.h"

namespace js {
namespace jit {

// The compiler at various points needs to be able to store references to the
// current inline path (the sequence of scripts and call-pcs that lead to the
// current function being inlined).
//
// To support this, use a tree that records the inlinings done during
// compilation.
class InlineScriptTree {
  // InlineScriptTree for the caller
  InlineScriptTree* caller_;

  // PC in the caller corresponding to this script.
  jsbytecode* callerPc_;

  // Script for this entry.
  JSScript* script_;

  // Child entries (linked together by nextCallee pointer)
  InlineScriptTree* children_;
  InlineScriptTree* nextCallee_;

  // Whether this script is monomorphically inlined into its caller.
  bool isMonomorphicallyInlined_;

 public:
  InlineScriptTree(InlineScriptTree* caller, jsbytecode* callerPc,
                   JSScript* script, bool isMonomorphicallyInlined)
      : caller_(caller),
        callerPc_(callerPc),
        script_(script),
        children_(nullptr),
        nextCallee_(nullptr),
        isMonomorphicallyInlined_(isMonomorphicallyInlined) {}

  static inline InlineScriptTree* New(TempAllocator* allocator,
                                      InlineScriptTree* caller,
                                      jsbytecode* callerPc, JSScript* script,
                                      bool isMonomorphicallyInlined = false);

  inline InlineScriptTree* addCallee(TempAllocator* allocator,
                                     jsbytecode* callerPc,
                                     JSScript* calleeScript,
                                     bool isMonomorphicallyInlined);
  inline void removeCallee(InlineScriptTree* callee);

  InlineScriptTree* caller() const { return caller_; }

  bool isOutermostCaller() const { return caller_ == nullptr; }
  bool hasCaller() const { return caller_ != nullptr; }

  jsbytecode* callerPc() const { return callerPc_; }

  JSScript* script() const { return script_; }

  bool hasChildren() const { return children_ != nullptr; }
  InlineScriptTree* firstChild() const {
    MOZ_ASSERT(hasChildren());
    return children_;
  }

  bool hasNextCallee() const { return nextCallee_ != nullptr; }
  InlineScriptTree* nextCallee() const {
    MOZ_ASSERT(hasNextCallee());
    return nextCallee_;
  }

  unsigned depth() const {
    if (isOutermostCaller()) {
      return 1;
    }
    return 1 + caller_->depth();
  }

  // Returns true if this script, or any of its callers in the
  // inlining tree, was monomorphically inlined. If this is true,
  // ICs transpiled into this compilation may also be transpiled
  // into another compilation.
  bool hasSharedICScript() const {
    const InlineScriptTree* script = this;
    while (!script->isOutermostCaller()) {
      if (script->isMonomorphicallyInlined_) {
        return true;
      }
      script = script->caller();
    }
    return false;
  }
};

class BytecodeSite : public TempObject {
  // InlineScriptTree identifying innermost active function at site.
  InlineScriptTree* tree_;

  // Bytecode address within innermost active function.
  jsbytecode* pc_;

 public:
  // Wasm compilation leaves both fields null.
  BytecodeSite() : tree_(nullptr), pc_(nullptr) {}

  // Warp compilation sets both fields to non-null values.
  BytecodeSite(InlineScriptTree* tree, jsbytecode* pc) : tree_(tree), pc_(pc) {
    MOZ_ASSERT(tree_ != nullptr);
    MOZ_ASSERT(pc_ != nullptr);
  }

  InlineScriptTree* tree() const { return tree_; }

  jsbytecode* pc() const { return pc_; }

  JSScript* script() const { return tree_ ? tree_->script() : nullptr; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_InlineScriptTree_h */
