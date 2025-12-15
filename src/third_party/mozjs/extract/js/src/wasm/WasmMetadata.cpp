/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmMetadata.h"

#include "mozilla/BinarySearch.h"
#include "mozilla/CheckedInt.h"

#include "jsnum.h"  // Int32ToCStringBuf

#include "vm/Logging.h"

using mozilla::CheckedInt;

using namespace js;
using namespace js::wasm;

// CodeMetadata helpers -- computing the Instance layout.

bool CodeMetadata::allocateInstanceDataBytes(uint32_t bytes, uint32_t align,
                                             uint32_t* assignedOffset) {
  // Assert that this offset hasn't already been computed.
  MOZ_ASSERT(*assignedOffset == UINT32_MAX);

  CheckedInt<uint32_t> newInstanceDataLength(instanceDataLength);

  // Adjust the current global data length so that it's aligned to `align`
  newInstanceDataLength +=
      ComputeByteAlignment(newInstanceDataLength.value(), align);
  if (!newInstanceDataLength.isValid()) {
    return false;
  }

  // The allocated data is given by the aligned length
  *assignedOffset = newInstanceDataLength.value();

  // Advance the length for `bytes` being allocated
  newInstanceDataLength += bytes;
  if (!newInstanceDataLength.isValid()) {
    return false;
  }

  // This is the highest offset into Instance::globalArea that will not
  // overflow a signed 32-bit integer.
  const uint32_t maxInstanceDataOffset =
      uint32_t(INT32_MAX) - uint32_t(Instance::offsetOfData());

  // Check that the highest offset into this allocated space would not overflow
  // a signed 32-bit integer.
  if (newInstanceDataLength.value() > maxInstanceDataOffset + 1) {
    return false;
  }

  instanceDataLength = newInstanceDataLength.value();
  return true;
}

bool CodeMetadata::allocateInstanceDataBytesN(uint32_t bytes, uint32_t align,
                                              uint32_t count,
                                              uint32_t* assignedOffset) {
  // The size of each allocation should be a multiple of alignment so that a
  // contiguous array of allocations will be aligned
  MOZ_ASSERT(bytes % align == 0);

  // Compute the total bytes being allocated
  CheckedInt<uint32_t> totalBytes = bytes;
  totalBytes *= count;
  if (!totalBytes.isValid()) {
    return false;
  }

  // Allocate the bytes
  return allocateInstanceDataBytes(totalBytes.value(), align, assignedOffset);
}

bool CodeMetadata::prepareForCompile(CompileMode mode) {
  MOZ_ASSERT(!isPreparedForCompile());

  // Find every function that is exported from this module and give it an
  // implicit index
  uint32_t exportedFuncCount = 0;
  for (uint32_t funcIndex = 0; funcIndex < funcs.length(); funcIndex++) {
    const FuncDesc& func = funcs[funcIndex];
    if (func.isExported()) {
      exportedFuncCount++;
    }
  }

  if (!exportedFuncIndices.reserve(exportedFuncCount)) {
    return false;
  }
  for (uint32_t funcIndex = 0; funcIndex < funcs.length(); funcIndex++) {
    const FuncDesc& func = funcs[funcIndex];
    if (!func.isExported()) {
      continue;
    }
    exportedFuncIndices.infallibleEmplaceBack(funcIndex);
  }

  // Allocate the layout for instance data
  instanceDataLength = 0;

  // Allocate space for function counters, if we have them
  if (mode == CompileMode::LazyTiering) {
    if (!allocateInstanceDataBytesN(sizeof(FuncDefInstanceData),
                                    alignof(FuncDefInstanceData), numFuncDefs(),
                                    &funcDefsOffsetStart)) {
      return false;
    }
  }

  // Allocate space for type definitions
  if (!allocateInstanceDataBytesN(sizeof(TypeDefInstanceData),
                                  alignof(TypeDefInstanceData), types->length(),
                                  &typeDefsOffsetStart)) {
    return false;
  }

  // Allocate space for every function import
  if (!allocateInstanceDataBytesN(sizeof(FuncImportInstanceData),
                                  alignof(FuncImportInstanceData),
                                  numFuncImports, &funcImportsOffsetStart)) {
    return false;
  }

  // Allocate space for every function export
  if (!allocateInstanceDataBytesN(
          sizeof(FuncExportInstanceData), alignof(FuncExportInstanceData),
          numExportedFuncs(), &funcExportsOffsetStart)) {
    return false;
  }

  // Allocate space for every memory
  if (!allocateInstanceDataBytesN(sizeof(MemoryInstanceData),
                                  alignof(MemoryInstanceData),
                                  memories.length(), &memoriesOffsetStart)) {
    return false;
  }

  // Allocate space for every table
  if (!allocateInstanceDataBytesN(sizeof(TableInstanceData),
                                  alignof(TableInstanceData), tables.length(),
                                  &tablesOffsetStart)) {
    return false;
  }

  // Allocate space for every tag
  if (!allocateInstanceDataBytesN(sizeof(TagInstanceData),
                                  alignof(TagInstanceData), tags.length(),
                                  &tagsOffsetStart)) {
    return false;
  }

  // Allocate space for every global that requires it
  for (GlobalDesc& global : globals) {
    if (global.isConstant()) {
      continue;
    }

    uint32_t width = global.isIndirect() ? sizeof(void*) : global.type().size();

    uint32_t assignedOffset = UINT32_MAX;
    if (!allocateInstanceDataBytes(width, width, &assignedOffset)) {
      return false;
    }

    global.setOffset(assignedOffset);
  }

  return true;
}

uint32_t CodeMetadata::findFuncExportIndex(uint32_t funcIndex) const {
  MOZ_ASSERT(funcs[funcIndex].isExported());

  size_t match;
  if (!mozilla::BinarySearch(exportedFuncIndices, 0,
                             exportedFuncIndices.length(), funcIndex, &match)) {
    MOZ_CRASH("missing function export");
  }
  return (uint32_t)match;
}

uint32_t CodeTailMetadata::findFuncIndex(uint32_t bytecodeOffset) const {
  size_t funcDefIndex;
  if (!mozilla::BinarySearchIf(
          funcDefRanges, 0, funcDefRanges.length(),
          [bytecodeOffset](const BytecodeRange& range) {
            return range.compareOffset(bytecodeOffset);
          },
          &funcDefIndex)) {
    MOZ_CRASH("missing function definition");
  }
  return codeMeta->numFuncImports + funcDefIndex;
}

// CodeMetadata helpers -- getting function names.

static bool AppendName(const Bytes& namePayload, const Name& name,
                       UTF8Bytes* bytes) {
  MOZ_RELEASE_ASSERT(name.offsetInNamePayload <= namePayload.length());
  MOZ_RELEASE_ASSERT(name.length <=
                     namePayload.length() - name.offsetInNamePayload);
  return bytes->append(
      (const char*)namePayload.begin() + name.offsetInNamePayload, name.length);
}

static bool AppendFunctionIndexName(uint32_t funcIndex, UTF8Bytes* bytes) {
  const char beforeFuncIndex[] = "wasm-function[";
  const char afterFuncIndex[] = "]";

  Int32ToCStringBuf cbuf;
  size_t funcIndexStrLen;
  const char* funcIndexStr =
      Uint32ToCString(&cbuf, funcIndex, &funcIndexStrLen);
  MOZ_ASSERT(funcIndexStr);

  return bytes->append(beforeFuncIndex, strlen(beforeFuncIndex)) &&
         bytes->append(funcIndexStr, funcIndexStrLen) &&
         bytes->append(afterFuncIndex, strlen(afterFuncIndex));
}

bool CodeMetadata::getFuncNameForWasm(NameContext ctx, uint32_t funcIndex,
                                      const ShareableBytes* nameSectionPayload,
                                      UTF8Bytes* name) const {
  if (nameSection && nameSection->moduleName.length != 0) {
    if (!AppendName(nameSectionPayload->vector, nameSection->moduleName,
                    name)) {
      return false;
    }
    if (!name->append('.')) {
      return false;
    }
  }

  if (nameSection && funcIndex < nameSection->funcNames.length() &&
      nameSection->funcNames[funcIndex].length != 0) {
    return AppendName(nameSectionPayload->vector,
                      nameSection->funcNames[funcIndex], name);
  }

  if (ctx == NameContext::BeforeLocation) {
    return true;
  }

  return AppendFunctionIndexName(funcIndex, name);
}

// CodeMetadata helpers -- memory accounting.

size_t CodeMetadata::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return memories.sizeOfExcludingThis(mallocSizeOf) +
         types->sizeOfExcludingThis(mallocSizeOf) +
         globals.sizeOfExcludingThis(mallocSizeOf) +
         tags.sizeOfExcludingThis(mallocSizeOf) +
         tables.sizeOfExcludingThis(mallocSizeOf) +
         SizeOfMaybeExcludingThis(nameSection, mallocSizeOf) +
         funcs.sizeOfExcludingThis(mallocSizeOf) +
         elemSegmentTypes.sizeOfExcludingThis(mallocSizeOf) +
         asmJSSigToTableIndex.sizeOfExcludingThis(mallocSizeOf) +
         customSectionRanges.sizeOfExcludingThis(mallocSizeOf);
}

CodeTailMetadata::CodeTailMetadata()
    : codeMeta(nullptr),
      debugEnabled(false),
      debugHash(),
      inliningBudget(mutexid::WasmInliningBudget, 0),
      callRefHints(nullptr),
      numCallRefMetrics(UINT32_MAX),
      numAllocSites(UINT32_MAX) {}

CodeTailMetadata::CodeTailMetadata(const CodeMetadata& codeMeta)
    : js::wasm::CodeTailMetadata() {
  this->codeMeta = &codeMeta;
  inliningBudget.lock().get() =
      InliningHeuristics::moduleInliningBudget(codeMeta.codeSectionSize());
}

// ModuleMetadata helpers -- memory accounting.

size_t ModuleMetadata::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return imports.sizeOfExcludingThis(mallocSizeOf) +
         exports.sizeOfExcludingThis(mallocSizeOf) +
         elemSegments.sizeOfExcludingThis(mallocSizeOf) +
         dataSegmentRanges.sizeOfExcludingThis(mallocSizeOf) +
         dataSegments.sizeOfExcludingThis(mallocSizeOf) +
         customSections.sizeOfExcludingThis(mallocSizeOf);
}

bool ModuleMetadata::addDefinedFunc(
    ValTypeVector&& params, ValTypeVector&& results, bool declareForRef,
    mozilla::Maybe<CacheableName>&& optionalExportedName) {
  uint32_t typeIndex = codeMeta->types->length();
  FuncType funcType(std::move(params), std::move(results));
  if (!codeMeta->types->addType(std::move(funcType))) {
    return false;
  }

  FuncDesc funcDesc = FuncDesc(typeIndex);
  uint32_t funcIndex = codeMeta->funcs.length();
  if (!codeMeta->funcs.append(funcDesc)) {
    return false;
  }
  if (declareForRef) {
    codeMeta->funcs[funcIndex].declareFuncExported(true, true);
  }
  if (optionalExportedName.isSome()) {
    if (!exports.emplaceBack(std::move(optionalExportedName.ref()), funcIndex,
                             DefinitionKind::Function)) {
      return false;
    }
  }
  return true;
}

bool ModuleMetadata::addImportedFunc(ValTypeVector&& params,
                                     ValTypeVector&& results,
                                     CacheableName&& importModName,
                                     CacheableName&& importFieldName) {
  MOZ_ASSERT(codeMeta->numFuncImports == codeMeta->funcs.length());
  if (!addDefinedFunc(std::move(params), std::move(results), false,
                      mozilla::Nothing())) {
    return false;
  }
  codeMeta->numFuncImports++;
  return imports.emplaceBack(std::move(importModName),
                             std::move(importFieldName),
                             DefinitionKind::Function);
}
