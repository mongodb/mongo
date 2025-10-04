/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=0 ft=c:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ObjLiteral.h"

#include "mozilla/DebugOnly.h"  // mozilla::DebugOnly
#include "mozilla/HashTable.h"  // mozilla::HashSet

#include "NamespaceImports.h"  // ValueVector

#include "builtin/Array.h"  // NewDenseCopiedArray
#include "frontend/CompilationStencil.h"  // frontend::{CompilationStencil, CompilationAtomCache}
#include "frontend/ParserAtom.h"                   // frontend::ParserAtomTable
#include "frontend/TaggedParserAtomIndexHasher.h"  // TaggedParserAtomIndexHasher
#include "gc/AllocKind.h"                          // gc::AllocKind
#include "js/Id.h"                                 // JS::PropertyKey
#include "js/Printer.h"                            // js::Fprinter
#include "js/RootingAPI.h"                         // Rooted
#include "js/TypeDecls.h"                          // RootedId, RootedValue
#include "vm/JSObject.h"                           // TenuredObject
#include "vm/JSONPrinter.h"                        // js::JSONPrinter
#include "vm/NativeObject.h"                       // NativeDefineDataProperty
#include "vm/PlainObject.h"                        // PlainObject

#include "gc/ObjectKind-inl.h"    // gc::GetGCObjectKind
#include "vm/JSAtomUtils-inl.h"   // AtomToId
#include "vm/JSObject-inl.h"      // NewBuiltinClassInstance
#include "vm/NativeObject-inl.h"  // AddDataPropertyNonDelegate

namespace js {

bool ObjLiteralWriter::checkForDuplicatedNames(FrontendContext* fc) {
  if (!mightContainDuplicatePropertyNames_) {
    return true;
  }

  // If possible duplicate property names are detected by bloom-filter,
  // check again with hash-set.

  mozilla::HashSet<frontend::TaggedParserAtomIndex,
                   frontend::TaggedParserAtomIndexHasher>
      propNameSet;

  if (!propNameSet.reserve(propertyCount_)) {
    js::ReportOutOfMemory(fc);
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
    case ObjLiteralOpcode::ConstString: {
      frontend::TaggedParserAtomIndex index = insn.getAtomIndex();
      JSString* str = atomCache.getExistingStringAt(cx, index);
      MOZ_ASSERT(str);
      valOut.setString(str);
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
bool InterpretObjLiteralObj(JSContext* cx, Handle<PlainObject*> obj,
                            const frontend::CompilationAtomCache& atomCache,
                            const mozilla::Span<const uint8_t> literalInsns) {
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
      propId = PropertyKey::Int(insn.getKey().getArrayIndex());
    } else {
      JSAtom* jsatom =
          atomCache.getExistingAtomAt(cx, insn.getKey().getAtomIndex());
      MOZ_ASSERT(jsatom);
      propId = AtomToId(jsatom);
    }

    InterpretObjLiteralValue(cx, atomCache, insn, &propVal);

    if constexpr (kind == PropertySetKind::UniqueNames) {
      if (!AddDataPropertyToPlainObject(cx, obj, propId, propVal)) {
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

static gc::AllocKind AllocKindForObjectLiteral(uint32_t propCount) {
  // Use NewObjectGCKind for empty object literals to reserve some fixed slots
  // for new properties. This improves performance for common patterns such as
  // |Object.assign({}, ...)|.
  return (propCount == 0) ? NewObjectGCKind() : gc::GetGCObjectKind(propCount);
}

static JSObject* InterpretObjLiteralObj(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, ObjLiteralFlags flags,
    uint32_t propertyCount) {
  gc::AllocKind allocKind = AllocKindForObjectLiteral(propertyCount);

  Rooted<PlainObject*> obj(
      cx, NewPlainObjectWithAllocKind(cx, allocKind, TenuredObject));
  if (!obj) {
    return nullptr;
  }

  if (!flags.hasFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName)) {
    if (!InterpretObjLiteralObj<PropertySetKind::UniqueNames>(
            cx, obj, atomCache, literalInsns)) {
      return nullptr;
    }
  } else {
    if (!InterpretObjLiteralObj<PropertySetKind::Normal>(cx, obj, atomCache,
                                                         literalInsns)) {
      return nullptr;
    }
  }
  return obj;
}

static JSObject* InterpretObjLiteralArray(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, uint32_t propertyCount) {
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
                             NewObjectKind::TenuredObject);
}

// ES2023 draft rev ee74c9cb74dbfa23e62b486f5226102c345c678e
//
// GetTemplateObject ( templateLiteral )
// https://tc39.es/ecma262/#sec-gettemplateobject
//
// Steps 8-16.
static JSObject* InterpretObjLiteralCallSiteObj(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, uint32_t propertyCount) {
  ObjLiteralReader reader(literalInsns);
  ObjLiteralInsn insn;

  // We have to read elements for two arrays. The 'cooked' values are followed
  // by the 'raw' values. Both arrays have the same length.
  MOZ_ASSERT((propertyCount % 2) == 0);
  uint32_t count = propertyCount / 2;

  Rooted<ValueVector> elements(cx, ValueVector(cx));
  if (!elements.reserve(count)) {
    return nullptr;
  }

  RootedValue propVal(cx);
  auto readElements = [&](uint32_t count) {
    MOZ_ASSERT(elements.empty());

    for (size_t i = 0; i < count; i++) {
      MOZ_ALWAYS_TRUE(reader.readInsn(&insn));
      MOZ_ASSERT(insn.isValid());

      InterpretObjLiteralValue(cx, atomCache, insn, &propVal);
      MOZ_ASSERT(propVal.isString() || propVal.isUndefined());
      elements.infallibleAppend(propVal);
    }
  };

  // Create cooked array.
  readElements(count);
  Rooted<ArrayObject*> cso(
      cx, NewDenseCopiedArray(cx, elements.length(), elements.begin(),
                              NewObjectKind::TenuredObject));
  if (!cso) {
    return nullptr;
  }
  elements.clear();

  // Create raw array.
  readElements(count);
  Rooted<ArrayObject*> raw(
      cx, NewDenseCopiedArray(cx, elements.length(), elements.begin(),
                              NewObjectKind::TenuredObject));
  if (!raw) {
    return nullptr;
  }

  // Define .raw property and freeze both arrays.
  RootedValue rawValue(cx, ObjectValue(*raw));
  if (!DefineDataProperty(cx, cso, cx->names().raw, rawValue, 0)) {
    return nullptr;
  }
  if (!FreezeObject(cx, raw)) {
    return nullptr;
  }
  if (!FreezeObject(cx, cso)) {
    return nullptr;
  }

  return cso;
}

template <PropertySetKind kind>
Shape* InterpretObjLiteralShape(JSContext* cx,
                                const frontend::CompilationAtomCache& atomCache,
                                const mozilla::Span<const uint8_t> literalInsns,
                                uint32_t numFixedSlots) {
  ObjLiteralReader reader(literalInsns);

  Rooted<SharedPropMap*> map(cx);
  uint32_t mapLength = 0;
  ObjectFlags objectFlags;

  uint32_t slot = 0;
  RootedId propId(cx);
  while (true) {
    // Make sure `insn` doesn't live across GC.
    ObjLiteralInsn insn;
    if (!reader.readInsn(&insn)) {
      break;
    }
    MOZ_ASSERT(insn.isValid());
    MOZ_ASSERT(!insn.getKey().isArrayIndex());
    MOZ_ASSERT(insn.getOp() == ObjLiteralOpcode::Undefined);

    JSAtom* jsatom =
        atomCache.getExistingAtomAt(cx, insn.getKey().getAtomIndex());
    MOZ_ASSERT(jsatom);
    propId = AtomToId(jsatom);

    // Assert or check property names are unique.
    if constexpr (kind == PropertySetKind::UniqueNames) {
      mozilla::DebugOnly<uint32_t> index;
      MOZ_ASSERT_IF(map, !map->lookupPure(mapLength, propId, &index));
    } else {
      uint32_t index;
      if (map && map->lookupPure(mapLength, propId, &index)) {
        continue;
      }
    }

    constexpr PropertyFlags propFlags = PropertyFlags::defaultDataPropFlags;

    if (!SharedPropMap::addPropertyWithKnownSlot(cx, &PlainObject::class_, &map,
                                                 &mapLength, propId, propFlags,
                                                 slot, &objectFlags)) {
      return nullptr;
    }

    slot++;
  }

  JSObject* proto = &cx->global()->getObjectPrototype();
  return SharedShape::getInitialOrPropMapShape(
      cx, &PlainObject::class_, cx->realm(), TaggedProto(proto), numFixedSlots,
      map, mapLength, objectFlags);
}

static Shape* InterpretObjLiteralShape(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache,
    const mozilla::Span<const uint8_t> literalInsns, ObjLiteralFlags flags,
    uint32_t propertyCount) {
  gc::AllocKind allocKind = AllocKindForObjectLiteral(propertyCount);
  uint32_t numFixedSlots = GetGCKindSlots(allocKind);

  if (!flags.hasFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName)) {
    return InterpretObjLiteralShape<PropertySetKind::UniqueNames>(
        cx, atomCache, literalInsns, numFixedSlots);
  }
  return InterpretObjLiteralShape<PropertySetKind::Normal>(
      cx, atomCache, literalInsns, numFixedSlots);
}

JS::GCCellPtr ObjLiteralStencil::create(
    JSContext* cx, const frontend::CompilationAtomCache& atomCache) const {
  switch (kind()) {
    case ObjLiteralKind::Array: {
      JSObject* obj =
          InterpretObjLiteralArray(cx, atomCache, code_, propertyCount_);
      if (!obj) {
        return JS::GCCellPtr();
      }
      return JS::GCCellPtr(obj);
    }
    case ObjLiteralKind::CallSiteObj: {
      JSObject* obj =
          InterpretObjLiteralCallSiteObj(cx, atomCache, code_, propertyCount_);
      if (!obj) {
        return JS::GCCellPtr();
      }
      return JS::GCCellPtr(obj);
    }
    case ObjLiteralKind::Object: {
      JSObject* obj =
          InterpretObjLiteralObj(cx, atomCache, code_, flags(), propertyCount_);
      if (!obj) {
        return JS::GCCellPtr();
      }
      return JS::GCCellPtr(obj);
    }
    case ObjLiteralKind::Shape: {
      Shape* shape = InterpretObjLiteralShape(cx, atomCache, code_, flags(),
                                              propertyCount_);
      if (!shape) {
        return JS::GCCellPtr();
      }
      return JS::GCCellPtr(shape);
    }
    case ObjLiteralKind::Invalid:
      break;
  }
  MOZ_CRASH("Invalid kind");
}

#ifdef DEBUG
bool ObjLiteralStencil::isContainedIn(const LifoAlloc& alloc) const {
  return alloc.contains(code_.data());
}
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)

static void DumpObjLiteralFlagsItems(js::JSONPrinter& json,
                                     ObjLiteralFlags flags) {
  if (flags.hasFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName)) {
    json.value("HasIndexOrDuplicatePropName");
    flags.clearFlag(ObjLiteralFlag::HasIndexOrDuplicatePropName);
  }

  if (!flags.isEmpty()) {
    json.value("Unknown(%x)", flags.toRaw());
  }
}

static const char* ObjLiteralKindToString(ObjLiteralKind kind) {
  switch (kind) {
    case ObjLiteralKind::Object:
      return "Object";
    case ObjLiteralKind::Array:
      return "Array";
    case ObjLiteralKind::CallSiteObj:
      return "CallSiteObj";
    case ObjLiteralKind::Shape:
      return "Shape";
    case ObjLiteralKind::Invalid:
      break;
  }
  MOZ_CRASH("Invalid kind");
}

static void DumpObjLiteral(js::JSONPrinter& json,
                           const frontend::CompilationStencil* stencil,
                           mozilla::Span<const uint8_t> code,
                           ObjLiteralKind kind, const ObjLiteralFlags& flags,
                           uint32_t propertyCount) {
  json.property("kind", ObjLiteralKindToString(kind));

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
      case ObjLiteralOpcode::ConstString: {
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
  DumpObjLiteral(json, stencil, getCode(), kind_, flags_, propertyCount_);
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
  DumpObjLiteral(json, stencil, code_, kind(), flags(), propertyCount_);
}

#endif  // defined(DEBUG) || defined(JS_JITSPEW)

}  // namespace js
