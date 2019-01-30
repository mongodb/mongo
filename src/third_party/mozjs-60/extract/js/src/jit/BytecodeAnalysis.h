/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BytecodeAnalysis_h
#define jit_BytecodeAnalysis_h

#include "jit/JitAllocPolicy.h"
#include "js/Vector.h"
#include "vm/JSScript.h"

namespace js {
namespace jit {

// Basic information about bytecodes in the script.  Used to help baseline compilation.
struct BytecodeInfo
{
    static const uint16_t MAX_STACK_DEPTH = 0xffffU;
    uint16_t stackDepth;
    bool initialized : 1;
    bool jumpTarget : 1;

    // If true, this is a JSOP_LOOPENTRY op inside a catch or finally block.
    bool loopEntryInCatchOrFinally : 1;

    void init(unsigned depth) {
        MOZ_ASSERT(depth <= MAX_STACK_DEPTH);
        MOZ_ASSERT_IF(initialized, stackDepth == depth);
        initialized = true;
        stackDepth = depth;
    }
};

class BytecodeAnalysis
{
    JSScript* script_;
    Vector<BytecodeInfo, 0, JitAllocPolicy> infos_;

    bool usesEnvironmentChain_;
    bool hasTryFinally_;

  public:
    explicit BytecodeAnalysis(TempAllocator& alloc, JSScript* script);

    MOZ_MUST_USE bool init(TempAllocator& alloc, GSNCache& gsn);

    BytecodeInfo& info(jsbytecode* pc) {
        MOZ_ASSERT(infos_[script_->pcToOffset(pc)].initialized);
        return infos_[script_->pcToOffset(pc)];
    }

    BytecodeInfo* maybeInfo(jsbytecode* pc) {
        if (infos_[script_->pcToOffset(pc)].initialized)
            return &infos_[script_->pcToOffset(pc)];
        return nullptr;
    }

    bool usesEnvironmentChain() const {
        return usesEnvironmentChain_;
    }

    bool hasTryFinally() const {
        return hasTryFinally_;
    }
};


} // namespace jit
} // namespace js

#endif /* jit_BytecodeAnalysis_h */
