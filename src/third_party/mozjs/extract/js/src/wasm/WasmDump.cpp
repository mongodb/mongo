/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "wasm/WasmDump.h"

#include <cinttypes>

using namespace js;
using namespace js::wasm;

#ifdef DEBUG

void wasm::Dump(ValType type) {
  Fprinter out(stdout);
  wasm::Dump(type, out);
}

void wasm::Dump(ValType type, GenericPrinter& out) {
  Dump(type.storageType(), out);
}

void wasm::Dump(StorageType type) {
  Fprinter out(stdout);
  wasm::Dump(type, out);
}

void wasm::Dump(StorageType type, GenericPrinter& out) {
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
      return Dump(type.refType(), out);
  }
  out.put(literal);
}

void wasm::Dump(RefType type) {
  Fprinter out(stdout);
  wasm::Dump(type, out);
}

void wasm::Dump(RefType type, GenericPrinter& out) {
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
  const char* heapType = nullptr;
  switch (type.kind()) {
    case RefType::Func:
      heapType = "func";
      break;
    case RefType::Extern:
      heapType = "extern";
      break;
    case RefType::Any:
      heapType = "any";
      break;
    case RefType::NoFunc:
      heapType = "nofunc";
      break;
    case RefType::NoExn:
      heapType = "noexn";
      break;
    case RefType::NoExtern:
      heapType = "noextern";
      break;
    case RefType::None:
      heapType = "none";
      break;
    case RefType::Eq:
      heapType = "eq";
      break;
    case RefType::I31:
      heapType = "eq";
      break;
    case RefType::Struct:
      heapType = "struct";
      break;
    case RefType::Array:
      heapType = "array";
      break;
    case RefType::Exn:
      heapType = "exn";
      break;
    case RefType::TypeRef: {
      uintptr_t typeAddress = (uintptr_t)type.typeDef();
      out.printf("(ref %s0x%" PRIxPTR ")", type.isNullable() ? "null " : "",
                 typeAddress);
      return;
    }
  }
  out.printf("(ref %s%s)", type.isNullable() ? "null " : "", heapType);
}

void wasm::Dump(const FuncType& funcType) {
  Fprinter fileOut(stdout);
  IndentedPrinter out(fileOut);
  wasm::Dump(funcType, out);
}

void wasm::Dump(const FuncType& funcType, IndentedPrinter& out) {
  out.printf("(func\n");
  {
    IndentedPrinter::AutoIndent innerIndent(out);
    for (ValType arg : funcType.args()) {
      out.printf("(param ");
      Dump(arg, out);
      out.printf(")\n");
    }
    for (ValType result : funcType.results()) {
      out.printf("(result ");
      Dump(result, out);
      out.printf(")\n");
    }
  }
  out.printf(")\n");
}

void wasm::Dump(const StructType& structType) {
  Fprinter fileOut(stdout);
  IndentedPrinter out(fileOut);
  wasm::Dump(structType, out);
}

void wasm::Dump(const StructType& structType, IndentedPrinter& out) {
  out.printf("(struct\n");
  {
    IndentedPrinter::AutoIndent innerIndent(out);
    for (const FieldType& field : structType.fields_) {
      out.printf("(field ");
      if (field.isMutable) {
        out.printf("(mut ");
      }
      Dump(field.type, out);
      if (field.isMutable) {
        out.printf(")");
      }
      out.printf(")\n");
    }
  }
  out.printf(")\n");
}

void wasm::Dump(const ArrayType& arrayType) {
  Fprinter fileOut(stdout);
  IndentedPrinter out(fileOut);
  wasm::Dump(arrayType, out);
}

void wasm::Dump(const ArrayType& arrayType, IndentedPrinter& out) {
  out.printf("(array ");
  if (arrayType.isMutable()) {
    out.printf("(mut ");
  }
  Dump(arrayType.elementType(), out);
  if (arrayType.isMutable()) {
    out.printf(")");
  }
  out.printf(")\n");
}

void wasm::Dump(const TypeDef& typeDef) {
  Fprinter fileOut(stdout);
  IndentedPrinter out(fileOut);
  wasm::Dump(typeDef, out);
}

void wasm::Dump(const TypeDef& typeDef, IndentedPrinter& out) {
  out.printf("(type 0x%" PRIxPTR "\n", (uintptr_t)&typeDef);

  {
    IndentedPrinter::AutoIndent innerIndent(out);
    out.printf("final=%u\n", typeDef.isFinal() ? 1 : 0);
    out.printf("subtypingDepth=%u\n", typeDef.subTypingDepth());
    if (typeDef.superTypeDef()) {
      out.printf("superType=0x%" PRIxPTR "\n",
                 (uintptr_t)typeDef.superTypeDef());
    }
    switch (typeDef.kind()) {
      case TypeDefKind::Func:
        Dump(typeDef.funcType(), out);
        break;
      case TypeDefKind::Struct:
        Dump(typeDef.structType(), out);
        break;
      case TypeDefKind::Array:
        Dump(typeDef.arrayType(), out);
        break;
      case TypeDefKind::None:
        out.printf("(none)\n");
        break;
    }
  }

  out.printf(")\n");
}

void wasm::Dump(const RecGroup& recGroup) {
  Fprinter fileOut(stdout);
  IndentedPrinter out(fileOut);
  wasm::Dump(recGroup, out);
}

void wasm::Dump(const RecGroup& recGroup, IndentedPrinter& out) {
  out.printf("(rec\n");
  {
    IndentedPrinter::AutoIndent innerIndent(out);
    for (uint32_t typeIndex = 0; typeIndex < recGroup.numTypes(); typeIndex++) {
      Dump(recGroup.type(typeIndex), out);
    }
  }
  out.printf(")\n");
}

void wasm::Dump(const TypeContext& typeContext) {
  Fprinter fileOut(stdout);
  IndentedPrinter out(fileOut);
  wasm::Dump(typeContext, out);
}

void wasm::Dump(const TypeContext& typeContext, IndentedPrinter& out) {
  out.printf("(types\n");
  {
    IndentedPrinter::AutoIndent innerIndent(out);
    for (const SharedRecGroup& recGroup : typeContext.groups()) {
      Dump(*recGroup, out);
    }
  }
  out.printf(")\n");
}

#endif  // DEBUG
