/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=0 ft=c:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ObjLiteral.h"

#include "mozilla/HashTable.h"  // mozilla::HashSet

#include "NamespaceImports.h"  // ValueVector

#include "builtin/Array.h"  // NewDenseCopiedArray
#include "frontend/CompilationStencil.h"  // frontend::{CompilationStencil, CompilationAtomCache}
#include "frontend/ParserAtom.h"                   // frontend::ParserAtomTable
#include "frontend/TaggedParserAtomIndexHasher.h"  // TaggedParserAtomIndexHasher
#include "gc/AllocKind.h"                          // gc::AllocKind
#include "gc/Rooting.h"                            // RootedPlainObject
#include "js/Id.h"                                 // INT_TO_JSID
#include "js/RootingAPI.h"                         // Rooted
#include "js/TypeDecls.h"                          // RootedId, RootedValue
#include "vm/JSAtom.h"                             // JSAtom
#include "vm/JSObject.h"                           // TenuredObject
#include "vm/JSONPrinter.h"                        // js::JSONPrinter
#include "vm/NativeObject.h"                       // NativeDefineDataProperty
#include "vm/PlainObject.h"                        // PlainObject
#include "vm/Printer.h"                            // js::Fprinter

#include "gc/ObjectKind-inl.h"    // gc::GetGCObjectKind
#include "vm/JSAtom-inl.h"        // AtomToId
#include "vm/JSObject-inl.h"      // NewBuiltinClassInstance
#include "vm/NativeObject-inl.h"  // AddDataPropertyNonDelegate

namespace js {

bool ObjLiteralWriter::checkForDuplicatedNames(JSContext* cx) {
  if (!mightContainDuplicatePropertyNames_) {
    return true;
  }

  // If possible duplicate property names are detected by bloom-filter,
  // check again with hash-set.

  mozilla::HashSet<frontend::TaggedParserAtomIndex,
                   frontend::TaggedParserAtomIndexHasher>
      propNameSet;

  if (!propNameSet.reserve(propertyCount_)) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  ObjLiteralReader reader(getCode());

  while (true) {
    ObjLiteralInsn insn;
    if (!reader.readInsn(&insn)) {
      break;
    }

    if (insn.getKey().isArrayIndex()) {
      continue;
    }

    auto propName = insn.getKey().getAtomIndex();

    auto p = propNameSet.lookupForAdd(propName);
    if (p) {
      flags_.setFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName);
      break;
    }

    // Already reserved above and doesn't fail.
    MOZ_ALWAYS_TRUE(propNameSet.add(p, propName));
  }

  return true;
}

static void InterpretObjLiteralValue(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const ObjLiteralInsn& insn, MutableHandleValue valOut) {
  switch (insn.getOp()) {
    case ObjLiteralOpcode::ConstValue:
      valOut.set(insn.getConstValue());
      return;
    case ObjLiteralOpcode::ConstAtom: {
      frontend::TaggedParserAtomIndex index = insn.getAtomIndex();
      JSAtom* jsatom = atomCache.getExistingAtomAt(cx, index);
      MOZ_ASSERT(jsatom);
      valOut.setString(jsatom);
      return;
    }
    case ObjLiteralOpcode::Null:
      valOut.setNull();
      return;
    case ObjLiteralOpcode::Undefined:
      valOut.setUndefined();
      return;
    case ObjLiteralOpcode::True:
      valOut.setBoolean(true);
      return;
    case ObjLiteralOpcode::False:
      valOut.setBoolean(false);
      return;
    default:
      MOZ_CRASH("Unexpected object-literal instruction opcode");
  }
}

enum class PropertySetKind {
  UniqueNames,
  Normal,
};

template <PropertySetKind kind>
bool InterpretObjLiteralObj(JSContext* cx, HandlePlainObject obj,
                            const frontend::CompilationAtomCache& atomCache,
                            const mozilla::Span<const uint8_t> literalInsns,
                            ObjLiteralFlags flags) {
  bool singleton = flags.hasFlag(ObjLiteralFlag::Singleton);

  ObjLiteralReader reader(literalInsns);

  RootedId propId(cx);
  RootedValue propVal(cx);
  while (true) {
    // Make sure `insn` doesn't live across GC.
    ObjLiteralInsn insn;
    if (!reader.readInsn(&insn)) {
      break;
    }
    MOZ_ASSERT(insn.isValid());
    MOZ_ASSERT_IF(kind == PropertySetKind::UniqueNames,
                  !insn.getKey().isArrayIndex());

    if (kind == PropertySetKind::Normal && insn.getKey().isArrayIndex()) {
      propId = INT_TO_JSID(insn.getKey().getArrayIndex());
    } else {
      JSAtom* jsatom =
          atomCache.getExistingAtomAt(cx, insn.getKey().getAtomIndex());
      MOZ_ASSERT(jsatom);
      propId = AtomToId(jsatom);
    }

    if (singleton) {
      InterpretObjLiteralValue(cx, atomCache, insn, &propVal);
    } else {
      propVal.setUndefined();
    }

    if (kind == PropertySetKind::UniqueNames) {
      if (!AddDataPropertyNonPrototype(cx, obj, propId, propVal)) {
        return false;
      }
    } else {
      if (!NativeDefineDataProperty(cx, obj, propId, propVal,
                                    JSPROP_ENUMERATE)) {
        return false;
      }
    }
  }
  return true;
}

static JSObject* InterpretObjLiteralObj(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, ObjLiteralFlags flags,
    uint32_t propertyCount) {
  // Use NewObjectGCKind for empty object literals to reserve some fixed slots
  // for new properties. This improves performance for common patterns such as
  // |Object.assign({}, ...)|.
  gc::AllocKind allocKind;
  if (propertyCount == 0) {
    allocKind = NewObjectGCKind();
  } else {
    allocKind = gc::GetGCObjectKind(propertyCount);
  }

  RootedPlainObject obj(
      cx, NewBuiltinClassInstance<PlainObject>(cx, allocKind, TenuredObject));
  if (!obj) {
    return nullptr;
  }

  if (!flags.hasFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName)) {
    if (!InterpretObjLiteralObj<PropertySetKind::UniqueNames>(
            cx, obj, atomCache, literalInsns, flags)) {
      return nullptr;
    }
  } else {
    if (!InterpretObjLiteralObj<PropertySetKind::Normal>(cx, obj, atomCache,
                                                         literalInsns, flags)) {
      return nullptr;
    }
  }
  return obj;
}

static JSObject* InterpretObjLiteralArray(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, ObjLiteralFlags flags,
    uint32_t propertyCount) {
  ObjLiteralReader reader(literalInsns);
  ObjLiteralInsn insn;

  Rooted<ValueVector> elements(cx, ValueVector(cx));
  if (!elements.reserve(propertyCount)) {
    return nullptr;
  }

  RootedValue propVal(cx);
  while (reader.readInsn(&insn)) {
    MOZ_ASSERT(insn.isValid());

    InterpretObjLiteralValue(cx, atomCache, insn, &propVal);
    elements.infallibleAppend(propVal);
  }

  return NewDenseCopiedArray(cx, elements.length(), elements.begin(),
                             /* proto = */ nullptr,
                             NewObjectKind::TenuredObject);
}

static JSObject* InterpretObjLiteral(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, ObjLiteralFlags flags,
    uint32_t propertyCount) {
  return flags.hasFlag(ObjLiteralFlag::Array)
             ? InterpretObjLiteralArray(cx, atomCache, literalInsns, flags,
                                        propertyCount)
             : InterpretObjLiteralObj(cx, atomCache, literalInsns, flags,
                                      propertyCount);
}

JSObject* ObjLiteralStencil::create(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache) const {
  return InterpretObjLiteral(cx, atomCache, code_, flags_, propertyCount_);
}

#ifdef DEBUG
bool ObjLiteralStencil::isContainedIn(const LifoAlloc& alloc) const {
  return alloc.contains(code_.data());
}
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)

static void DumpObjLiteralFlagsItems(js::JSONPrinter& json,
                                     ObjLiteralFlags flags) {
  if (flags.hasFlag(ObjLiteralFlag::Array)) {
    json.value("Array");
    flags.clearFlag(ObjLiteralFlag::Array);
  }
  if (flags.hasFlag(ObjLiteralFlag::Singleton)) {
    json.value("Singleton");
    flags.clearFlag(ObjLiteralFlag::Singleton);
  }
  if (flags.hasFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName)) {
    json.value("HasIndexOrDuplicatePropName");
    flags.clearFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName);
  }

  if (!flags.isEmpty()) {
    json.value("Unknown(%x)", flags.toRaw());
  }
}

static void DumpObjLiteral(js::JSONPrinter& json,
                           const frontend::CompilationStencil* stencil,
                           mozilla::Span<const uint8_t> code,
                           const ObjLiteralFlags& flags,
                           uint32_t propertyCount) {
  json.beginListProperty("flags");
  DumpObjLiteralFlagsItems(json, flags);
  json.endList();

  json.beginListProperty("code");
  ObjLiteralReader reader(code);
  ObjLiteralInsn insn;
  while (reader.readInsn(&insn)) {
    json.beginObject();

    if (insn.getKey().isNone()) {
      json.nullProperty("key");
    } else if (insn.getKey().isAtomIndex()) {
      frontend::TaggedParserAtomIndex index = insn.getKey().getAtomIndex();
      json.beginObjectProperty("key");
      DumpTaggedParserAtomIndex(json, index, stencil);
      json.endObject();
    } else if (insn.getKey().isArrayIndex()) {
      uint32_t index = insn.getKey().getArrayIndex();
      json.formatProperty("key", "ArrayIndex(%u)", index);
    }

    switch (insn.getOp()) {
      case ObjLiteralOpcode::ConstValue: {
        const Value& v = insn.getConstValue();
        json.formatProperty("op", "ConstValue(%f)", v.toNumber());
        break;
      }
      case ObjLiteralOpcode::ConstAtom: {
        frontend::TaggedParserAtomIndex index = insn.getAtomIndex();
        json.beginObjectProperty("op");
        DumpTaggedParserAtomIndex(json, index, stencil);
        json.endObject();
        break;
      }
      case ObjLiteralOpcode::Null:
        json.property("op", "Null");
        break;
      case ObjLiteralOpcode::Undefined:
        json.property("op", "Undefined");
        break;
      case ObjLiteralOpcode::True:
        json.property("op", "True");
        break;
      case ObjLiteralOpcode::False:
        json.property("op", "False");
        break;
      default:
        json.formatProperty("op", "Invalid(%x)", uint8_t(insn.getOp()));
        break;
    }

    json.endObject();
  }
  json.endList();

  json.property("propertyCount", propertyCount);
}

void ObjLiteralWriter::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json, nullptr);
}

void ObjLiteralWriter::dump(js::JSONPrinter& json,
                            const frontend::CompilationStencil* stencil) const {
  json.beginObject();
  dumpFields(json, stencil);
  json.endObject();
}

void ObjLiteralWriter::dumpFields(
    js::JSONPrinter& json, const frontend::CompilationStencil* stencil) const {
  DumpObjLiteral(json, stencil, getCode(), flags_, propertyCount_);
}

void ObjLiteralStencil::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json, nullptr);
}

void ObjLiteralStencil::dump(
    js::JSONPrinter& json, const frontend::CompilationStencil* stencil) const {
  json.beginObject();
  dumpFields(json, stencil);
  json.endObject();
}

void ObjLiteralStencil::dumpFields(
    js::JSONPrinter& json, const frontend::CompilationStencil* stencil) const {
  DumpObjLiteral(json, stencil, code_, flags_, propertyCount_);
}

#endif  // defined(DEBUG) || defined(JS_JITSPEW)

}  // namespace js
