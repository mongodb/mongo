/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_BaselineCompiler_shared_h
#define jit_shared_BaselineCompiler_shared_h

#include "jit/BaselineFrameInfo.h"
#include "jit/BaselineIC.h"
#include "jit/BytecodeAnalysis.h"
#include "jit/MacroAssembler.h"

namespace js {
namespace jit {

class BaselineCompilerShared
{
  protected:
    JSContext* cx;
    JSScript* script;
    jsbytecode* pc;
    MacroAssembler masm;
    bool ionCompileable_;
    bool ionOSRCompileable_;
    bool compileDebugInstrumentation_;

    TempAllocator& alloc_;
    BytecodeAnalysis analysis_;
    FrameInfo frame;

    FallbackICStubSpace stubSpace_;
    js::Vector<ICEntry, 16, SystemAllocPolicy> icEntries_;

    // Stores the native code offset for a bytecode pc.
    struct PCMappingEntry
    {
        uint32_t pcOffset;
        uint32_t nativeOffset;
        PCMappingSlotInfo slotInfo;

        // If set, insert a PCMappingIndexEntry before encoding the
        // current entry.
        bool addIndexEntry;
    };

    js::Vector<PCMappingEntry, 16, SystemAllocPolicy> pcMappingEntries_;

    // Labels for the 'movWithPatch' for loading IC entry pointers in
    // the generated IC-calling code in the main jitcode.  These need
    // to be patched with the actual icEntry offsets after the BaselineScript
    // has been allocated.
    struct ICLoadLabel {
        size_t icEntry;
        CodeOffset label;
    };
    js::Vector<ICLoadLabel, 16, SystemAllocPolicy> icLoadLabels_;

    uint32_t pushedBeforeCall_;
    mozilla::DebugOnly<bool> inCall_;

    CodeOffset spsPushToggleOffset_;
    CodeOffset profilerEnterFrameToggleOffset_;
    CodeOffset profilerExitFrameToggleOffset_;
    CodeOffset traceLoggerEnterToggleOffset_;
    CodeOffset traceLoggerExitToggleOffset_;
    CodeOffset traceLoggerScriptTextIdOffset_;

    BaselineCompilerShared(JSContext* cx, TempAllocator& alloc, JSScript* script);

    ICEntry* allocateICEntry(ICStub* stub, ICEntry::Kind kind) {
        if (!stub)
            return nullptr;

        // Create the entry and add it to the vector.
        if (!icEntries_.append(ICEntry(script->pcToOffset(pc), kind))) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        ICEntry& vecEntry = icEntries_.back();

        // Set the first stub for the IC entry to the fallback stub
        vecEntry.setFirstStub(stub);

        // Return pointer to the IC entry
        return &vecEntry;
    }

    // Append an ICEntry without a stub.
    bool appendICEntry(ICEntry::Kind kind, uint32_t returnOffset) {
        ICEntry entry(script->pcToOffset(pc), kind);
        entry.setReturnOffset(CodeOffset(returnOffset));
        if (!icEntries_.append(entry)) {
            ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }

    bool addICLoadLabel(CodeOffset label) {
        MOZ_ASSERT(!icEntries_.empty());
        ICLoadLabel loadLabel;
        loadLabel.label = label;
        loadLabel.icEntry = icEntries_.length() - 1;
        if (!icLoadLabels_.append(loadLabel)) {
            ReportOutOfMemory(cx);
            return false;
        }
        return true;
    }

    JSFunction* function() const {
        // Not delazifying here is ok as the function is guaranteed to have
        // been delazified before compilation started.
        return script->functionNonDelazifying();
    }

    ModuleObject* module() const {
        return script->module();
    }

    PCMappingSlotInfo getStackTopSlotInfo() {
        MOZ_ASSERT(frame.numUnsyncedSlots() <= 2);
        switch (frame.numUnsyncedSlots()) {
          case 0:
            return PCMappingSlotInfo::MakeSlotInfo();
          case 1:
            return PCMappingSlotInfo::MakeSlotInfo(PCMappingSlotInfo::ToSlotLocation(frame.peek(-1)));
          case 2:
          default:
            return PCMappingSlotInfo::MakeSlotInfo(PCMappingSlotInfo::ToSlotLocation(frame.peek(-1)),
                                                   PCMappingSlotInfo::ToSlotLocation(frame.peek(-2)));
        }
    }

    template <typename T>
    void pushArg(const T& t) {
        masm.Push(t);
    }
    void prepareVMCall();

    enum CallVMPhase {
        POST_INITIALIZE,
        PRE_INITIALIZE,
        CHECK_OVER_RECURSED
    };
    bool callVM(const VMFunction& fun, CallVMPhase phase=POST_INITIALIZE);

    bool callVMNonOp(const VMFunction& fun, CallVMPhase phase=POST_INITIALIZE) {
        if (!callVM(fun, phase))
            return false;
        icEntries_.back().setFakeKind(ICEntry::Kind_NonOpCallVM);
        return true;
    }

  public:
    BytecodeAnalysis& analysis() {
        return analysis_;
    }

    void setCompileDebugInstrumentation() {
        compileDebugInstrumentation_ = true;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_shared_BaselineCompiler_shared_h */
