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
#include "js/AllocPolicy.h"
#include "js/Vector.h"

class JSScript;
enum class JSOp : uint8_t;

namespace js {

namespace wasm {
struct OpBytes;
}

namespace jit {

class JitCode;
class CompilerFrameInfo;
class MacroAssembler;
class MBasicBlock;
class LInstruction;
enum class CacheOp : uint16_t;

struct AutoLockPerfSpewer {
  AutoLockPerfSpewer();
  ~AutoLockPerfSpewer();
};

bool PerfEnabled();

class PerfSpewer {
 protected:
  struct OpcodeEntry {
    uint32_t offset = 0;
    uint32_t opcode = 0;
    jsbytecode* bytecodepc = nullptr;

    // This string is used to replace the opcode, to define things like
    // Prologue/Epilogue, or to add operand info.
    JS::UniqueChars str;

    explicit OpcodeEntry(uint32_t offset_, uint32_t opcode_,
                         JS::UniqueChars& str_, jsbytecode* pc)
        : offset(offset_), opcode(opcode_), bytecodepc(pc) {
      str = std::move(str_);
    }

    explicit OpcodeEntry(uint32_t offset_, uint32_t opcode_,
                         JS::UniqueChars& str_)
        : offset(offset_), opcode(opcode_) {
      str = std::move(str_);
    }
    explicit OpcodeEntry(uint32_t offset_, JS::UniqueChars& str_)
        : offset(offset_) {
      str = std::move(str_);
    }
    explicit OpcodeEntry(uint32_t offset_, uint32_t opcode_)
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
  uint32_t startOffset_ = 0;

  virtual const char* CodeName(uint32_t op) = 0;

  virtual void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                                     AutoLockPerfSpewer& lock);

  void saveJitCodeDebugInfo(JSScript* script, JitCode* code,
                            AutoLockPerfSpewer& lock);
  void saveWasmCodeDebugInfo(uintptr_t codeBase, AutoLockPerfSpewer& lock);

  void saveJSProfile(JitCode* code, JS::UniqueChars& desc, JSScript* script);
  void saveWasmProfile(uintptr_t codeBase, size_t codeSize,
                       JS::UniqueChars& desc);

  void saveIRInfo(uintptr_t codeBase, AutoLockPerfSpewer& lock);

 public:
  PerfSpewer() = default;
  PerfSpewer(PerfSpewer&&) = default;
  PerfSpewer& operator=(PerfSpewer&&) = default;

  void markStartOffset(uint32_t offset) { startOffset_ = offset; }
  void recordOffset(MacroAssembler& masm, const char*);

  static void Init();

  static void CollectJitCodeInfo(JS::UniqueChars& function_name, JitCode* code,
                                 AutoLockPerfSpewer& lock);
  static void CollectJitCodeInfo(JS::UniqueChars& function_name,
                                 void* code_addr, uint64_t code_size,
                                 AutoLockPerfSpewer& lock);
};

void CollectPerfSpewerJitCodeProfile(JitCode* code, const char* msg);
void CollectPerfSpewerJitCodeProfile(uintptr_t base, uint64_t size,
                                     const char* msg);

void CollectPerfSpewerWasmMap(uintptr_t base, uintptr_t size,
                              JS::UniqueChars&& desc);

class IonPerfSpewer : public PerfSpewer {
  const char* CodeName(uint32_t op) override;

 public:
  IonPerfSpewer() = default;
  IonPerfSpewer(IonPerfSpewer&&) = default;
  IonPerfSpewer& operator=(IonPerfSpewer&&) = default;

  void recordInstruction(MacroAssembler& masm, LInstruction* ins);
  void saveJSProfile(JSContext* cx, JSScript* script, JitCode* code);
  void saveWasmProfile(uintptr_t codeBase, size_t codeSize,
                       JS::UniqueChars& desc);
};

class WasmBaselinePerfSpewer : public PerfSpewer {
  const char* CodeName(uint32_t op) override;

 public:
  WasmBaselinePerfSpewer() = default;
  WasmBaselinePerfSpewer(WasmBaselinePerfSpewer&&) = default;
  WasmBaselinePerfSpewer& operator=(WasmBaselinePerfSpewer&&) = default;

  [[nodiscard]] bool needsToRecordInstruction() const;
  void recordInstruction(MacroAssembler& masm, const wasm::OpBytes& op);
  void saveProfile(uintptr_t codeBase, size_t codeSize, JS::UniqueChars& desc);
};

class BaselineInterpreterPerfSpewer : public PerfSpewer {
  const char* CodeName(uint32_t op) override;

  // Do nothing, BaselineInterpreter has no source to reference.
  void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
                             AutoLockPerfSpewer& lock) override {}

 public:
  void recordOffset(MacroAssembler& masm, const JSOp& op);
  void recordOffset(MacroAssembler& masm, const char* name);
  void saveProfile(JitCode* code);
};

class BaselinePerfSpewer : public PerfSpewer {
  const char* CodeName(uint32_t op) override;

 public:
  void recordInstruction(MacroAssembler& masm, jsbytecode* pc,
                         CompilerFrameInfo& frame);
  void saveProfile(JSContext* cx, JSScript* script, JitCode* code);
};

class InlineCachePerfSpewer : public PerfSpewer {
  const char* CodeName(uint32_t op) override;

 public:
  void recordInstruction(MacroAssembler& masm, const CacheOp& op);
};

class BaselineICPerfSpewer : public InlineCachePerfSpewer {
  void saveJitCodeSourceInfo(JSScript* script, JitCode* code,
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
                             AutoLockPerfSpewer& lock) override;

  void saveProfile(JSContext* cx, JSScript* script, JitCode* code,
                   const char* stubName);
};

class PerfSpewerRangeRecorder {
  using OffsetPair = std::tuple<uint32_t, JS::UniqueChars>;
  Vector<OffsetPair, 0, js::SystemAllocPolicy> ranges;

  MacroAssembler& masm;

  void appendEntry(JS::UniqueChars& desc);

 public:
  explicit PerfSpewerRangeRecorder(MacroAssembler& masm_) : masm(masm_) {};
  void recordOffset(const char* name);
  void recordOffset(const char* name, JSContext* cx, JSScript* script);
  void recordVMWrapperOffset(const char* name);
  void collectRangesForJitCode(JitCode* code);
};

}  // namespace jit
}  // namespace js

#endif /* jit_PerfSpewer_h */
