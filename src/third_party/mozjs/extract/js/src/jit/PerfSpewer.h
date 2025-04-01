/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_PerfSpewer_h
#define jit_PerfSpewer_h

#ifdef JS_ION_PERF
#  include <stdio.h>
#endif
#include "jit/BaselineFrameInfo.h"
#include "jit/CacheIR.h"
#include "jit/JitCode.h"
#include "jit/LIR.h"
#include "js/AllocPolicy.h"
#include "js/JitCodeAPI.h"
#include "js/Vector.h"
#include "vm/JSScript.h"

namespace js::jit {

using ProfilerJitCodeVector = Vector<JS::JitCodeRecord, 0, SystemAllocPolicy>;

void ResetPerfSpewer(bool enabled);

struct AutoLockPerfSpewer {
  AutoLockPerfSpewer();
  ~AutoLockPerfSpewer();
};

class MBasicBlock;
class MacroAssembler;

bool PerfEnabled();

class PerfSpewer {
 protected:
  struct OpcodeEntry {
    uint32_t offset = 0;
    unsigned opcode = 0;
    jsbytecode* bytecodepc = nullptr;

    // This string is used to replace the opcode, to define things like
    // Prologue/Epilogue, or to add operand info.
    UniqueChars str;

    explicit OpcodeEntry(uint32_t offset_, unsigned opcode_, UniqueChars& str_,
                         jsbytecode* pc)
        : offset(offset_), opcode(opcode_), bytecodepc(pc) {
      str = std::move(str_);
    }

    explicit OpcodeEntry(uint32_t offset_, unsigned opcode_, UniqueChars& str_)
        : offset(offset_), opcode(opcode_) {
      str = std::move(str_);
    }
    explicit OpcodeEntry(uint32_t offset_, UniqueChars& str_)
        : offset(offset_) {
      str = std::move(str_);
    }
    explicit OpcodeEntry(uint32_t offset_, unsigned opcode_)
        : offset(offset_), opcode(opcode_) {}

    explicit OpcodeEntry(jsbytecode* pc) : bytecodepc(pc) {}

    OpcodeEntry(OpcodeEntry&& copy) {
      offset = copy.offset;
      opcode = copy.opcode;
      bytecodepc = copy.bytecodepc;
      str = std::move(copy.str);
    }

    // Do not copy the UniqueChars member.
    OpcodeEntry(OpcodeEntry& copy) = delete;
  };
  Vector<OpcodeEntry, 0, SystemAllocPolicy> opcodes_;

  uint32_t lir_opcode_length = 0;
  uint32_t js_opcode_length = 0;

  virtual JS::JitTier GetTier() { return JS::JitTier::Other; }

  virtual const char* CodeName(unsigned op) = 0;

  virtual void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                                     JS::JitCodeRecord* record,
                                     AutoLockPerfSpewer& lock);

  void saveDebugInfo(JSScript* script, JitCode* code,
                     JS::JitCodeRecord* profilerRecord,
                     AutoLockPerfSpewer& lock);

  void saveProfile(JitCode* code, UniqueChars& desc, JSScript* script);

  void saveJitCodeIRInfo(JitCode* code, JS::JitCodeRecord* profilerRecord,
                         AutoLockPerfSpewer& lock);

 public:
  PerfSpewer() = default;

  void recordOffset(MacroAssembler& masm, const char*);

  static void Init();

  static void CollectJitCodeInfo(UniqueChars& function_name, JitCode* code,
                                 JS::JitCodeRecord*, AutoLockPerfSpewer& lock);
  static void CollectJitCodeInfo(UniqueChars& function_name, void* code_addr,
                                 uint64_t code_size,
                                 JS::JitCodeRecord* profilerRecord,
                                 AutoLockPerfSpewer& lock);
};

void CollectPerfSpewerJitCodeProfile(JitCode* code, const char* msg);
void CollectPerfSpewerJitCodeProfile(uintptr_t base, uint64_t size,
                                     const char* msg);

void CollectPerfSpewerWasmMap(uintptr_t base, uintptr_t size,
                              const char* filename, const char* annotation);
void CollectPerfSpewerWasmFunctionMap(uintptr_t base, uintptr_t size,
                                      const char* filename, unsigned lineno,
                                      const char* funcName);

class IonPerfSpewer : public PerfSpewer {
  JS::JitTier GetTier() override { return JS::JitTier::Ion; }
  const char* CodeName(unsigned op) override;

 public:
  void recordInstruction(MacroAssembler& masm, LInstruction* ins);
  void saveProfile(JSContext* cx, JSScript* script, JitCode* code);
};

class BaselineInterpreterPerfSpewer : public PerfSpewer {
  JS::JitTier GetTier() override { return JS::JitTier::Baseline; }
  const char* CodeName(unsigned op) override;

  // Do nothing, BaselineInterpreter has no source to reference.
  void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                             JS::JitCodeRecord* record,
                             AutoLockPerfSpewer& lock) override {}

 public:
  void recordOffset(MacroAssembler& masm, JSOp op);
  void recordOffset(MacroAssembler& masm, const char* name);
  void saveProfile(JitCode* code);
};

class BaselinePerfSpewer : public PerfSpewer {
  JS::JitTier GetTier() override { return JS::JitTier::Baseline; }
  const char* CodeName(unsigned op) override;

 public:
  void recordInstruction(JSContext* cx, MacroAssembler& masm, jsbytecode* pc,
                         CompilerFrameInfo& frame);
  void saveProfile(JSContext* cx, JSScript* script, JitCode* code);
};

class InlineCachePerfSpewer : public PerfSpewer {
  JS::JitTier GetTier() override { return JS::JitTier::IC; }
  const char* CodeName(unsigned op) override;

 public:
  void recordInstruction(MacroAssembler& masm, CacheOp op);
};

class BaselineICPerfSpewer : public InlineCachePerfSpewer {
  void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                             JS::JitCodeRecord* record,
                             AutoLockPerfSpewer& lock) override {
    // Baseline IC stubs are shared and have no source code to reference.
    return;
  }

 public:
  void saveProfile(JitCode* code, const char* stubName);
};

class IonICPerfSpewer : public InlineCachePerfSpewer {
 public:
  explicit IonICPerfSpewer(jsbytecode* pc);

  void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                             JS::JitCodeRecord* record,
                             AutoLockPerfSpewer& lock) override;

  void saveProfile(JSContext* cx, JSScript* script, JitCode* code,
                   const char* stubName);
};

class PerfSpewerRangeRecorder {
  using OffsetPair = std::tuple<uint32_t, UniqueChars>;
  Vector<OffsetPair, 0, js::SystemAllocPolicy> ranges;

  MacroAssembler& masm;

  void appendEntry(UniqueChars& desc);

 public:
  explicit PerfSpewerRangeRecorder(MacroAssembler& masm_) : masm(masm_){};
  void recordOffset(const char* name);
  void recordOffset(const char* name, JSContext* cx, JSScript* script);
  void recordVMWrapperOffset(const char* name);
  void collectRangesForJitCode(JitCode* code);
};

}  // namespace js::jit

#endif /* jit_PerfSpewer_h */
