/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmMetadata_h
#define wasm_WasmMetadata_h

#include "mozilla/Atomics.h"

#include "wasm/WasmBinaryTypes.h"
#include "wasm/WasmHeuristics.h"
#include "wasm/WasmInstanceData.h"  // various of *InstanceData
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmProcess.h"  // IsHugeMemoryEnabled

namespace js {
namespace wasm {

using BuiltinModuleFuncIdVector =
    Vector<BuiltinModuleFuncId, 0, SystemAllocPolicy>;

// ==== Printing of names
//
// The Developer-Facing Display Conventions section of the WebAssembly Web
// API spec defines two cases for displaying a wasm function name:
//  1. the function name stands alone
//  2. the function name precedes the location

enum class NameContext { Standalone, BeforeLocation };

// wasm::CodeMetadata contains metadata whose lifetime ends at the same time
// that the lifetime of wasm::Code ends.  This encompasses a wide variety of
// uses.  In practice that means metadata needed for any and all aspects of
// compilation or execution of wasm code.  Hence this metadata conceptually
// belongs to, and is kept alive by, wasm::Code.  Note also that wasm::Code is
// in turn kept alive by wasm::Instance(s), hence this metadata will be kept
// alive as long as any instance for it exists.

using ModuleHash = uint8_t[8];

struct CodeMetadata : public ShareableBase<CodeMetadata> {
  // NOTE: if you add, remove, rename or reorder fields here, be sure to
  // update CodeCodeMetadata() to keep it in sync.

  // Constant parameters for the entire compilation.  These are not marked
  // `const` only because it breaks constructor delegation in
  // CodeMetadata::CodeMetadata, which is a shame.
  ModuleKind kind;

  // The compile arguments that were used for this module.
  SharedCompileArgs compileArgs;

  // The number of imported functions in the module.
  uint32_t numFuncImports;
  // A vector of the builtin func id (or 'none') for all imported functions.
  // This may be empty for internally constructed modules which don't care
  // about this information.
  BuiltinModuleFuncIdVector knownFuncImports;
  // Treat imported wasm functions as if they were JS functions. This is used
  // when compiling the module for new WebAssembly.Function.
  bool funcImportsAreJS;
  // The number of imported globals in the module.
  uint32_t numGlobalImports;

  // Info about all types in the module.
  MutableTypeContext types;
  // Info about all functions in the module.
  FuncDescVector funcs;
  // Info about all tables in the module.
  TableDescVector tables;
  // Info about all memories in the module.
  MemoryDescVector memories;
  // Info about all tags in the module.
  TagDescVector tags;
  // Info about all globals in the module.
  GlobalDescVector globals;

  // The start function for the module, if any
  mozilla::Maybe<uint32_t> startFuncIndex;

  // Info about elem segments needed only for validation and compilation.
  // Should have the same length as ModuleMetadata::elemSegments, and each
  // entry here should be identical the corresponding .elemType field in
  // ModuleMetadata::elemSegments.
  RefTypeVector elemSegmentTypes;

  // The number of data segments this module will have. Pre-declared before the
  // code section so that we can validate instructions that reference data
  // segments.
  mozilla::Maybe<uint32_t> dataCount;

  // A sorted vector of the index of every function that is exported from this
  // module. An index into this vector is a 'exported function index' and can
  // be used to lookup exported functions on an instance.
  Uint32Vector exportedFuncIndices;

  // asm.js tables are homogenous and only store functions of the same type.
  // This maps from a function type to the table index to use for an indirect
  // call.
  Uint32Vector asmJSSigToTableIndex;

  // Branch hints to apply to functions
  BranchHintCollection branchHints;

  // Name section information
  mozilla::Maybe<NameSection> nameSection;

  // Bytecode ranges for custom sections.
  CustomSectionRangeVector customSectionRanges;

  // Bytecode range for the code section.
  MaybeBytecodeRange codeSectionRange;

  // ==== Instance layout fields
  //
  // The start offset of the FuncDefInstanceData[] section of the instance
  // data. There is one entry for every function definition.
  uint32_t funcDefsOffsetStart;
  // The start offset of the FuncImportInstanceData[] section of the instance
  // data. There is one entry for every imported function.
  uint32_t funcImportsOffsetStart;
  // The start offset of the FuncExportInstanceData[] section of the instance
  // data. There is one entry for every exported function.
  uint32_t funcExportsOffsetStart;
  // The start offset of the TypeDefInstanceData[] section of the instance
  // data. There is one entry for every type.
  uint32_t typeDefsOffsetStart;
  // The start offset of the MemoryInstanceData[] section of the instance data.
  // There is one entry for every memory.
  uint32_t memoriesOffsetStart;
  // The start offset of the TableInstanceData[] section of the instance data.
  // There is one entry for every table.
  uint32_t tablesOffsetStart;
  // The start offset of the tag section of the instance data. There is one
  // entry for every tag.
  uint32_t tagsOffsetStart;
  // The total size of the instance data.
  uint32_t instanceDataLength;

  explicit CodeMetadata(const CompileArgs* compileArgs = nullptr,
                        ModuleKind kind = ModuleKind::Wasm)
      : kind(kind),
        compileArgs(compileArgs),
        numFuncImports(0),
        funcImportsAreJS(false),
        numGlobalImports(0),
        funcDefsOffsetStart(UINT32_MAX),
        funcImportsOffsetStart(UINT32_MAX),
        funcExportsOffsetStart(UINT32_MAX),
        typeDefsOffsetStart(UINT32_MAX),
        memoriesOffsetStart(UINT32_MAX),
        tablesOffsetStart(UINT32_MAX),
        tagsOffsetStart(UINT32_MAX),
        instanceDataLength(UINT32_MAX) {}

  [[nodiscard]] bool init() {
    MOZ_ASSERT(!types);
    types = js_new<TypeContext>();
    return types;
  }

  // Generates any new metadata necessary to compile this module. This must be
  // called after the 'module environment' (everything before the code section)
  // has been decoded.
  [[nodiscard]] bool prepareForCompile(CompileMode mode);
  bool isPreparedForCompile() const { return instanceDataLength != UINT32_MAX; }

  bool isAsmJS() const { return kind == ModuleKind::AsmJS; }
  // A builtin module is a host constructed wasm module that exports host
  // functionality, using special opcodes. Otherwise, it has the same rules
  // as wasm modules and so it does not get a new ModuleKind.
  bool isBuiltinModule() const { return features().isBuiltinModule; }

#define WASM_FEATURE(NAME, SHORT_NAME, ...) \
  bool SHORT_NAME##Enabled() const { return features().SHORT_NAME; }
  JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE
  Shareable sharedMemoryEnabled() const { return features().sharedMemory; }
  bool simdAvailable() const { return features().simd; }

  bool hugeMemoryEnabled(uint32_t memoryIndex) const {
    return !isAsmJS() && memoryIndex < memories.length() &&
           IsHugeMemoryEnabled(memories[memoryIndex].addressType());
  }
  bool usesSharedMemory(uint32_t memoryIndex) const {
    return memoryIndex < memories.length() && memories[memoryIndex].isShared();
  }

  const FeatureArgs& features() const { return compileArgs->features; }
  const ScriptedCaller& scriptedCaller() const {
    return compileArgs->scriptedCaller;
  }
  const UniqueChars& sourceMapURL() const { return compileArgs->sourceMapURL; }

  size_t numTypes() const { return types->length(); }
  size_t numFuncs() const { return funcs.length(); }
  size_t numFuncDefs() const { return funcs.length() - numFuncImports; }
  size_t numTables() const { return tables.length(); }
  size_t numMemories() const { return memories.length(); }

  bool funcIsImport(uint32_t funcIndex) const {
    return funcIndex < numFuncImports;
  }
  const TypeDef& getFuncTypeDef(uint32_t funcIndex) const {
    return types->type(funcs[funcIndex].typeIndex);
  }
  const FuncType& getFuncType(uint32_t funcIndex) const {
    return getFuncTypeDef(funcIndex).funcType();
  }

  BuiltinModuleFuncId knownFuncImport(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex < numFuncImports);
    if (knownFuncImports.empty()) {
      return BuiltinModuleFuncId::None;
    }
    return knownFuncImports[funcIndex];
  }

  // Find the exported function index for a function index
  uint32_t findFuncExportIndex(uint32_t funcIndex) const;

  // The number of functions that are exported in this module
  uint32_t numExportedFuncs() const { return exportedFuncIndices.length(); }

  size_t codeSectionSize() const {
    if (codeSectionRange) {
      return codeSectionRange->size();
    }
    return 0;
  }

  // This gets names for wasm only.
  // For asm.js, see CodeMetadataForAsmJS::getFuncNameForAsmJS.
  bool getFuncNameForWasm(NameContext ctx, uint32_t funcIndex,
                          const ShareableBytes* nameSectionPayload,
                          UTF8Bytes* name) const;

  uint32_t offsetOfFuncDefInstanceData(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex >= numFuncImports && funcIndex < numFuncs());
    return funcDefsOffsetStart +
           (funcIndex - numFuncImports) * sizeof(FuncDefInstanceData);
  }

  uint32_t offsetOfFuncImportInstanceData(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex < numFuncImports);
    return funcImportsOffsetStart + funcIndex * sizeof(FuncImportInstanceData);
  }

  uint32_t offsetOfFuncExportInstanceData(uint32_t funcExportIndex) const {
    MOZ_ASSERT(funcExportIndex < exportedFuncIndices.length());
    return funcExportsOffsetStart +
           funcExportIndex * sizeof(FuncExportInstanceData);
  }

  uint32_t offsetOfTypeDefInstanceData(uint32_t typeIndex) const {
    MOZ_ASSERT(typeIndex < types->length());
    return typeDefsOffsetStart + typeIndex * sizeof(TypeDefInstanceData);
  }

  uint32_t offsetOfTypeDef(uint32_t typeIndex) const {
    return offsetOfTypeDefInstanceData(typeIndex) +
           offsetof(TypeDefInstanceData, typeDef);
  }
  uint32_t offsetOfSuperTypeVector(uint32_t typeIndex) const {
    return offsetOfTypeDefInstanceData(typeIndex) +
           offsetof(TypeDefInstanceData, superTypeVector);
  }

  uint32_t offsetOfMemoryInstanceData(uint32_t memoryIndex) const {
    MOZ_ASSERT(memoryIndex < memories.length());
    return memoriesOffsetStart + memoryIndex * sizeof(MemoryInstanceData);
  }
  uint32_t offsetOfTableInstanceData(uint32_t tableIndex) const {
    MOZ_ASSERT(tableIndex < tables.length());
    return tablesOffsetStart + tableIndex * sizeof(TableInstanceData);
  }

  uint32_t offsetOfTagInstanceData(uint32_t tagIndex) const {
    MOZ_ASSERT(tagIndex < tags.length());
    return tagsOffsetStart + tagIndex * sizeof(TagInstanceData);
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

 private:
  // Allocate space for `bytes`@`align` in the instance, updating
  // `instanceDataLength` and returning the offset in in `assignedOffset`.
  [[nodiscard]] bool allocateInstanceDataBytes(uint32_t bytes, uint32_t align,
                                               uint32_t* assignedOffset);
  // The same for an array of allocations.
  [[nodiscard]] bool allocateInstanceDataBytesN(uint32_t bytes, uint32_t align,
                                                uint32_t count,
                                                uint32_t* assignedOffset);
};

using MutableCodeMetadata = RefPtr<CodeMetadata>;
using SharedCodeMetadata = RefPtr<const CodeMetadata>;

using InliningBudget = ExclusiveData<int64_t>;

// wasm::CodeTailMetadata contains all metadata needed by wasm::Code that is
// only after the whole module has been downloaded. It is sometimes available
// for Ion compilation, and never available for baseline compilation.
struct CodeTailMetadata : public ShareableBase<CodeTailMetadata> {
  // Default initializer only used for serialization.
  CodeTailMetadata();

  // Initialize the metadata with a given wasm::CodeMetadata.
  explicit CodeTailMetadata(const CodeMetadata& codeMeta);

  // The code metadata for this module.
  SharedCodeMetadata codeMeta;

  // The bytes for the code section.
  SharedBytes codeSectionBytecode;

  // Whether this module was compiled with debugging support.
  bool debugEnabled;

  // A SHA-1 hash of the module bytecode for use in display urls. Only
  // available if we're debugging.
  ModuleHash debugHash;

  // The full bytecode for this module. Only available for debuggable modules.
  BytecodeBuffer debugBytecode;

  // Shared and mutable inlining budget for this module.
  mutable InliningBudget inliningBudget;

  // The ranges of every function defined in this module.
  BytecodeRangeVector funcDefRanges;

  // The feature usage for every function defined in this module.
  FeatureUsageVector funcDefFeatureUsages;

  // Tracks the range of CallRefMetrics created for each function definition in
  // this module.
  CallRefMetricsRangeVector funcDefCallRefs;

  // Tracks the range of AllocSites created for each function definition in
  // this module. If we are compiling with a 'once' compilation and using just
  // ion, this will be empty and ion will instead use per-typedef alloc sites.
  AllocSitesRangeVector funcDefAllocSites;

  // An array of hints to use when compiling a call_ref.
  //
  // This is written into when an instance requests a function to be tiered up,
  // and read from our function compilers.
  MutableCallRefHints callRefHints;

  // nameSectionPayload points at the name section's CustomSection::payload so
  // that the Names (which are use payload-relative offsets) can be used
  // independently of the Module without duplicating the name section.
  SharedBytes nameSectionPayload;

  // The number of call ref metrics in Instance::callRefs_
  uint32_t numCallRefMetrics;

  // The number of AllocSites in Instance::allocSites_.
  uint32_t numAllocSites;

  // Given a bytecode offset inside a function definition, find the function
  // index.
  uint32_t findFuncIndex(uint32_t bytecodeOffset) const;
  uint32_t funcBytecodeOffset(uint32_t funcIndex) const {
    if (funcIndex < codeMeta->numFuncImports) {
      return 0;
    }
    uint32_t funcDefIndex = funcIndex - codeMeta->numFuncImports;
    return funcDefRanges[funcDefIndex].start;
  }
  const BytecodeRange& funcDefRange(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex >= codeMeta->numFuncImports);
    uint32_t funcDefIndex = funcIndex - codeMeta->numFuncImports;
    return funcDefRanges[funcDefIndex];
  }
  BytecodeSpan funcDefBody(uint32_t funcIndex) const {
    return funcDefRange(funcIndex)
        .relativeTo(*codeMeta->codeSectionRange)
        .toSpan(*codeSectionBytecode);
  }
  FeatureUsage funcDefFeatureUsage(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex >= codeMeta->numFuncImports);
    uint32_t funcDefIndex = funcIndex - codeMeta->numFuncImports;
    return funcDefFeatureUsages[funcDefIndex];
  }

  CallRefMetricsRange getFuncDefCallRefs(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex >= codeMeta->numFuncImports);
    uint32_t funcDefIndex = funcIndex - codeMeta->numFuncImports;
    return funcDefCallRefs[funcDefIndex];
  }

  AllocSitesRange getFuncDefAllocSites(uint32_t funcIndex) const {
    MOZ_ASSERT(funcIndex >= codeMeta->numFuncImports);
    uint32_t funcDefIndex = funcIndex - codeMeta->numFuncImports;
    return funcDefAllocSites[funcDefIndex];
  }

  bool hasFuncDefAllocSites() const { return !funcDefAllocSites.empty(); }

  CallRefHint getCallRefHint(uint32_t callRefIndex) const {
    if (!callRefHints) {
      return CallRefHint();
    }
    return CallRefHint::fromRepr(callRefHints[callRefIndex]);
  }
  void setCallRefHint(uint32_t callRefIndex, CallRefHint hint) const {
    callRefHints[callRefIndex] = hint.toRepr();
  }
};

using MutableCodeTailMetadata = RefPtr<CodeTailMetadata>;
using SharedCodeTailMetadata = RefPtr<const CodeTailMetadata>;

// wasm::ModuleMetadata contains metadata whose lifetime ends at the same time
// that the lifetime of wasm::Module ends.  In practice that means metadata
// that is needed only for creating wasm::Instances.  Hence this metadata
// conceptually belongs to, and is held alive by, wasm::Module.

struct ModuleMetadata : public ShareableBase<ModuleMetadata> {
  // NOTE: if you add, remove, rename or reorder fields here, be sure to
  // update CodeModuleMetadata() to keep it in sync.

  // The subset of module metadata that is shared between a module and
  // instance (i.e. wasm::Code), and is available for all compilation. It
  // does not contain any data that is only available after the whole module
  // has been downloaded.
  MutableCodeMetadata codeMeta;

  // The subset of module metadata that is shared between a module and
  // instance (i.e. wasm::Code), and is only available after the whole module
  // has been downloaded.
  MutableCodeTailMetadata codeTailMeta;

  // Module fields decoded from the module environment (or initialized while
  // validating an asm.js module) and immutable during compilation:
  ImportVector imports;
  ExportVector exports;

  // Info about elem segments needed for instantiation.  Should have the same
  // length as CodeMetadata::elemSegmentTypes.
  ModuleElemSegmentVector elemSegments;

  // Info about data segments needed for instantiation.  These wind up having
  // the same length.  Initially both are empty.  `dataSegmentRanges` is
  // filled in during validation, and `dataSegments` remains empty.  Later, at
  // module-generation time, `dataSegments` is filled in, by copying the
  // underlying data blocks, and so the two vectors have the same length after
  // that.
  DataSegmentRangeVector dataSegmentRanges;
  DataSegmentVector dataSegments;

  CustomSectionVector customSections;

  // Which features were observed when compiling this module.
  FeatureUsage featureUsage = FeatureUsage::None;

  explicit ModuleMetadata() = default;

  [[nodiscard]] bool init(const CompileArgs& compileArgs,
                          ModuleKind kind = ModuleKind::Wasm) {
    codeMeta = js_new<CodeMetadata>(&compileArgs, kind);
    return !!codeMeta && codeMeta->init();
  }

  bool addDefinedFunc(ValTypeVector&& params, ValTypeVector&& results,
                      bool declareForRef = false,
                      mozilla::Maybe<CacheableName>&& optionalExportedName =
                          mozilla::Nothing());
  bool addImportedFunc(ValTypeVector&& params, ValTypeVector&& results,
                       CacheableName&& importModName,
                       CacheableName&& importFieldName);

  // Generates any new metadata necessary to compile this module. This must be
  // called after the 'module environment' (everything before the code section)
  // has been decoded.
  [[nodiscard]] bool prepareForCompile(CompileMode mode) {
    return codeMeta->prepareForCompile(mode);
  }
  bool isPreparedForCompile() const { return codeMeta->isPreparedForCompile(); }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

using MutableModuleMetadata = RefPtr<ModuleMetadata>;
using SharedModuleMetadata = RefPtr<const ModuleMetadata>;

}  // namespace wasm
}  // namespace js

#endif /* wasm_WasmMetadata_h */
