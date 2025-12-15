/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmDump.h"

#include <cinttypes>

#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::wasm;

static void DumpTypeDefIndex(const TypeDef* typeDef, GenericPrinter& out,
                             const TypeContext* types) {
  if (types) {
    out.printf("%" PRIu32, types->indexOf(*typeDef));
  } else {
    out.printf("? (;0x%" PRIxPTR ";)", (uintptr_t)typeDef);
  }
}

static void DumpFuncParamsAndResults(const FuncType& funcType,
                                     StructuredPrinter& out,
                                     const TypeContext* types) {
  if (funcType.args().length() > 0) {
    out.brk(" ", "\n");
    out.printf("(param");
    for (ValType arg : funcType.args()) {
      out.printf(" ");
      DumpValType(arg, out, types);
    }
    out.printf(")");
  }
  if (funcType.results().length() > 0) {
    out.brk(" ", "\n");
    out.printf("(result");
    for (ValType result : funcType.results()) {
      out.printf(" ");
      DumpValType(result, out, types);
    }
    out.printf(")");
  }
}

template <size_t MinInlineCapacity, class AllocPolicy>
static void DumpBytesAsString(
    const mozilla::Vector<uint8_t, MinInlineCapacity, AllocPolicy>& bytes,
    GenericPrinter& out) {
  WATStringEscape esc;
  EscapePrinter o(out, esc);

  out.put("\"");
  for (uint8_t b : bytes) {
    o.putChar(b);
  }
  out.put("\"");
}

static void DumpName(const CacheableName& name, GenericPrinter& out) {
  WATStringEscape esc;
  EscapePrinter o(out, esc);

  out.put("\"");
  o.put(name.utf8Bytes());
  out.put("\"");
}

void wasm::DumpModule(const Module& module) {
  Fprinter out(stdout);
  wasm::DumpModule(module, out);
  out.printf("\n");
}

void wasm::DumpModule(const Module& module, GenericPrinter& out) {
  StructuredPrinter o(out);

  o.printf("(module");
  {
    StructuredPrinter::Scope _(o);

    // Type section
    const TypeContext& types = *module.moduleMeta().codeMeta->types.get();
    if (types.length()) {
      o.printf("\n");
      DumpTypeContext(types, o);
      o.printf("\n");
    }

    // Import section
    size_t numFuncImports = 0;
    size_t numTableImports = 0;
    size_t numMemoryImports = 0;
    size_t numGlobalImports = 0;
    size_t numTagImports = 0;
    for (const Import& import : module.moduleMeta().imports) {
      o.printf("\n");
      o.printf("(import ");

      DumpName(import.module, o);
      o.put(" ");
      DumpName(import.field, o);
      o.put(" ");

      switch (import.kind) {
        case DefinitionKind::Function: {
          size_t funcIndex = numFuncImports++;
          const TypeDef& funcTypeDef =
              module.codeMeta().getFuncTypeDef(funcIndex);
          o.printf("(func (;%zu;) (type ", funcIndex);
          DumpTypeDefIndex(&funcTypeDef, o, &types);
          o.printf("))");
        } break;
        case DefinitionKind::Table: {
          size_t tableIndex = numTableImports++;
          const TableDesc& tableDesc = module.codeMeta().tables[tableIndex];
          DumpTableDesc(tableDesc, module.codeMeta(), /*includeInitExpr=*/false,
                        o, int32_t(tableIndex));
        } break;
        case DefinitionKind::Memory: {
          size_t memIndex = numMemoryImports++;
          const MemoryDesc& memDesc = module.codeMeta().memories[memIndex];
          DumpMemoryDesc(memDesc, o, int32_t(memIndex));
        } break;
        case DefinitionKind::Global: {
          size_t globalIndex = numGlobalImports++;
          const GlobalDesc& globalDesc = module.codeMeta().globals[globalIndex];
          DumpGlobalDesc(globalDesc, module.codeMeta(), false, o,
                         int32_t(globalIndex));
        } break;
        case DefinitionKind::Tag: {
          size_t tagIndex = numTagImports++;
          const TagDesc& tagDesc = module.codeMeta().tags[tagIndex];
          DumpTagDesc(tagDesc, o, int32_t(tagIndex), &types);
        } break;
        default: {
          o.printf("(; unknown import kind ;)");
        } break;
      }

      o.printf(")");
    }
    if (module.moduleMeta().imports.length() > 0) {
      o.printf("\n");
    }

    // We skip the function section because the function types show up later in
    // the dump of the code section.

    // Table section
    for (size_t i = numTableImports; i < module.codeMeta().tables.length();
         i++) {
      const TableDesc& tableDesc = module.codeMeta().tables[i];
      o.printf("\n");
      DumpTableDesc(tableDesc, module.codeMeta(), /*includeInitExpr=*/true, o,
                    int32_t(i));
    }
    if (module.codeMeta().tables.length() - numTableImports > 0) {
      o.printf("\n");
    }

    // Memory section
    for (size_t i = numMemoryImports; i < module.codeMeta().memories.length();
         i++) {
      const MemoryDesc& memDesc = module.codeMeta().memories[i];
      o.printf("\n");
      DumpMemoryDesc(memDesc, o, int32_t(i));
    }
    if (module.codeMeta().memories.length() - numMemoryImports > 0) {
      o.printf("\n");
    }

    // Tag section
    for (size_t i = numTagImports; i < module.codeMeta().tags.length(); i++) {
      const TagDesc& tagDesc = module.codeMeta().tags[i];
      o.printf("\n");
      DumpTagDesc(tagDesc, o, int32_t(i), &types);
    }
    if (module.codeMeta().tags.length() - numTagImports > 0) {
      o.printf("\n");
    }

    // Global section
    for (size_t i = numGlobalImports; i < module.codeMeta().globals.length();
         i++) {
      const GlobalDesc& globalDesc = module.codeMeta().globals[i];
      o.printf("\n");
      DumpGlobalDesc(globalDesc, module.codeMeta(), /*includeInitExpr=*/true, o,
                     int32_t(i));
    }
    if (module.codeMeta().globals.length() - numGlobalImports > 0) {
      o.printf("\n");
    }

    // Export section
    for (const Export& exp : module.moduleMeta().exports) {
      o.printf("\n");
      o.printf("(export ");
      DumpName(exp.fieldName(), o);
      o.printf(" ");
      switch (exp.kind()) {
        case DefinitionKind::Function: {
          o.printf("(func %" PRIu32 ")", exp.funcIndex());
        } break;
        case DefinitionKind::Table: {
          o.printf("(table %" PRIu32 ")", exp.tableIndex());
        } break;
        case DefinitionKind::Memory: {
          o.printf("(memory %" PRIu32 ")", exp.memoryIndex());
        } break;
        case DefinitionKind::Global: {
          o.printf("(global %" PRIu32 ")", exp.globalIndex());
        } break;
        case DefinitionKind::Tag: {
          o.printf("(tag %" PRIu32 ")", exp.tagIndex());
        } break;
        default: {
          o.printf("(; unknown export kind ;)");
        } break;
      }
      o.printf(")");
    }
    if (module.moduleMeta().exports.length() > 0) {
      o.printf("\n");
    }

    // Start section
    if (module.codeMeta().startFuncIndex.isSome()) {
      o.printf("\n");
      o.printf("(start %" PRIu32 ")", module.codeMeta().startFuncIndex.value());
      o.printf("\n");
    }

    // Element section
    for (size_t i = 0; i < module.moduleMeta().elemSegments.length(); i++) {
      const ModuleElemSegment& elem = module.moduleMeta().elemSegments[i];
      o.printf("\n");
      o.printf("(elem (;%zu;)", i);

      bool typeExpanded = false;
      {
        StructuredPrinter::Scope _(o);
        if (elem.active()) {
          o.brk(" ", "\n");
          o.printf("(table %" PRIu32 ")", elem.tableIndex);
          o.brk(" ", "\n");
          o.printf("(offset");
          {
            StructuredPrinter::Scope _(o);
            DumpInitExpr(elem.offset(), module.codeMeta(), o);
            o.brk("", "\n");
          }
          o.printf(")");
        } else if (elem.kind == ModuleElemSegment::Kind::Declared) {
          o.brk(" ", "\n");
          o.printf("declare");
        }
        if (elem.encoding == ModuleElemSegment::Encoding::Expressions) {
          o.brk(" ", "\n");
          DumpRefType(elem.elemType, o, &types);
        }
        o.brk("", "\n");
        typeExpanded = o.isExpanded();
      }
      {
        StructuredPrinter::Scope _(o);
        if (typeExpanded) {
          o.expand();
        }

        switch (elem.encoding) {
          case ModuleElemSegment::Encoding::Indices: {
            o.brk(" ", "\n");
            o.printf("func");
            for (uint32_t idx : elem.elemIndices) {
              o.printf(" %" PRIu32, idx);
            }
          } break;
          case ModuleElemSegment::Encoding::Expressions: {
            UniqueChars error;
            Decoder d(elem.elemExpressions.exprBytes.begin(),
                      elem.elemExpressions.exprBytes.end(), /* dummy */ 0,
                      &error);
            ValTypeVector locals;
            ValidatingOpIter iter(module.codeMeta(), d, locals,
                                  ValidatingOpIter::Kind::InitExpr);
            for (uint32_t i = 0; i < elem.numElements(); i++) {
              o.brk(" ", "\n");
              o.printf("(item");
              {
                StructuredPrinter::Scope _(o);

                OpDumper visitor(o, &types);
                if (!iter.startInitExpr(elem.elemType)) {
                  out.printf("(; bad expression ;)");
                  return;
                }
                if (!ValidateOps(iter, visitor, module.codeMeta())) {
                  out.printf("(; bad expression: %s ;)", d.error()->get());
                  return;
                }
                if (!iter.endInitExpr()) {
                  out.printf("(; bad expression ;)");
                  return;
                }
                o.brk("", "\n");
              }
              o.printf(")");
            }

            if (elem.numElements() > 1) {
              o.expand();
            }
          } break;
          default: {
            out.printf("(; unknown encoding ;)");
          } break;
        }
        o.brk("", "\n");
      }
      o.printf(")");
    }
    if (module.moduleMeta().elemSegments.length() > 0) {
      o.printf("\n");
    }

    // Code section
    for (size_t i = numFuncImports; i < module.codeMeta().numFuncs(); i++) {
      o.printf("\n");
      DumpFunction(module.codeMeta(), module.codeTailMeta(), i, o);
    }
    if (module.codeMeta().numFuncs() - numFuncImports > 0) {
      o.printf("\n");
    }

    // Data section
    for (size_t i = 0; i < module.moduleMeta().dataSegments.length(); i++) {
      RefPtr<const DataSegment> seg = module.moduleMeta().dataSegments[i];

      o.printf("\n");
      o.printf("(data (;%zu;)", i);
      {
        StructuredPrinter::Scope _(o);
        if (seg->active()) {
          o.brk(" ", "\n");
          o.printf("(memory %" PRIu32 ")", seg->memoryIndex);
          o.brk(" ", "\n");
          o.printf("(offset");
          {
            StructuredPrinter::Scope _(o);
            DumpInitExpr(seg->offset(), module.codeMeta(), o);
            o.brk("", "\n");
          }
          o.printf(")");
        }
        o.brk(" ", "\n");
        DumpBytesAsString(seg->bytes, o);
        o.brk("", "\n");
      }
      o.printf(")");
    }
  }
  o.brk("", "\n");
  o.printf(")");
}

void wasm::DumpValType(ValType type, const TypeContext* types) {
  Fprinter out(stdout);
  wasm::DumpValType(type, out, types);
  out.printf("\n");
}

void wasm::DumpValType(ValType type, GenericPrinter& out,
                       const TypeContext* types) {
  DumpStorageType(type.storageType(), out, types);
}

void wasm::DumpStorageType(StorageType type, const TypeContext* types) {
  Fprinter out(stdout);
  wasm::DumpStorageType(type, out, types);
  out.printf("\n");
}

void wasm::DumpStorageType(StorageType type, GenericPrinter& out,
                           const TypeContext* types) {
  const char* literal = nullptr;
  switch (type.kind()) {
    case StorageType::I8:
      literal = "i8";
      break;
    case StorageType::I16:
      literal = "i16";
      break;
    case StorageType::I32:
      literal = "i32";
      break;
    case StorageType::I64:
      literal = "i64";
      break;
    case StorageType::V128:
      literal = "v128";
      break;
    case StorageType::F32:
      literal = "f32";
      break;
    case StorageType::F64:
      literal = "f64";
      break;
    case StorageType::Ref:
      return DumpRefType(type.refType(), out, types);
    default:
      MOZ_CRASH("unexpected storage type");
  }
  out.put(literal);
}

void wasm::DumpRefType(RefType type, const TypeContext* types) {
  Fprinter out(stdout);
  wasm::DumpRefType(type, out, types);
  out.printf("\n");
}

void wasm::DumpRefType(RefType type, GenericPrinter& out,
                       const TypeContext* types) {
  if (type.isNullable() && !type.isTypeRef()) {
    const char* literal = nullptr;
    switch (type.kind()) {
      case RefType::Func:
        literal = "funcref";
        break;
      case RefType::Extern:
        literal = "externref";
        break;
      case RefType::Any:
        literal = "anyref";
        break;
      case RefType::NoFunc:
        literal = "nullfuncref";
        break;
      case RefType::NoExn:
        literal = "nullexn";
        break;
      case RefType::NoExtern:
        literal = "nullexternref";
        break;
      case RefType::None:
        literal = "nullref";
        break;
      case RefType::Eq:
        literal = "eqref";
        break;
      case RefType::I31:
        literal = "i31ref";
        break;
      case RefType::Struct:
        literal = "structref";
        break;
      case RefType::Array:
        literal = "arrayref";
        break;
      case RefType::Exn:
        literal = "exnref";
        break;
      case RefType::TypeRef: {
        MOZ_CRASH("type ref should not be possible here");
      }
    }
    out.put(literal);
    return;
  }

  // Emit the full reference type with heap type
  out.printf("(ref %s", type.isNullable() ? "null " : "");
  DumpHeapType(type, out, types);
  out.printf(")");
}

void wasm::DumpHeapType(RefType type, const TypeContext* types) {
  Fprinter out(stdout);
  wasm::DumpHeapType(type, out, types);
  out.printf("\n");
}

void wasm::DumpHeapType(RefType type, GenericPrinter& out,
                        const TypeContext* types) {
  switch (type.kind()) {
    case RefType::Func:
      out.put("func");
      return;
    case RefType::Extern:
      out.put("extern");
      return;
    case RefType::Any:
      out.put("any");
      return;
    case RefType::NoFunc:
      out.put("nofunc");
      return;
    case RefType::NoExn:
      out.put("noexn");
      return;
    case RefType::NoExtern:
      out.put("noextern");
      return;
    case RefType::None:
      out.put("none");
      return;
    case RefType::Eq:
      out.put("eq");
      return;
    case RefType::I31:
      out.put("i31");
      return;
    case RefType::Struct:
      out.put("struct");
      return;
    case RefType::Array:
      out.put("array");
      return;
    case RefType::Exn:
      out.put("exn");
      return;
    case RefType::TypeRef: {
      DumpTypeDefIndex(type.typeDef(), out, types);
      return;
    }
  }
}

void wasm::DumpFuncType(const FuncType& funcType, const TypeContext* types) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpFuncType(funcType, out, types);
  out.printf("\n");
}

void wasm::DumpFuncType(const FuncType& funcType, StructuredPrinter& out,
                        const TypeContext* types) {
  out.printf("(func");
  {
    StructuredPrinter::Scope _(out);
    DumpFuncParamsAndResults(funcType, out, types);
    out.brk("", "\n");

    if (funcType.args().length() + funcType.results().length() > 10) {
      out.expand();
    }
  }
  out.printf(")");
}

void wasm::DumpStructType(const StructType& structType,
                          const TypeContext* types) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpStructType(structType, out, types);
  out.printf("\n");
}

void wasm::DumpStructType(const StructType& structType, StructuredPrinter& out,
                          const TypeContext* types) {
  out.printf("(struct");
  {
    StructuredPrinter::Scope _(out);

    for (const FieldType& field : structType.fields_) {
      out.brk(" ", "\n");
      out.printf("(field ");
      if (field.isMutable) {
        out.printf("(mut ");
      }
      DumpStorageType(field.type, out, types);
      if (field.isMutable) {
        out.printf(")");
      }
      out.printf(")");
    }
    out.brk("", "\n");

    if (structType.fields_.length() > 1) {
      out.expand();
    }
  }
  out.printf(")");
}

void wasm::DumpArrayType(const ArrayType& arrayType, const TypeContext* types) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpArrayType(arrayType, out, types);
  out.printf("\n");
}

void wasm::DumpArrayType(const ArrayType& arrayType, StructuredPrinter& out,
                         const TypeContext* types) {
  out.printf("(array ");
  if (arrayType.isMutable()) {
    out.printf("(mut ");
  }
  DumpStorageType(arrayType.elementType(), out, types);
  if (arrayType.isMutable()) {
    out.printf(")");
  }
  out.printf(")");
}

void wasm::DumpTypeDef(const TypeDef& typeDef, int32_t index,
                       const TypeContext* types) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpTypeDef(typeDef, out, index, types);
  out.printf("\n");
}

void wasm::DumpTypeDef(const TypeDef& typeDef, StructuredPrinter& out,
                       int32_t index, const TypeContext* types) {
  out.printf("(type ");
  if (index >= 0) {
    out.printf("(;%" PRIi32 ";) ", index);
  }
  if (types && int32_t(types->indexOf(typeDef)) != index) {
    out.printf("(;canonicalized;) ");
  }

  // Somewhat counterintuitively, the text format works like so:
  //
  // (type (struct)):                       no parent, final
  // (type (sub (struct))):                 no parent, open
  // (type (sub $parent (struct))):         has parent, open
  // (type (sub $parent final (struct))):   has parent, final
  bool printSub = typeDef.superTypeDef() || !typeDef.isFinal();

  if (printSub) {
    out.printf("(sub ");
    if (typeDef.isFinal()) {
      out.printf("final ");
    }
    if (typeDef.superTypeDef()) {
      DumpTypeDefIndex(typeDef.superTypeDef(), out, types);
      out.printf(" ");
    }
  }

  switch (typeDef.kind()) {
    case TypeDefKind::Func:
      DumpFuncType(typeDef.funcType(), out, types);
      break;
    case TypeDefKind::Struct:
      DumpStructType(typeDef.structType(), out, types);
      break;
    case TypeDefKind::Array:
      DumpArrayType(typeDef.arrayType(), out, types);
      break;
    case TypeDefKind::None:
      out.printf("(; TypeDefKind::None ;)\n");
      break;
  }

  if (printSub) {
    out.printf(")");
  }

  out.printf(")");
}

void wasm::DumpRecGroup(const RecGroup& recGroup, int32_t startTypeIndex,
                        const TypeContext* types) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpRecGroup(recGroup, out, startTypeIndex, types);
  out.printf("\n");
}

void wasm::DumpRecGroup(const RecGroup& recGroup, StructuredPrinter& out,
                        int32_t startTypeIndex, const TypeContext* types) {
  if (recGroup.numTypes() > 1) {
    out.printf("(rec\n");
    {
      StructuredPrinter::Scope _(out);
      for (uint32_t i = 0; i < recGroup.numTypes(); i++) {
        if (i > 0) {
          out.printf("\n");
        }
        DumpTypeDef(recGroup.type(i), out,
                    startTypeIndex < 0 ? -1 : startTypeIndex + int32_t(i),
                    types);
      }
      out.printf("\n");
    }
    out.printf(")");
  } else {
    DumpTypeDef(recGroup.type(0), out, startTypeIndex < 0 ? -1 : startTypeIndex,
                types);
  }
}

void wasm::DumpTableDesc(const TableDesc& tableDesc,
                         const CodeMetadata& codeMeta, bool includeInitExpr,
                         int32_t index) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpTableDesc(tableDesc, codeMeta, includeInitExpr, out, index);
  out.printf("\n");
}

void wasm::DumpTableDesc(const TableDesc& tableDesc,
                         const CodeMetadata& codeMeta, bool includeInitExpr,
                         StructuredPrinter& out, int32_t index) {
  out.printf("(table ");
  if (index >= 0) {
    out.printf("(;%" PRIi32 ";) ", index);
  }
  if (tableDesc.addressType() == AddressType::I64) {
    out.printf("i64 ");
  }
  out.printf("%" PRIu64 " ", tableDesc.initialLength());
  if (tableDesc.maximumLength().isSome()) {
    out.printf("%" PRIu64 " ", tableDesc.maximumLength().value());
  }
  DumpRefType(tableDesc.elemType, out, codeMeta.types);
  if (includeInitExpr && tableDesc.initExpr) {
    StructuredPrinter::Scope _(out);
    DumpInitExpr(tableDesc.initExpr.ref(), codeMeta, out);
    out.brk("", "\n");
  }
  out.printf(")");
}

void wasm::DumpMemoryDesc(const MemoryDesc& memDesc, int32_t index) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpMemoryDesc(memDesc, out, index);
  out.printf("\n");
}

void wasm::DumpMemoryDesc(const MemoryDesc& memDesc, StructuredPrinter& out,
                          int32_t index) {
  out.printf("(memory ");
  if (index >= 0) {
    out.printf("(;%" PRIi32 ";) ", index);
  }
  if (memDesc.addressType() == AddressType::I64) {
    out.printf("i64 ");
  }
  out.printf("%" PRIu64, memDesc.initialPages().value());
  if (memDesc.maximumPages().isSome()) {
    out.printf(" %" PRIu64, memDesc.maximumPages().value().value());
  }
  out.printf(")");
}

void wasm::DumpGlobalDesc(const GlobalDesc& globalDesc,
                          const CodeMetadata& codeMeta, bool includeInitExpr,
                          int32_t index) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpGlobalDesc(globalDesc, codeMeta, includeInitExpr, out, index);
  out.printf("\n");
}

void wasm::DumpGlobalDesc(const GlobalDesc& globalDesc,
                          const CodeMetadata& codeMeta, bool includeInitExpr,
                          StructuredPrinter& out, int32_t index) {
  out.printf("(global ");
  if (index >= 0) {
    out.printf("(;%" PRIi32 ";) ", index);
  }
  if (globalDesc.isMutable()) {
    out.printf("(mut ");
  }
  DumpValType(globalDesc.type(), out, codeMeta.types);
  if (globalDesc.isMutable()) {
    out.printf(")");
  }
  if (includeInitExpr) {
    StructuredPrinter::Scope _(out);
    DumpInitExpr(globalDesc.initExpr(), codeMeta, out);
    out.brk("", "\n");
  }
  out.printf(")");
}

void wasm::DumpTagDesc(const TagDesc& tagDesc, int32_t index,
                       const TypeContext* types) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpTagDesc(tagDesc, out, index, types);
  out.printf("\n");
}

void wasm::DumpTagDesc(const TagDesc& tagDesc, StructuredPrinter& out,
                       int32_t index, const TypeContext* types) {
  out.printf("(tag ");
  if (index >= 0) {
    out.printf("(;%" PRIi32 ";) ", index);
  }
  out.printf("(type ");
  DumpTypeDefIndex(&tagDesc.type->type(), out, types);
  out.printf("))");
}

void wasm::DumpInitExpr(const InitExpr& initExpr,
                        const CodeMetadata& codeMeta) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpInitExpr(initExpr, codeMeta, out);
  out.printf("\n");
}

void wasm::DumpInitExpr(const InitExpr& initExpr, const CodeMetadata& codeMeta,
                        StructuredPrinter& out) {
  UniqueChars error;
  Decoder d(initExpr.bytecode().begin(), initExpr.bytecode().end(),
            /* dummy */ 0, &error);
  ValTypeVector locals;
  ValidatingOpIter iter(codeMeta, d, locals, ValidatingOpIter::Kind::InitExpr);
  OpDumper visitor(out, codeMeta.types);

  if (!iter.startInitExpr(initExpr.type())) {
    out.brk(" ", "\n");
    out.printf("(; bad expression ;)");
    return;
  }
  if (!ValidateOps(iter, visitor, codeMeta)) {
    out.brk(" ", "\n");
    out.printf("(; bad expression: %s ;)", d.error()->get());
    return;
  }
  if (!iter.endInitExpr()) {
    out.brk(" ", "\n");
    out.printf("(; bad expression ;)");
    return;
  }
}

void wasm::DumpTypeContext(const TypeContext& typeContext) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpTypeContext(typeContext, out);
  out.printf("\n");
}

void wasm::DumpTypeContext(const TypeContext& typeContext,
                           StructuredPrinter& out) {
  uint32_t numTypesSoFar = 0;
  for (size_t i = 0; i < typeContext.groups().length(); i++) {
    if (i > 0) {
      out.printf("\n");
    }
    const RecGroup& group = *typeContext.groups()[i];
    DumpRecGroup(group, out, int32_t(numTypesSoFar), &typeContext);
    numTypesSoFar += group.numTypes();
  }
}

void wasm::DumpFunction(const CodeMetadata& codeMeta,
                        const CodeTailMetadata& codeTailMeta,
                        uint32_t funcIndex) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpFunction(codeMeta, codeTailMeta, funcIndex, out);
  out.printf("\n");
}

void wasm::DumpFunction(const CodeMetadata& codeMeta,
                        const CodeTailMetadata& codeTailMeta,
                        uint32_t funcIndex, StructuredPrinter& out) {
  const FuncDesc& f = codeMeta.funcs[funcIndex];
  const TypeDef& typeDef = codeMeta.getFuncTypeDef(funcIndex);
  const FuncType& funcType = typeDef.funcType();

  out.printf("(func (;%" PRIu32 ";) (type %" PRIu32 ")", funcIndex,
             f.typeIndex);

  bool typeExpanded = false;
  {
    StructuredPrinter::Scope _(out);

    DumpFuncParamsAndResults(funcType, out, codeMeta.types);
    out.brk("", "\n");
    if (funcType.args().length() + funcType.results().length() > 8) {
      out.expand();
    }

    typeExpanded = out.isExpanded();
  }
  {
    StructuredPrinter::Scope _(out);
    if (typeExpanded) {
      out.expand();
    }

    if (codeTailMeta.codeSectionBytecode) {
      UniqueChars error;
      BytecodeSpan funcBytecode = codeTailMeta.funcDefBody(funcIndex);
      DumpFunctionBody(codeMeta, funcIndex, funcBytecode.data(),
                       funcBytecode.size(), out);
    } else {
      out.brk(" ", "\n");
      out.printf("(; no bytecode available ;)");
    }
    out.brk("", "\n");
  }
  out.printf(")");
}

void wasm::DumpFunctionBody(const CodeMetadata& codeMeta, uint32_t funcIndex,
                            const uint8_t* bodyStart, uint32_t bodySize) {
  Fprinter fileOut(stdout);
  StructuredPrinter out(fileOut);
  wasm::DumpFunctionBody(codeMeta, funcIndex, bodyStart, bodySize, out);
  out.printf("\n");
}

void wasm::DumpFunctionBody(const CodeMetadata& codeMeta, uint32_t funcIndex,
                            const uint8_t* bodyStart, uint32_t bodySize,
                            StructuredPrinter& out) {
  UniqueChars error;

  const uint8_t* bodyEnd = bodyStart + bodySize;
  Decoder d(bodyStart, bodyEnd, 0, &error);

  ValTypeVector locals;
  if (!DecodeLocalEntriesWithParams(d, codeMeta, funcIndex, &locals)) {
    out.brk(" ", "\n");
    out.printf("(; error: %s ;)", error.get());
    return;
  }
  uint32_t numArgs = codeMeta.getFuncType(funcIndex).args().length();
  if (locals.length() - numArgs > 0) {
    out.printf("\n(local");
    for (size_t i = numArgs; i < locals.length(); i++) {
      ValType local = locals[i];
      out.printf(" ");
      DumpValType(local, out, codeMeta.types);
    }
    out.printf(")\n");
  }

  ValidatingOpIter iter(codeMeta, d, locals);
  if (!iter.startFunction(funcIndex)) {
    out.brk(" ", "\n");
    out.printf("(; error: %s ;)", error.get());
    return;
  }

  OpDumper visitor(out, codeMeta.types);
  if (!ValidateOps(iter, visitor, codeMeta)) {
    out.brk(" ", "\n");
    out.printf("(; error: %s ;)", error.get());
    return;
  }

  if (!iter.endFunction(bodyEnd)) {
    out.brk(" ", "\n");
    out.printf("(; error: %s ;)", error.get());
    return;
  }
}
