/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
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

#ifndef wasm_validate_h
#define wasm_validate_h

#include <type_traits>

#include "js/WasmFeatures.h"

#include "wasm/WasmBinary.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmTypes.h"

namespace js {
namespace wasm {

// ModuleEnvironment contains all the state necessary to process or render
// functions, and all of the state necessary to validate all aspects of the
// functions.
//
// A ModuleEnvironment is created by decoding all the sections before the wasm
// code section and then used immutably during. When compiling a module using a
// ModuleGenerator, the ModuleEnvironment holds state shared between the
// ModuleGenerator thread and background compile threads. All the threads
// are given a read-only view of the ModuleEnvironment, thus preventing race
// conditions.

struct ModuleEnvironment {
  // Constant parameters for the entire compilation:
  const ModuleKind kind;
  const FeatureArgs features;

  // Module fields decoded from the module environment (or initialized while
  // validating an asm.js module) and immutable during compilation:
  Maybe<uint32_t> dataCount;
  Maybe<MemoryDesc> memory;
  TypeContext types;
  TypeIdDescVector typeIds;
  FuncDescVector funcs;
  Uint32Vector funcImportGlobalDataOffsets;

  GlobalDescVector globals;
#ifdef ENABLE_WASM_EXCEPTIONS
  EventDescVector events;
#endif
  TableDescVector tables;
  Uint32Vector asmJSSigToTableIndex;
  ImportVector imports;
  ExportVector exports;
  Maybe<uint32_t> startFuncIndex;
  ElemSegmentVector elemSegments;
  MaybeSectionRange codeSection;
  bool usesDuplicateImports;

  // Fields decoded as part of the wasm module tail:
  DataSegmentEnvVector dataSegments;
  CustomSectionEnvVector customSections;
  Maybe<uint32_t> nameCustomSectionIndex;
  Maybe<Name> moduleName;
  NameVector funcNames;

  explicit ModuleEnvironment(FeatureArgs features,
                             ModuleKind kind = ModuleKind::Wasm)
      : kind(kind),
        features(features),
        memory(Nothing()),
        types(features, TypeDefVector()),
        usesDuplicateImports(false) {}

  size_t numTables() const { return tables.length(); }
  size_t numTypes() const { return types.length(); }
  size_t numFuncs() const { return funcs.length(); }
  size_t numFuncImports() const { return funcImportGlobalDataOffsets.length(); }
  size_t numFuncDefs() const {
    return funcs.length() - funcImportGlobalDataOffsets.length();
  }

#define WASM_FEATURE(NAME, SHORT_NAME, ...) \
  bool SHORT_NAME##Enabled() const { return features.SHORT_NAME; }
  JS_FOR_WASM_FEATURES(WASM_FEATURE, WASM_FEATURE)
#undef WASM_FEATURE
  Shareable sharedMemoryEnabled() const { return features.sharedMemory; }
  bool hugeMemoryEnabled() const { return !isAsmJS() && features.hugeMemory; }
  bool simdWormholeEnabled() const { return features.simdWormhole; }

  bool isAsmJS() const { return kind == ModuleKind::AsmJS; }

  bool funcIsImport(uint32_t funcIndex) const {
    return funcIndex < funcImportGlobalDataOffsets.length();
  }

  bool usesMemory() const { return memory.isSome(); }
  bool usesSharedMemory() const {
    return memory.isSome() && memory->isShared();
  }

  void declareFuncExported(uint32_t funcIndex, bool eager, bool canRefFunc) {
    FuncFlags flags = funcs[funcIndex].flags;

    // Set the `Exported` flag, if not set.
    flags = FuncFlags(uint8_t(flags) | uint8_t(FuncFlags::Exported));

    // Merge in the `Eager` and `CanRefFunc` flags, if they're set. Be sure
    // to not unset them if they've already been set.
    if (eager) {
      flags = FuncFlags(uint8_t(flags) | uint8_t(FuncFlags::Eager));
    }
    if (canRefFunc) {
      flags = FuncFlags(uint8_t(flags) | uint8_t(FuncFlags::CanRefFunc));
    }

    funcs[funcIndex].flags = flags;
  }
};

// ElemSegmentFlags provides methods for decoding and encoding the flags field
// of an element segment. This is needed as the flags field has a non-trivial
// encoding that is effectively split into independent `kind` and `payload`
// enums.
class ElemSegmentFlags {
  enum class Flags : uint32_t {
    Passive = 0x1,
    WithIndexOrDeclared = 0x2,
    ElemExpression = 0x4,
    // Below this line are convenient combinations of flags
    KindMask = Passive | WithIndexOrDeclared,
    PayloadMask = ElemExpression,
    AllFlags = Passive | WithIndexOrDeclared | ElemExpression,
  };
  uint32_t encoded_;

  explicit ElemSegmentFlags(uint32_t encoded) : encoded_(encoded) {}

 public:
  ElemSegmentFlags(ElemSegmentKind kind, ElemSegmentPayload payload) {
    encoded_ = uint32_t(kind) | uint32_t(payload);
  }

  static Maybe<ElemSegmentFlags> construct(uint32_t encoded) {
    if (encoded > uint32_t(Flags::AllFlags)) {
      return Nothing();
    }
    return Some(ElemSegmentFlags(encoded));
  }

  uint32_t encoded() const { return encoded_; }

  ElemSegmentKind kind() const {
    return static_cast<ElemSegmentKind>(encoded_ & uint32_t(Flags::KindMask));
  }
  ElemSegmentPayload payload() const {
    return static_cast<ElemSegmentPayload>(encoded_ &
                                           uint32_t(Flags::PayloadMask));
  }
};

// OpIter specialized for validation.

class NothingVector {
  Nothing unused_;

 public:
  bool resize(size_t length) { return true; }
  Nothing& operator[](size_t) { return unused_; }
  Nothing& back() { return unused_; }
};

struct ValidatingPolicy {
  using Value = Nothing;
  using ValueVector = NothingVector;
  using ControlItem = Nothing;
};

template <typename Policy>
class OpIter;

using ValidatingOpIter = OpIter<ValidatingPolicy>;

// Shared subtyping function across validation.

[[nodiscard]] bool CheckIsSubtypeOf(Decoder& d, const ModuleEnvironment& env,
                                    size_t opcodeOffset, ValType actual,
                                    ValType expected, TypeCache* cache);

// The local entries are part of function bodies and thus serialized by both
// wasm and asm.js and decoded as part of both validation and compilation.

[[nodiscard]] bool EncodeLocalEntries(Encoder& e, const ValTypeVector& locals);

// This performs no validation; the local entries must already have been
// validated by an earlier pass.

[[nodiscard]] bool DecodeValidatedLocalEntries(Decoder& d,
                                               ValTypeVector* locals);

// This validates the entries.

[[nodiscard]] bool DecodeLocalEntries(Decoder& d, const TypeContext& types,
                                      const FeatureArgs& features,
                                      ValTypeVector* locals);

// Returns whether the given [begin, end) prefix of a module's bytecode starts a
// code section and, if so, returns the SectionRange of that code section.
// Note that, even if this function returns 'false', [begin, end) may actually
// be a valid module in the special case when there are no function defs and the
// code section is not present. Such modules can be valid so the caller must
// handle this special case.

[[nodiscard]] bool StartsCodeSection(const uint8_t* begin, const uint8_t* end,
                                     SectionRange* codeSection);

// Calling DecodeModuleEnvironment decodes all sections up to the code section
// and performs full validation of all those sections. The client must then
// decode the code section itself, reusing ValidateFunctionBody if necessary,
// and finally call DecodeModuleTail to decode all remaining sections after the
// code section (again, performing full validation).

[[nodiscard]] bool DecodeModuleEnvironment(Decoder& d, ModuleEnvironment* env);

[[nodiscard]] bool ValidateFunctionBody(const ModuleEnvironment& env,
                                        uint32_t funcIndex, uint32_t bodySize,
                                        Decoder& d);

[[nodiscard]] bool DecodeModuleTail(Decoder& d, ModuleEnvironment* env);

// Validate an entire module, returning true if the module was validated
// successfully. If Validate returns false:
//  - if *error is null, the caller should report out-of-memory
//  - otherwise, there was a legitimate error described by *error

[[nodiscard]] bool Validate(JSContext* cx, const ShareableBytes& bytecode,
                            const FeatureOptions& options, UniqueChars* error);

}  // namespace wasm
}  // namespace js

#endif  // namespace wasm_validate_h
