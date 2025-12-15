/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmCodegenTypes.h"

#include "wasm/WasmExprType.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmSummarizeInsn.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValidate.h"
#include "wasm/WasmValue.h"

using mozilla::MakeEnumeratedRange;
using mozilla::PodZero;

using namespace js;
using namespace js::wasm;

ArgTypeVector::ArgTypeVector(const FuncType& funcType)
    : args_(funcType.args()),
      hasStackResults_(ABIResultIter::HasStackResults(
          ResultType::Vector(funcType.results()))) {}

bool TrapSitesForKind::lookup(uint32_t trapInstructionOffset,
                              const InliningContext& inliningContext,
                              TrapSite* trapOut) const {
  size_t lowerBound = 0;
  size_t upperBound = pcOffsets_.length();

  size_t match;
  if (BinarySearch(pcOffsets_, lowerBound, upperBound, trapInstructionOffset,
                   &match)) {
    TrapSite site;
    site.bytecodeOffset = bytecodeOffsets_[match];
    if (auto inlinedCallerOffsetsIndex =
            inlinedCallerOffsetsMap_.lookup(match)) {
      site.inlinedCallerOffsets =
          inliningContext[inlinedCallerOffsetsIndex->value()];
    } else {
      site.inlinedCallerOffsets = nullptr;
    }
    *trapOut = site;
    return true;
  }
  return false;
}

#ifdef DEBUG
const char* wasm::ToString(Trap trap) {
  switch (trap) {
    case Trap::Unreachable:
      return "Unreachable";
    case Trap::IntegerOverflow:
      return "IntegerOverflow";
    case Trap::InvalidConversionToInteger:
      return "InvalidConversionToInteger";
    case Trap::IntegerDivideByZero:
      return "IntegerDivideByZero";
    case Trap::OutOfBounds:
      return "OutOfBounds";
    case Trap::UnalignedAccess:
      return "UnalignedAccess";
    case Trap::IndirectCallToNull:
      return "IndirectCallToNull";
    case Trap::IndirectCallBadSig:
      return "IndirectCallBadSig";
    case Trap::NullPointerDereference:
      return "NullPointerDereference";
    case Trap::BadCast:
      return "BadCast";
    case Trap::StackOverflow:
      return "StackOverflow";
    case Trap::CheckInterrupt:
      return "CheckInterrupt";
    case Trap::ThrowReported:
      return "ThrowReported";
    case Trap::Limit:
      return "Limit";
    default:
      return "Unknown";
  }
}

const char* wasm::ToString(TrapMachineInsn tmi) {
  switch (tmi) {
    case TrapMachineInsn::OfficialUD:
      return "OfficialUD";
    case TrapMachineInsn::Load8:
      return "Load8";
    case TrapMachineInsn::Load16:
      return "Load16";
    case TrapMachineInsn::Load32:
      return "Load32";
    case TrapMachineInsn::Load64:
      return "Load64";
    case TrapMachineInsn::Load128:
      return "Load128";
    case TrapMachineInsn::Store8:
      return "Store8";
    case TrapMachineInsn::Store16:
      return "Store16";
    case TrapMachineInsn::Store32:
      return "Store32";
    case TrapMachineInsn::Store64:
      return "Store64";
    case TrapMachineInsn::Store128:
      return "Store128";
    case TrapMachineInsn::Atomic:
      return "Atomic";
    default:
      return "Unknown";
  }
}
#endif  // DEBUG

void TrapSitesForKind::checkInvariants(const uint8_t* codeBase) const {
#ifdef DEBUG
  MOZ_ASSERT(machineInsns_.length() == pcOffsets_.length());
  MOZ_ASSERT(pcOffsets_.length() == bytecodeOffsets_.length());

  uint32_t last = 0;
  for (uint32_t pcOffset : pcOffsets_) {
    MOZ_ASSERT(pcOffset > last);
    last = pcOffset;
  }

#  if (defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||   \
       defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_ARM) || \
       defined(JS_CODEGEN_LOONG64) || defined(JS_CODEGEN_MIPS64))
  // Check that each trapsite is associated with a plausible instruction.  The
  // required instruction kind depends on the trapsite kind.
  //
  // NOTE: currently enabled on x86_{32,64}, arm{32,64}, loongson64 and mips64.
  // Ideally it should be extended to riscv64 too.
  //
  for (uint32_t i = 0; i < length(); i++) {
    uint32_t pcOffset = pcOffsets_[i];
    TrapMachineInsn expected = machineInsns_[i];

    const uint8_t* insnAddr = codeBase + uintptr_t(pcOffset);
    // `expected` describes the kind of instruction we expect to see at
    // `insnAddr`.  Find out what is actually there and check it matches.
    mozilla::Maybe<TrapMachineInsn> actual = SummarizeTrapInstruction(insnAddr);
    bool valid = actual.isSome() && actual.value() == expected;
    // This is useful for diagnosing validation failures.
    // if (!valid) {
    //   fprintf(stderr,
    //           "FAIL: reason=%-22s  expected=%-12s  "
    //           "pcOffset=%-5u  addr= %p\n",
    //           ToString(trap), ToString(expected),
    //           pcOffset, insnAddr);
    //   if (actual.isSome()) {
    //     fprintf(stderr, "FAIL: identified as %s\n",
    //             actual.isSome() ? ToString(actual.value())
    //                             : "(insn not identified)");
    //   }
    // }
    MOZ_ASSERT(valid, "wasm trapsite does not reference a valid insn");
  }

  for (auto iter = inlinedCallerOffsetsMap_.iter(); !iter.done(); iter.next()) {
    MOZ_ASSERT(iter.get().key() < length());
    MOZ_ASSERT(!iter.get().value().isNone());
  }
#  endif
#endif
}

CodeRange::CodeRange(Kind kind, Offsets offsets)
    : begin_(offsets.begin), ret_(0), end_(offsets.end), kind_(kind) {
  MOZ_ASSERT(begin_ <= end_);
  PodZero(&u);
#ifdef DEBUG
  switch (kind_) {
    case FarJumpIsland:
    case TrapExit:
    case Throw:
      break;
    default:
      MOZ_CRASH("should use more specific constructor");
  }
#endif
}

CodeRange::CodeRange(Kind kind, uint32_t funcIndex, Offsets offsets)
    : begin_(offsets.begin), ret_(0), end_(offsets.end), kind_(kind) {
  u.funcIndex_ = funcIndex;
  u.func.beginToUncheckedCallEntry_ = 0;
  u.func.beginToTierEntry_ = 0;
  u.func.hasUnwindInfo_ = false;
  MOZ_ASSERT(isEntry());
  MOZ_ASSERT(begin_ <= end_);
}

CodeRange::CodeRange(Kind kind, CallableOffsets offsets)
    : begin_(offsets.begin), ret_(offsets.ret), end_(offsets.end), kind_(kind) {
  MOZ_ASSERT(begin_ < ret_);
  MOZ_ASSERT(ret_ < end_);
  PodZero(&u);
#ifdef DEBUG
  switch (kind_) {
    case DebugStub:
    case BuiltinThunk:
    case RequestTierUpStub:
    case UpdateCallRefMetricsStub:
      break;
    default:
      MOZ_CRASH("should use more specific constructor");
  }
#endif
}

CodeRange::CodeRange(Kind kind, uint32_t funcIndex, CallableOffsets offsets)
    : begin_(offsets.begin), ret_(offsets.ret), end_(offsets.end), kind_(kind) {
  MOZ_ASSERT(isImportExit() || isJitEntry());
  MOZ_ASSERT(begin_ < ret_);
  MOZ_ASSERT(ret_ < end_);
  u.funcIndex_ = funcIndex;
  u.func.beginToUncheckedCallEntry_ = 0;
  u.func.beginToTierEntry_ = 0;
  u.func.hasUnwindInfo_ = false;
}

CodeRange::CodeRange(Kind kind, uint32_t funcIndex, ImportOffsets offsets)
    : begin_(offsets.begin), ret_(offsets.ret), end_(offsets.end), kind_(kind) {
  MOZ_ASSERT(isImportJitExit());
  MOZ_ASSERT(begin_ < ret_);
  MOZ_ASSERT(ret_ < end_);
  uint32_t entry = offsets.afterFallbackCheck;
  MOZ_ASSERT(begin_ <= entry && entry <= ret_);
  u.funcIndex_ = funcIndex;
  u.jitExitEntry_ = entry - begin_;
}

CodeRange::CodeRange(uint32_t funcIndex, FuncOffsets offsets,
                     bool hasUnwindInfo)
    : begin_(offsets.begin),
      ret_(offsets.ret),
      end_(offsets.end),
      kind_(Function) {
  MOZ_ASSERT(begin_ < ret_);
  MOZ_ASSERT(ret_ < end_);
  MOZ_ASSERT(offsets.uncheckedCallEntry - begin_ <= UINT16_MAX);
  MOZ_ASSERT(offsets.tierEntry - begin_ <= UINT16_MAX);
  u.funcIndex_ = funcIndex;
  u.func.beginToUncheckedCallEntry_ = offsets.uncheckedCallEntry - begin_;
  u.func.beginToTierEntry_ = offsets.tierEntry - begin_;
  u.func.hasUnwindInfo_ = hasUnwindInfo;
}

const CodeRange* wasm::LookupInSorted(const CodeRangeVector& codeRanges,
                                      CodeRange::OffsetInCode target) {
  size_t lowerBound = 0;
  size_t upperBound = codeRanges.length();

  size_t match;
  if (!BinarySearch(codeRanges, lowerBound, upperBound, target, &match)) {
    return nullptr;
  }

  return &codeRanges[match];
}

bool CallSites::lookup(uint32_t returnAddressOffset,
                       const InliningContext& inliningContext,
                       CallSite* callSite) const {
  size_t lowerBound = 0;
  size_t upperBound = returnAddressOffsets_.length();

  size_t match;
  if (BinarySearch(returnAddressOffsets_, lowerBound, upperBound,
                   returnAddressOffset, &match)) {
    *callSite = get(match, inliningContext);
    return true;
  }
  return false;
}

CallIndirectId CallIndirectId::forAsmJSFunc() {
  return CallIndirectId(CallIndirectIdKind::AsmJS);
}

CallIndirectId CallIndirectId::forFunc(const CodeMetadata& codeMeta,
                                       uint32_t funcIndex) {
  // asm.js tables are homogenous and don't require a signature check
  if (codeMeta.isAsmJS()) {
    return CallIndirectId::forAsmJSFunc();
  }

  FuncDesc func = codeMeta.funcs[funcIndex];
  if (!func.canRefFunc()) {
    return CallIndirectId();
  }
  return CallIndirectId::forFuncType(codeMeta,
                                     codeMeta.funcs[funcIndex].typeIndex);
}

CallIndirectId CallIndirectId::forFuncType(const CodeMetadata& codeMeta,
                                           uint32_t funcTypeIndex) {
  // asm.js tables are homogenous and don't require a signature check
  if (codeMeta.isAsmJS()) {
    return CallIndirectId::forAsmJSFunc();
  }

  const TypeDef& typeDef = codeMeta.types->type(funcTypeIndex);
  const FuncType& funcType = typeDef.funcType();
  CallIndirectId callIndirectId;
  if (funcType.hasImmediateTypeId()) {
    callIndirectId.kind_ = CallIndirectIdKind::Immediate;
    callIndirectId.immediate_ = funcType.immediateTypeId();
  } else {
    callIndirectId.kind_ = CallIndirectIdKind::Global;
    callIndirectId.global_.instanceDataOffset_ =
        codeMeta.offsetOfTypeDef(funcTypeIndex);
    callIndirectId.global_.hasSuperType_ = typeDef.superTypeDef() != nullptr;
  }
  return callIndirectId;
}

CalleeDesc CalleeDesc::function(uint32_t funcIndex) {
  CalleeDesc c;
  c.which_ = Func;
  c.u.funcIndex_ = funcIndex;
  return c;
}
CalleeDesc CalleeDesc::import(uint32_t instanceDataOffset) {
  CalleeDesc c;
  c.which_ = Import;
  c.u.import.instanceDataOffset_ = instanceDataOffset;
  return c;
}
CalleeDesc CalleeDesc::wasmTable(const CodeMetadata& codeMeta,
                                 const TableDesc& desc, uint32_t tableIndex,
                                 CallIndirectId callIndirectId) {
  CalleeDesc c;
  c.which_ = WasmTable;
  c.u.table.instanceDataOffset_ =
      codeMeta.offsetOfTableInstanceData(tableIndex);
  c.u.table.minLength_ = desc.initialLength();
  c.u.table.maxLength_ = desc.maximumLength();
  c.u.table.callIndirectId_ = callIndirectId;
  return c;
}
CalleeDesc CalleeDesc::asmJSTable(const CodeMetadata& codeMeta,
                                  uint32_t tableIndex) {
  CalleeDesc c;
  c.which_ = AsmJSTable;
  c.u.table.instanceDataOffset_ =
      codeMeta.offsetOfTableInstanceData(tableIndex);
  return c;
}
CalleeDesc CalleeDesc::builtin(SymbolicAddress callee) {
  CalleeDesc c;
  c.which_ = Builtin;
  c.u.builtin_ = callee;
  return c;
}
CalleeDesc CalleeDesc::builtinInstanceMethod(SymbolicAddress callee) {
  CalleeDesc c;
  c.which_ = BuiltinInstanceMethod;
  c.u.builtin_ = callee;
  return c;
}
CalleeDesc CalleeDesc::wasmFuncRef() {
  CalleeDesc c;
  c.which_ = FuncRef;
  return c;
}

void CompileStats::merge(const CompileStats& other) {
  MOZ_ASSERT(&other != this);
  numFuncs += other.numFuncs;
  bytecodeSize += other.bytecodeSize;
  inlinedDirectCallCount += other.inlinedDirectCallCount;
  inlinedCallRefCount += other.inlinedCallRefCount;
  inlinedDirectCallBytecodeSize += other.inlinedDirectCallBytecodeSize;
  inlinedCallRefBytecodeSize += other.inlinedCallRefBytecodeSize;
  numInliningBudgetOverruns += other.numInliningBudgetOverruns;
  numLargeFunctionBackoffs += other.numLargeFunctionBackoffs;
}

void CompileAndLinkStats::merge(const CompileAndLinkStats& other) {
  MOZ_ASSERT(&other != this);
  CompileStats::merge(other);
  codeBytesMapped += other.codeBytesMapped;
  codeBytesUsed += other.codeBytesUsed;
}

void CompileAndLinkStats::print() const {
#ifdef JS_JITSPEW
  // To see the statistics printed here:
  // * configure with --enable-jitspew or --enable-debug
  // * run with MOZ_LOG=wasmPerf:3
  // * this works for both JS builds and full browser builds
  JS_LOG(wasmPerf, Info, "    %7zu functions compiled", numFuncs);
  JS_LOG(wasmPerf, Info, "    %7zu bytecode bytes compiled", bytecodeSize);
  JS_LOG(wasmPerf, Info, "    %7zu direct-calls inlined",
         inlinedDirectCallCount);
  JS_LOG(wasmPerf, Info, "    %7zu call_ref-calls inlined",
         inlinedCallRefCount);
  JS_LOG(wasmPerf, Info, "    %7zu direct-call bytecodes inlined",
         inlinedDirectCallBytecodeSize);
  JS_LOG(wasmPerf, Info, "    %7zu call_ref-call bytecodes inlined",
         inlinedCallRefBytecodeSize);
  JS_LOG(wasmPerf, Info, "    %7zu functions overran inlining budget",
         numInliningBudgetOverruns);
  JS_LOG(wasmPerf, Info, "    %7zu functions needed large-function backoff",
         numLargeFunctionBackoffs);
  JS_LOG(wasmPerf, Info, "    %7zu bytes mmap'd for code storage",
         codeBytesMapped);
  JS_LOG(wasmPerf, Info, "    %7zu bytes actually used for code storage",
         codeBytesUsed);

  size_t inlinedTotalBytecodeSize =
      inlinedDirectCallBytecodeSize + inlinedCallRefBytecodeSize;

  // This value will be 0.0 if inlining did not cause any code expansion.  A
  // value of 1.0 means inlining doubled the total amount of bytecode, 2.0
  // means tripled it, etc.  Take care not to compute 0.0 / 0.0 as that is,
  // confusingly, -nan.
  float inliningExpansion =
      inlinedTotalBytecodeSize == 0
          ? 0.0
          : float(inlinedTotalBytecodeSize) / float(bytecodeSize);

  // This is always between 0.0 and 1.0.
  float codeSpaceUseRatio =
      codeBytesUsed == 0 ? 0.0 : float(codeBytesUsed) / float(codeBytesMapped);

  JS_LOG(wasmPerf, Info, "     %5.1f%% bytecode expansion caused by inlining",
         inliningExpansion * 100.0);
  JS_LOG(wasmPerf, Info, "      %4.1f%% of mapped code space used",
         codeSpaceUseRatio * 100.0);
#endif
}
