/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2018 Mozilla Foundation
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

#include "wasm/WasmCraneliftCompile.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/ScopeExit.h"

#include "jit/Disassemble.h"
#include "js/Printf.h"
#include "vm/JSContext.h"

#include "wasm/cranelift/baldrapi.h"
#include "wasm/cranelift/clifapi.h"
#include "wasm/WasmFrameIter.h"  // js::wasm::GenerateFunction{Pro,Epi}logue
#include "wasm/WasmGC.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmStubs.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedInt;

bool wasm::CraneliftPlatformSupport() { return cranelift_supports_platform(); }

static inline SymbolicAddress ToSymbolicAddress(BD_SymbolicAddress bd) {
  switch (bd) {
    case BD_SymbolicAddress::RefFunc:
      return SymbolicAddress::RefFunc;
    case BD_SymbolicAddress::MemoryGrow:
      return SymbolicAddress::MemoryGrow;
    case BD_SymbolicAddress::MemorySize:
      return SymbolicAddress::MemorySize;
    case BD_SymbolicAddress::MemoryCopy:
      return SymbolicAddress::MemCopy32;
    case BD_SymbolicAddress::MemoryCopyShared:
      return SymbolicAddress::MemCopyShared32;
    case BD_SymbolicAddress::DataDrop:
      return SymbolicAddress::DataDrop;
    case BD_SymbolicAddress::MemoryFill:
      return SymbolicAddress::MemFill32;
    case BD_SymbolicAddress::MemoryFillShared:
      return SymbolicAddress::MemFillShared32;
    case BD_SymbolicAddress::MemoryInit:
      return SymbolicAddress::MemInit32;
    case BD_SymbolicAddress::TableCopy:
      return SymbolicAddress::TableCopy;
    case BD_SymbolicAddress::ElemDrop:
      return SymbolicAddress::ElemDrop;
    case BD_SymbolicAddress::TableFill:
      return SymbolicAddress::TableFill;
    case BD_SymbolicAddress::TableGet:
      return SymbolicAddress::TableGet;
    case BD_SymbolicAddress::TableGrow:
      return SymbolicAddress::TableGrow;
    case BD_SymbolicAddress::TableInit:
      return SymbolicAddress::TableInit;
    case BD_SymbolicAddress::TableSet:
      return SymbolicAddress::TableSet;
    case BD_SymbolicAddress::TableSize:
      return SymbolicAddress::TableSize;
    case BD_SymbolicAddress::FloorF32:
      return SymbolicAddress::FloorF;
    case BD_SymbolicAddress::FloorF64:
      return SymbolicAddress::FloorD;
    case BD_SymbolicAddress::CeilF32:
      return SymbolicAddress::CeilF;
    case BD_SymbolicAddress::CeilF64:
      return SymbolicAddress::CeilD;
    case BD_SymbolicAddress::NearestF32:
      return SymbolicAddress::NearbyIntF;
    case BD_SymbolicAddress::NearestF64:
      return SymbolicAddress::NearbyIntD;
    case BD_SymbolicAddress::TruncF32:
      return SymbolicAddress::TruncF;
    case BD_SymbolicAddress::TruncF64:
      return SymbolicAddress::TruncD;
    case BD_SymbolicAddress::PreBarrier:
      return SymbolicAddress::PreBarrierFiltering;
    case BD_SymbolicAddress::PostBarrier:
      return SymbolicAddress::PostBarrierFiltering;
    case BD_SymbolicAddress::WaitI32:
      return SymbolicAddress::WaitI32;
    case BD_SymbolicAddress::WaitI64:
      return SymbolicAddress::WaitI64;
    case BD_SymbolicAddress::Wake:
      return SymbolicAddress::Wake;
    case BD_SymbolicAddress::Limit:
      break;
  }
  MOZ_CRASH("unknown baldrdash symbolic address");
}

static bool GenerateCraneliftCode(
    WasmMacroAssembler& masm, const CraneliftCompiledFunc& func,
    const FuncType& funcType, const TypeIdDesc& funcTypeId,
    uint32_t lineOrBytecode, uint32_t funcBytecodeSize, StackMaps* stackMaps,
    size_t stackMapsOffset, size_t stackMapsCount, FuncOffsets* offsets) {
  wasm::GenerateFunctionPrologue(masm, funcTypeId, mozilla::Nothing(), offsets);

  // Omit the check when framePushed is small and we know there's no
  // recursion.
  if (func.frame_pushed < MAX_UNCHECKED_LEAF_FRAME_SIZE &&
      !func.contains_calls) {
    masm.reserveStack(func.frame_pushed);
  } else {
    std::pair<CodeOffset, uint32_t> pair = masm.wasmReserveStackChecked(
        func.frame_pushed, BytecodeOffset(lineOrBytecode));
    CodeOffset trapInsnOffset = pair.first;
    size_t nBytesReservedBeforeTrap = pair.second;

    MachineState trapExitLayout;
    size_t trapExitLayoutNumWords;
    GenerateTrapExitMachineState(&trapExitLayout, &trapExitLayoutNumWords);

    size_t nInboundStackArgBytes = StackArgAreaSizeUnaligned(funcType.args());

    ArgTypeVector args(funcType);
    wasm::StackMap* functionEntryStackMap = nullptr;
    if (!CreateStackMapForFunctionEntryTrap(
            args, trapExitLayout, trapExitLayoutNumWords,
            nBytesReservedBeforeTrap, nInboundStackArgBytes,
            &functionEntryStackMap)) {
      return false;
    }

    // In debug builds, we'll always have a stackmap, even if there are no
    // refs to track.
    MOZ_ASSERT(functionEntryStackMap);

    if (functionEntryStackMap &&
        !stackMaps->add((uint8_t*)(uintptr_t)trapInsnOffset.offset(),
                        functionEntryStackMap)) {
      functionEntryStackMap->destroy();
      return false;
    }
  }
  MOZ_ASSERT(masm.framePushed() == func.frame_pushed);

  // Copy the machine code; handle jump tables and other read-only data below.
  uint32_t funcBase = masm.currentOffset();
  if (func.code_size && !masm.appendRawCode(func.code, func.code_size)) {
    return false;
  }
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  uint32_t codeEnd = masm.currentOffset();
#endif

  wasm::GenerateFunctionEpilogue(masm, func.frame_pushed, offsets);

  if (func.num_rodata_relocs > 0) {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
    constexpr size_t jumptableElementSize = 4;

    MOZ_ASSERT(func.jumptables_size % jumptableElementSize == 0);

    // Align the jump tables properly.
    masm.haltingAlign(jumptableElementSize);

    // Copy over the tables and read-only data.
    uint32_t rodataBase = masm.currentOffset();
    if (!masm.appendRawCode(func.code + func.code_size,
                            func.total_size - func.code_size)) {
      return false;
    }

    uint32_t numElem = func.jumptables_size / jumptableElementSize;
    uint32_t bias = rodataBase - codeEnd;

    // Bias the jump table(s).  The table values are negative values
    // representing backward jumps.  By shifting the table down we increase the
    // distance and so we add a negative value to reflect the larger distance.
    //
    // Note addToPCRel4() works from the end of the instruction, hence the loop
    // bounds.
    for (uint32_t i = 1; i <= numElem; i++) {
      masm.addToPCRel4(rodataBase + (i * jumptableElementSize), -bias);
    }

    // Patch up the code locations.  These represent forward distances that also
    // become greater, so we add a positive value.
    for (uint32_t i = 0; i < func.num_rodata_relocs; i++) {
      MOZ_ASSERT(func.rodata_relocs[i] < func.code_size);
      masm.addToPCRel4(funcBase + func.rodata_relocs[i], bias);
    }
#else
    MOZ_CRASH("No jump table support on this platform");
#endif
  }

  masm.flush();
  if (masm.oom()) {
    return false;
  }
  offsets->end = masm.currentOffset();

  for (size_t i = 0; i < stackMapsCount; i++) {
    auto* maplet = stackMaps->getRef(stackMapsOffset + i);
    maplet->offsetBy(funcBase);
  }

  for (size_t i = 0; i < func.num_metadata; i++) {
    const CraneliftMetadataEntry& metadata = func.metadatas[i];

    CheckedInt<size_t> offset = funcBase;
    offset += metadata.code_offset;
    if (!offset.isValid()) {
      return false;
    }

#ifdef DEBUG
    // Check code offsets.
    MOZ_ASSERT(offset.value() >= offsets->uncheckedCallEntry);
    MOZ_ASSERT(offset.value() < offsets->ret);
    MOZ_ASSERT(metadata.module_bytecode_offset != 0);

    // Check bytecode offsets.
    if (lineOrBytecode > 0) {
      MOZ_ASSERT(metadata.module_bytecode_offset >= lineOrBytecode);
      MOZ_ASSERT(metadata.module_bytecode_offset <
                 lineOrBytecode + funcBytecodeSize);
    }
#endif
    uint32_t bytecodeOffset = metadata.module_bytecode_offset;

    switch (metadata.which) {
      case CraneliftMetadataEntry::Which::DirectCall: {
        CallSiteDesc desc(bytecodeOffset, CallSiteDesc::Func);
        masm.append(desc, CodeOffset(offset.value()), metadata.extra);
        break;
      }
      case CraneliftMetadataEntry::Which::IndirectCall: {
        CallSiteDesc desc(bytecodeOffset, CallSiteDesc::Dynamic);
        masm.append(desc, CodeOffset(offset.value()));
        break;
      }
      case CraneliftMetadataEntry::Which::Trap: {
        Trap trap = (Trap)metadata.extra;
        BytecodeOffset trapOffset(bytecodeOffset);
        masm.append(trap, wasm::TrapSite(offset.value(), trapOffset));
        break;
      }
      case CraneliftMetadataEntry::Which::SymbolicAccess: {
        CodeOffset raOffset(offset.value());
        CallSiteDesc desc(bytecodeOffset, CallSiteDesc::Symbolic);
        masm.append(desc, raOffset);

        SymbolicAddress sym =
            ToSymbolicAddress(BD_SymbolicAddress(metadata.extra));
        masm.append(SymbolicAccess(raOffset, sym));
        break;
      }
      default: {
        MOZ_CRASH("unknown cranelift metadata kind");
      }
    }
  }

  return true;
}

// In Rust, a BatchCompiler variable has a lifetime constrained by those of its
// associated StaticEnvironment and ModuleEnvironment. This RAII class ties
// them together, as well as makes sure that the compiler is properly destroyed
// when it exits scope.

class CraneliftContext {
  CraneliftStaticEnvironment staticEnv_;
  CraneliftModuleEnvironment moduleEnv_;
  CraneliftCompiler* compiler_;

 public:
  explicit CraneliftContext(const ModuleEnvironment& moduleEnv)
      : moduleEnv_(moduleEnv), compiler_(nullptr) {
    staticEnv_.ref_types_enabled = true;
    staticEnv_.threads_enabled = true;
    staticEnv_.v128_enabled = moduleEnv.v128Enabled();
#ifdef WASM_SUPPORTS_HUGE_MEMORY
    if (moduleEnv.hugeMemoryEnabled()) {
      // In the huge memory configuration, we always reserve the full 4 GB
      // index space for a heap.
      staticEnv_.static_memory_bound = HugeIndexRange;
      staticEnv_.memory_guard_size = HugeOffsetGuardLimit;
    } else {
      staticEnv_.memory_guard_size = OffsetGuardLimit;
    }
#endif
    // Otherwise, heap bounds are stored in the `boundsCheckLimit` field
    // of TlsData.
  }
  bool init() {
    compiler_ = cranelift_compiler_create(&staticEnv_, &moduleEnv_);
    return !!compiler_;
  }
  ~CraneliftContext() {
    if (compiler_) {
      cranelift_compiler_destroy(compiler_);
    }
  }
  operator CraneliftCompiler*() { return compiler_; }
};

CraneliftFuncCompileInput::CraneliftFuncCompileInput(
    const FuncCompileInput& func)
    : bytecode(func.begin),
      bytecode_size(func.end - func.begin),
      index(func.index),
      offset_in_module(func.lineOrBytecode) {}

static_assert(offsetof(TlsData, boundsCheckLimit) == sizeof(void*),
              "fix make_heap() in wasm2clif.rs");

CraneliftStaticEnvironment::CraneliftStaticEnvironment()
    :
#ifdef JS_CODEGEN_X64
      has_sse2(Assembler::HasSSE2()),
      has_sse3(Assembler::HasSSE3()),
      has_sse41(Assembler::HasSSE41()),
      has_sse42(Assembler::HasSSE42()),
      has_popcnt(Assembler::HasPOPCNT()),
      has_avx(Assembler::HasAVX()),
      has_bmi1(Assembler::HasBMI1()),
      has_bmi2(Assembler::HasBMI2()),
      has_lzcnt(Assembler::HasLZCNT()),
#else
      has_sse2(false),
      has_sse3(false),
      has_sse41(false),
      has_sse42(false),
      has_popcnt(false),
      has_avx(false),
      has_bmi1(false),
      has_bmi2(false),
      has_lzcnt(false),
#endif
#if defined(XP_WIN)
      platform_is_windows(true),
#else
      platform_is_windows(false),
#endif
      ref_types_enabled(false),
      threads_enabled(false),
      v128_enabled(false),
      static_memory_bound(0),
      memory_guard_size(0),
      memory_base_tls_offset(offsetof(TlsData, memoryBase)),
      instance_tls_offset(offsetof(TlsData, instance)),
      interrupt_tls_offset(offsetof(TlsData, interrupt)),
      cx_tls_offset(offsetof(TlsData, cx)),
      realm_cx_offset(JSContext::offsetOfRealm()),
      realm_tls_offset(offsetof(TlsData, realm)),
      realm_func_import_tls_offset(offsetof(FuncImportTls, realm)),
      size_of_wasm_frame(sizeof(wasm::Frame)) {
}

// Most of BaldrMonkey's data structures refer to a "global offset" which is a
// byte offset into the `globalArea` field of the  `TlsData` struct.
//
// Cranelift represents global variables with their byte offset from the "VM
// context pointer" which is the `WasmTlsReg` pointing to the `TlsData`
// struct.
//
// This function translates between the two.

static size_t globalToTlsOffset(size_t globalOffset) {
  return offsetof(wasm::TlsData, globalArea) + globalOffset;
}

CraneliftModuleEnvironment::CraneliftModuleEnvironment(
    const ModuleEnvironment& env)
    : env(&env) {
  // env.minMemoryLength is in bytes.  Convert it to wasm pages.
  static_assert(sizeof(env.minMemoryLength) == 8);
  MOZ_RELEASE_ASSERT(env.minMemoryLength <= (((uint64_t)1) << 32));
  MOZ_RELEASE_ASSERT((env.minMemoryLength & wasm::PageMask) == 0);
  min_memory_length = (uint32_t)(env.minMemoryLength >> wasm::PageBits);
}

TypeCode env_unpack(BD_ValType valType) {
  return PackedTypeCode::fromBits(valType.packed).typeCode();
}

size_t env_num_datas(const CraneliftModuleEnvironment* env) {
  return env->env->dataCount.valueOr(0);
}

size_t env_num_elems(const CraneliftModuleEnvironment* env) {
  return env->env->elemSegments.length();
}
TypeCode env_elem_typecode(const CraneliftModuleEnvironment* env,
                           uint32_t index) {
  return env->env->elemSegments[index]->elemType.packed().typeCode();
}

// Returns a number of pages in the range [0..65536], or UINT32_MAX to signal
// that no maximum has been set.
uint32_t env_max_memory(const CraneliftModuleEnvironment* env) {
  // env.maxMemoryLength is in bytes.  Convert it to wasm pages.
  if (env->env->maxMemoryLength.isSome()) {
    // We use |auto| here rather than |uint64_t| so that the static_assert will
    // fail if |maxMemoryLength| is changed to some other size.
    auto inBytes = *(env->env->maxMemoryLength);
    static_assert(sizeof(inBytes) == 8);
    MOZ_RELEASE_ASSERT(inBytes <= (((uint64_t)1) << 32));
    MOZ_RELEASE_ASSERT((inBytes & wasm::PageMask) == 0);
    return (uint32_t)(inBytes >> wasm::PageBits);
  }
  return UINT32_MAX;
}

bool env_uses_shared_memory(const CraneliftModuleEnvironment* env) {
  return env->env->usesSharedMemory();
}

bool env_has_memory(const CraneliftModuleEnvironment* env) {
  return env->env->usesMemory();
}

size_t env_num_types(const CraneliftModuleEnvironment* env) {
  return env->env->types.length();
}
const FuncType* env_type(const CraneliftModuleEnvironment* env,
                         size_t typeIndex) {
  return &env->env->types[typeIndex].funcType();
}

size_t env_num_funcs(const CraneliftModuleEnvironment* env) {
  return env->env->funcs.length();
}
const FuncType* env_func_sig(const CraneliftModuleEnvironment* env,
                             size_t funcIndex) {
  return env->env->funcs[funcIndex].type;
}
const TypeIdDesc* env_func_sig_id(const CraneliftModuleEnvironment* env,
                                  size_t funcIndex) {
  return env->env->funcs[funcIndex].typeId;
}
size_t env_func_sig_index(const CraneliftModuleEnvironment* env,
                          size_t funcIndex) {
  return env->env->funcs[funcIndex].typeIndex;
}
bool env_is_func_valid_for_ref(const CraneliftModuleEnvironment* env,
                               uint32_t index) {
  return env->env->funcs[index].canRefFunc();
}

size_t env_func_import_tls_offset(const CraneliftModuleEnvironment* env,
                                  size_t funcIndex) {
  return globalToTlsOffset(env->env->funcImportGlobalDataOffsets[funcIndex]);
}

bool env_func_is_import(const CraneliftModuleEnvironment* env,
                        size_t funcIndex) {
  return env->env->funcIsImport(funcIndex);
}

const FuncType* env_signature(const CraneliftModuleEnvironment* env,
                              size_t funcTypeIndex) {
  return &env->env->types[funcTypeIndex].funcType();
}

const TypeIdDesc* env_signature_id(const CraneliftModuleEnvironment* env,
                                   size_t funcTypeIndex) {
  return &env->env->typeIds[funcTypeIndex];
}

size_t env_num_tables(const CraneliftModuleEnvironment* env) {
  return env->env->tables.length();
}
const TableDesc* env_table(const CraneliftModuleEnvironment* env,
                           size_t tableIndex) {
  return &env->env->tables[tableIndex];
}

size_t env_num_globals(const CraneliftModuleEnvironment* env) {
  return env->env->globals.length();
}
const GlobalDesc* env_global(const CraneliftModuleEnvironment* env,
                             size_t globalIndex) {
  return &env->env->globals[globalIndex];
}

bool wasm::CraneliftCompileFunctions(const ModuleEnvironment& moduleEnv,
                                     const CompilerEnvironment& compilerEnv,
                                     LifoAlloc& lifo,
                                     const FuncCompileInputVector& inputs,
                                     CompiledCode* code, UniqueChars* error) {
  MOZ_RELEASE_ASSERT(CraneliftPlatformSupport());

  MOZ_ASSERT(compilerEnv.tier() == Tier::Optimized);
  MOZ_ASSERT(compilerEnv.debug() == DebugEnabled::False);
  MOZ_ASSERT(compilerEnv.optimizedBackend() == OptimizedBackend::Cranelift);
  MOZ_ASSERT(!moduleEnv.isAsmJS());

  TempAllocator alloc(&lifo);
  JitContext jitContext(&alloc);
  WasmMacroAssembler masm(alloc, moduleEnv);
  MOZ_ASSERT(IsCompilingWasm());

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());

  CraneliftReusableData reusableContext;
  if (!code->swapCranelift(masm, reusableContext)) {
    return false;
  }

  if (!reusableContext) {
    auto context = MakeUnique<CraneliftContext>(moduleEnv);
    if (!context || !context->init()) {
      return false;
    }
    reusableContext.reset((void**)context.release());
  }

  CraneliftContext* compiler = (CraneliftContext*)reusableContext.get();

  // Disable instruction spew if we're going to disassemble after code
  // generation, or the output will be a mess.

  bool jitSpew = JitSpewEnabled(js::jit::JitSpew_Codegen);
  if (jitSpew) {
    DisableChannel(js::jit::JitSpew_Codegen);
  }
  auto reenableSpew = mozilla::MakeScopeExit([&] {
    if (jitSpew) {
      EnableChannel(js::jit::JitSpew_Codegen);
    }
  });

  for (const FuncCompileInput& func : inputs) {
    Decoder d(func.begin, func.end, func.lineOrBytecode, error);

    size_t funcBytecodeSize = func.end - func.begin;

    size_t previousStackmapCount = code->stackMaps.length();

    CraneliftFuncCompileInput clifInput(func);
    clifInput.stackmaps = (BD_Stackmaps*)&code->stackMaps;

    CraneliftCompiledFunc clifFunc;

    char* clifError = nullptr;
    if (!cranelift_compile_function(*compiler, &clifInput, &clifFunc,
                                    &clifError)) {
      *error = JS_smprintf("%s", clifError);
      cranelift_compiler_free_error(clifError);
      return false;
    }

    uint32_t lineOrBytecode = func.lineOrBytecode;
    const FuncType& funcType = *moduleEnv.funcs[clifInput.index].type;
    const TypeIdDesc& funcTypeId = *moduleEnv.funcs[clifInput.index].typeId;

    FuncOffsets offsets;
    if (!GenerateCraneliftCode(
            masm, clifFunc, funcType, funcTypeId, lineOrBytecode,
            funcBytecodeSize, &code->stackMaps, previousStackmapCount,
            code->stackMaps.length() - previousStackmapCount, &offsets)) {
      return false;
    }

    if (!code->codeRanges.emplaceBack(func.index, lineOrBytecode, offsets)) {
      return false;
    }
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  if (jitSpew) {
    // The disassembler uses the jitspew for output, so re-enable now.
    EnableChannel(js::jit::JitSpew_Codegen);

    uint32_t totalCodeSize = masm.currentOffset();
    uint8_t* codeBuf = (uint8_t*)js_malloc(totalCodeSize);
    if (codeBuf) {
      masm.executableCopy(codeBuf);

      const CodeRangeVector& codeRanges = code->codeRanges;
      MOZ_ASSERT(codeRanges.length() >= inputs.length());

      // Within the current batch, functions' code ranges have been added in
      // the same order as the inputs.
      size_t firstCodeRangeIndex = codeRanges.length() - inputs.length();

      for (size_t i = 0; i < inputs.length(); i++) {
        int funcIndex = inputs[i].index;

        JitSpew(JitSpew_Codegen, "# ========================================");
        JitSpew(JitSpew_Codegen, "# Start of wasm cranelift code for index %d",
                funcIndex);

        size_t codeRangeIndex = firstCodeRangeIndex + i;
        uint32_t codeStart = codeRanges[codeRangeIndex].begin();
        uint32_t codeEnd = codeRanges[codeRangeIndex].end();

        jit::Disassemble(
            codeBuf + codeStart, codeEnd - codeStart,
            [](const char* text) { JitSpew(JitSpew_Codegen, "%s", text); });

        JitSpew(JitSpew_Codegen, "# End of wasm cranelift code for index %d",
                funcIndex);
      }
      js_free(codeBuf);
    }
  }

  return code->swapCranelift(masm, reusableContext);
}

void wasm::CraneliftFreeReusableData(void* ptr) {
  CraneliftContext* compiler = (CraneliftContext*)ptr;
  if (compiler) {
    js_delete(compiler);
  }
}

////////////////////////////////////////////////////////////////////////////////
//
// Callbacks from Rust to C++.

// Offsets assumed by the `make_heap()` function.
static_assert(offsetof(wasm::TlsData, memoryBase) == 0, "memory base moved");

// The translate_call() function in wasm2clif.rs depends on these offsets.
static_assert(offsetof(wasm::FuncImportTls, code) == 0,
              "Import code field moved");
static_assert(offsetof(wasm::FuncImportTls, tls) == sizeof(void*),
              "Import tls moved");

// Global

bool global_isConstant(const GlobalDesc* global) {
  return global->isConstant();
}

bool global_isMutable(const GlobalDesc* global) { return global->isMutable(); }

bool global_isIndirect(const GlobalDesc* global) {
  return global->isIndirect();
}

BD_ConstantValue global_constantValue(const GlobalDesc* global) {
  Val value(global->constantValue());
  BD_ConstantValue v;
  v.t = TypeCode(value.type().kind());
  switch (v.t) {
    case TypeCode::I32:
      v.u.i32 = value.i32();
      break;
    case TypeCode::I64:
      v.u.i64 = value.i64();
      break;
    case TypeCode::F32:
      v.u.f32 = value.f32();
      break;
    case TypeCode::F64:
      v.u.f64 = value.f64();
      break;
    case TypeCode::V128:
      memcpy(&v.u.v128, &value.v128(), sizeof(v.u.v128));
      break;
    case AbstractReferenceTypeCode:
      v.u.r = value.ref().forCompiledCode();
      break;
    default:
      MOZ_CRASH("Bad type");
  }
  return v;
}

TypeCode global_type(const GlobalDesc* global) {
  return global->type().packed().typeCode();
}

size_t global_tlsOffset(const GlobalDesc* global) {
  return globalToTlsOffset(global->offset());
}

// TableDesc

size_t table_tlsOffset(const TableDesc* table) {
  return globalToTlsOffset(table->globalDataOffset);
}

uint32_t table_initialLimit(const TableDesc* table) {
  return table->initialLength;
}
uint32_t table_maximumLimit(const TableDesc* table) {
  return table->maximumLength.valueOr(UINT32_MAX);
}
TypeCode table_elementTypeCode(const TableDesc* table) {
  return table->elemType.packed().typeCode();
}

// Sig

size_t funcType_numArgs(const FuncType* funcType) {
  return funcType->args().length();
}

const BD_ValType* funcType_args(const FuncType* funcType) {
  static_assert(sizeof(BD_ValType) == sizeof(ValType), "update BD_ValType");
  return (const BD_ValType*)funcType->args().begin();
}

size_t funcType_numResults(const FuncType* funcType) {
  return funcType->results().length();
}

const BD_ValType* funcType_results(const FuncType* funcType) {
  static_assert(sizeof(BD_ValType) == sizeof(ValType), "update BD_ValType");
  return (const BD_ValType*)funcType->results().begin();
}

TypeIdDescKind funcType_idKind(const TypeIdDesc* funcTypeId) {
  return funcTypeId->kind();
}

size_t funcType_idImmediate(const TypeIdDesc* funcTypeId) {
  return funcTypeId->immediate();
}

size_t funcType_idTlsOffset(const TypeIdDesc* funcTypeId) {
  return globalToTlsOffset(funcTypeId->globalDataOffset());
}

void stackmaps_add(BD_Stackmaps* sink, const uint32_t* bitMap,
                   size_t mappedWords, size_t argsSize, size_t codeOffset) {
  const uint32_t BitElemSize = sizeof(uint32_t) * 8;

  StackMaps* maps = (StackMaps*)sink;
  StackMap* map = StackMap::create(mappedWords);
  MOZ_ALWAYS_TRUE(map);

  // Copy the cranelift stackmap into our spidermonkey one
  // TODO: Take ownership of the cranelift stackmap and avoid a copy
  for (uint32_t i = 0; i < mappedWords; i++) {
    uint32_t bit = (bitMap[i / BitElemSize] >> (i % BitElemSize)) & 0x1;
    if (bit) {
      map->setBit(i);
    }
  }

  map->setFrameOffsetFromTop((argsSize + sizeof(wasm::Frame)) /
                             sizeof(uintptr_t));
  MOZ_ALWAYS_TRUE(maps->add((uint8_t*)codeOffset, map));
}
