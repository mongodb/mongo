/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2023 Mozilla Foundation
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

#ifndef wasm_dump_h
#define wasm_dump_h

#include "js/Printer.h"

#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmTypeDef.h"
#include "wasm/WasmValType.h"

namespace js {
namespace wasm {

extern void DumpModule(const Module& module);
extern void DumpModule(const Module& module, GenericPrinter& out);

extern void DumpValType(ValType type, const TypeContext* types = nullptr);
extern void DumpValType(ValType type, GenericPrinter& out,
                        const TypeContext* types = nullptr);

extern void DumpStorageType(StorageType type,
                            const TypeContext* types = nullptr);
extern void DumpStorageType(StorageType type, GenericPrinter& out,
                            const TypeContext* types = nullptr);

extern void DumpRefType(RefType type, const TypeContext* types = nullptr);
extern void DumpRefType(RefType type, GenericPrinter& out,
                        const TypeContext* types = nullptr);

extern void DumpHeapType(RefType type, const TypeContext* types = nullptr);
extern void DumpHeapType(RefType type, GenericPrinter& out,
                         const TypeContext* types = nullptr);

extern void DumpFuncType(const FuncType& funcType,
                         const TypeContext* types = nullptr);
extern void DumpFuncType(const FuncType& funcType, StructuredPrinter& out,
                         const TypeContext* types = nullptr);

extern void DumpStructType(const StructType& structType,
                           const TypeContext* types = nullptr);
extern void DumpStructType(const StructType& structType, StructuredPrinter& out,
                           const TypeContext* types = nullptr);

extern void DumpArrayType(const ArrayType& arrayType,
                          const TypeContext* types = nullptr);
extern void DumpArrayType(const ArrayType& arrayType, StructuredPrinter& out,
                          const TypeContext* types = nullptr);

extern void DumpTypeDef(const TypeDef& typeDef, int32_t index = -1,
                        const TypeContext* types = nullptr);
extern void DumpTypeDef(const TypeDef& typeDef, StructuredPrinter& out,
                        int32_t index = -1, const TypeContext* types = nullptr);

extern void DumpRecGroup(const RecGroup& recGroup, int32_t startTypeIndex = -1,
                         const TypeContext* types = nullptr);
extern void DumpRecGroup(const RecGroup& recGroup, StructuredPrinter& out,
                         int32_t startTypeIndex = -1,
                         const TypeContext* types = nullptr);

extern void DumpTableDesc(const TableDesc& tableDesc,
                          const CodeMetadata& codeMeta, bool includeInitExpr,
                          int32_t index = -1);
extern void DumpTableDesc(const TableDesc& tableDesc,
                          const CodeMetadata& codeMeta, bool includeInitExpr,
                          StructuredPrinter& out, int32_t index = -1);

extern void DumpMemoryDesc(const MemoryDesc& memDesc, int32_t index = -1);
extern void DumpMemoryDesc(const MemoryDesc& memDesc, StructuredPrinter& out,
                           int32_t index = -1);

extern void DumpGlobalDesc(const GlobalDesc& globalDesc,
                           const CodeMetadata& codeMeta, bool includeInitExpr,
                           int32_t index = -1);
extern void DumpGlobalDesc(const GlobalDesc& globalDesc,
                           const CodeMetadata& codeMeta, bool includeInitExpr,
                           StructuredPrinter& out, int32_t index = -1);

extern void DumpTagDesc(const TagDesc& tagDesc, int32_t index = -1,
                        const TypeContext* types = nullptr);
extern void DumpTagDesc(const TagDesc& tagDesc, StructuredPrinter& out,
                        int32_t index = -1, const TypeContext* types = nullptr);

extern void DumpInitExpr(const InitExpr& initExpr,
                         const CodeMetadata& codeMeta);
extern void DumpInitExpr(const InitExpr& initExpr, const CodeMetadata& codeMeta,
                         StructuredPrinter& out);

extern void DumpTypeContext(const TypeContext& typeContext);
extern void DumpTypeContext(const TypeContext& typeContext,
                            StructuredPrinter& out);

extern void DumpFunction(const CodeMetadata& codeMeta,
                         const CodeTailMetadata& codeTailMeta,
                         uint32_t funcIndex);
extern void DumpFunction(const CodeMetadata& codeMeta,
                         const CodeTailMetadata& codeTailMeta,
                         uint32_t funcIndex, StructuredPrinter& out);

extern void DumpFunctionBody(const CodeMetadata& codeMeta, uint32_t funcIndex,
                             const uint8_t* bodyStart, uint32_t bodySize);
extern void DumpFunctionBody(const CodeMetadata& codeMeta, uint32_t funcIndex,
                             const uint8_t* bodyStart, uint32_t bodySize,
                             StructuredPrinter& out);

struct OpDumper {
  StructuredPrinter& out;
  const TypeContext* types;
  int numOps = 0;
  explicit OpDumper(StructuredPrinter& out, const TypeContext* types = nullptr)
      : out(out), types(types) {}

  void dumpOpBegin(OpBytes op) {
    out.brk(" ", "\n");
    out.put(op.toString());
    numOps += 1;
    if (numOps > 1) {
      out.expand();
    }
  }
  void dumpOpEnd() {}
  void dumpTypeIndex(uint32_t typeIndex, bool asTypeUse = false) {
    if (asTypeUse) {
      out.put(" (type");
    }
    out.printf(" %" PRIu32, typeIndex);
    if (asTypeUse) {
      out.put(")");
    }
  }
  void dumpFuncIndex(uint32_t funcIndex) { out.printf(" %" PRIu32, funcIndex); }
  void dumpTableIndex(uint32_t tableIndex) {
    out.printf(" %" PRIu32, tableIndex);
  }
  void dumpGlobalIndex(uint32_t globalIndex) {
    out.printf(" %" PRIu32, globalIndex);
  }
  void dumpMemoryIndex(uint32_t memoryIndex) {
    out.printf(" %" PRIu32, memoryIndex);
  }
  void dumpElemIndex(uint32_t elemIndex) { out.printf(" %" PRIu32, elemIndex); }
  void dumpDataIndex(uint32_t dataIndex) { out.printf(" %" PRIu32, dataIndex); }
  void dumpTagIndex(uint32_t tagIndex) { out.printf(" %" PRIu32, tagIndex); }
  void dumpLocalIndex(uint32_t localIndex) {
    out.printf(" %" PRIu32, localIndex);
  }
  void dumpBlockType(BlockType type) {
    if (type.params().length() > 0) {
      out.put(" (param");
      for (uint32_t i = 0; i < type.params().length(); i++) {
        dumpValType(type.params()[i]);
      }
      out.put(")");
    }
    if (type.results().length() > 0) {
      out.put(" (result");
      for (uint32_t i = 0; i < type.results().length(); i++) {
        dumpValType(type.results()[i]);
      }
      out.put(")");
    }
  }
  void dumpI32Const(int32_t constant) { out.printf(" %" PRId32, constant); }
  void dumpI64Const(int64_t constant) { out.printf(" %" PRId64, constant); }
  void dumpF32Const(float constant) { out.printf(" %f", constant); }
  void dumpF64Const(double constant) { out.printf(" %lf", constant); }
  void dumpV128Const(V128 constant) {
    out.printf("i8x16 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
               constant.bytes[0], constant.bytes[1], constant.bytes[2],
               constant.bytes[3], constant.bytes[4], constant.bytes[5],
               constant.bytes[6], constant.bytes[7], constant.bytes[8],
               constant.bytes[9], constant.bytes[10], constant.bytes[11],
               constant.bytes[12], constant.bytes[13], constant.bytes[14],
               constant.bytes[15]);
  }
  void dumpVectorMask(V128 mask) {
    out.printf("%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", mask.bytes[0],
               mask.bytes[1], mask.bytes[2], mask.bytes[3], mask.bytes[4],
               mask.bytes[5], mask.bytes[6], mask.bytes[7], mask.bytes[8],
               mask.bytes[9], mask.bytes[10], mask.bytes[11], mask.bytes[12],
               mask.bytes[13], mask.bytes[14], mask.bytes[15]);
  }
  void dumpRefType(RefType type) {
    out.put(" ");
    wasm::DumpRefType(type, out, types);
  }
  void dumpHeapType(RefType type) {
    out.put(" ");
    wasm::DumpHeapType(type, out, types);
  }
  void dumpValType(ValType type) {
    out.put(" ");
    wasm::DumpValType(type, out, types);
  }
  void dumpTryTableCatches(const TryTableCatchVector& catches) {
    for (uint32_t i = 0; i < catches.length(); i++) {
      const TryTableCatch& tryCatch = catches[i];
      if (tryCatch.tagIndex == CatchAllIndex) {
        if (tryCatch.captureExnRef) {
          out.put(" (catch_all_ref ");
        } else {
          out.put(" (catch_all ");
        }
      } else {
        if (tryCatch.captureExnRef) {
          out.printf(" (catch_ref %d ", tryCatch.tagIndex);
        } else {
          out.printf(" (catch %d ", tryCatch.tagIndex);
        }
      }
      dumpBlockDepth(tryCatch.labelRelativeDepth);
      out.put(")");
    }
  }
  void dumpLinearMemoryAddress(LinearMemoryAddress<mozilla::Nothing> addr) {
    if (addr.memoryIndex != 0) {
      out.printf(" %d", addr.memoryIndex);
    }
    if (addr.offset != 0) {
      out.printf(" offset=%" PRIu64, addr.offset);
    }
    if (addr.align != 0) {
      out.printf(" align=%d", addr.align);
    }
  }
  void dumpBlockDepth(uint32_t relativeDepth) {
    out.printf(" %d", relativeDepth);
  }
  void dumpBlockDepths(const Uint32Vector& relativeDepths) {
    for (uint32_t i = 0; i < relativeDepths.length(); i++) {
      out.printf(" %d", relativeDepths[i]);
    }
  }
  void dumpFieldIndex(uint32_t fieldIndex) { out.printf(" %d", fieldIndex); }
  void dumpNumElements(uint32_t numElements) { out.printf(" %d", numElements); }
  void dumpLaneIndex(uint32_t laneIndex) { out.printf(" %d", laneIndex); }

  void startScope() { out.pushScope(); }
  void endScope() { out.popScope(); }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_dump_h
