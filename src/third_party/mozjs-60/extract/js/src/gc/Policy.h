/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS Garbage Collector. */

#ifndef gc_Policy_h
#define gc_Policy_h

#include "mozilla/TypeTraits.h"
#include "gc/Barrier.h"
#include "gc/Marking.h"
#include "js/GCPolicyAPI.h"

// Forward declare the types we're defining policies for. This file is
// included in all places that define GC things, so the real definitions
// will be available when we do template expansion, allowing for use of
// static members in the underlying types. We cannot, however, use
// static_assert to verify relations between types.
class JSLinearString;
namespace js {
class AccessorShape;
class ArgumentsObject;
class ArrayBufferObject;
class ArrayBufferObjectMaybeShared;
class ArrayBufferViewObject;
class ArrayObject;
class BaseShape;
class DebugEnvironmentProxy;
class DebuggerFrame;
class ExportEntryObject;
class EnvironmentObject;
class GlobalObject;
class ImportEntryObject;
class LazyScript;
class LexicalEnvironmentObject;
class ModuleEnvironmentObject;
class ModuleNamespaceObject;
class ModuleObject;
class NativeObject;
class ObjectGroup;
class PlainObject;
class PropertyName;
class RegExpObject;
class SavedFrame;
class Scope;
class EnvironmentObject;
class RequestedModuleObject;
class ScriptSourceObject;
class Shape;
class SharedArrayBufferObject;
class StructTypeDescr;
class UnownedBaseShape;
class WasmFunctionScope;
class WasmMemoryObject;
namespace jit {
class JitCode;
} // namespace jit
} // namespace js

// Expand the given macro D for each valid GC reference type.
#define FOR_EACH_INTERNAL_GC_POINTER_TYPE(D) \
    D(JSFlatString*) \
    D(JSLinearString*) \
    D(js::AccessorShape*) \
    D(js::ArgumentsObject*) \
    D(js::ArrayBufferObject*) \
    D(js::ArrayBufferObjectMaybeShared*) \
    D(js::ArrayBufferViewObject*) \
    D(js::ArrayObject*) \
    D(js::BaseShape*) \
    D(js::DebugEnvironmentProxy*) \
    D(js::DebuggerFrame*) \
    D(js::ExportEntryObject*) \
    D(js::EnvironmentObject*) \
    D(js::GlobalObject*) \
    D(js::ImportEntryObject*) \
    D(js::LazyScript*) \
    D(js::LexicalEnvironmentObject*) \
    D(js::ModuleEnvironmentObject*) \
    D(js::ModuleNamespaceObject*) \
    D(js::ModuleObject*) \
    D(js::NativeObject*) \
    D(js::ObjectGroup*) \
    D(js::PlainObject*) \
    D(js::PropertyName*) \
    D(js::RegExpObject*) \
    D(js::RegExpShared*) \
    D(js::RequestedModuleObject*) \
    D(js::SavedFrame*) \
    D(js::Scope*) \
    D(js::ScriptSourceObject*) \
    D(js::Shape*) \
    D(js::SharedArrayBufferObject*) \
    D(js::StructTypeDescr*) \
    D(js::UnownedBaseShape*) \
    D(js::WasmFunctionScope*) \
    D(js::WasmInstanceObject*) \
    D(js::WasmMemoryObject*) \
    D(js::WasmTableObject*) \
    D(js::jit::JitCode*)

// Expand the given macro D for each internal tagged GC pointer type.
#define FOR_EACH_INTERNAL_TAGGED_GC_POINTER_TYPE(D) \
    D(js::TaggedProto)

// Expand the macro D for every GC reference type that we know about.
#define FOR_EACH_GC_POINTER_TYPE(D) \
    FOR_EACH_PUBLIC_GC_POINTER_TYPE(D) \
    FOR_EACH_PUBLIC_TAGGED_GC_POINTER_TYPE(D) \
    FOR_EACH_INTERNAL_GC_POINTER_TYPE(D) \
    FOR_EACH_INTERNAL_TAGGED_GC_POINTER_TYPE(D)

namespace js {

// Define the GCPolicy for all internal pointers.
template <typename T>
struct InternalGCPointerPolicy {
    using Type = typename mozilla::RemovePointer<T>::Type;
    static T initial() { return nullptr; }
    static void preBarrier(T v) { Type::writeBarrierPre(v); }
    static void postBarrier(T* vp, T prev, T next) { Type::writeBarrierPost(vp, prev, next); }
    static void readBarrier(T v) { Type::readBarrier(v); }
    static void trace(JSTracer* trc, T* vp, const char* name) {
        TraceManuallyBarrieredEdge(trc, vp, name);
    }
    static bool isValid(T v) {
        return gc::IsCellPointerValidOrNull(v);
    }
};

} // namespace js

namespace JS {

#define DEFINE_INTERNAL_GC_POLICY(type) \
    template <> struct GCPolicy<type> : public js::InternalGCPointerPolicy<type> {};
FOR_EACH_INTERNAL_GC_POINTER_TYPE(DEFINE_INTERNAL_GC_POLICY)
#undef DEFINE_INTERNAL_GC_POLICY

template <typename T>
struct GCPolicy<js::HeapPtr<T>>
{
    static void trace(JSTracer* trc, js::HeapPtr<T>* thingp, const char* name) {
        js::TraceEdge(trc, thingp, name);
    }
    static bool needsSweep(js::HeapPtr<T>* thingp) {
        return js::gc::IsAboutToBeFinalized(thingp);
    }
};

template <typename T>
struct GCPolicy<js::ReadBarriered<T>>
{
    static void trace(JSTracer* trc, js::ReadBarriered<T>* thingp, const char* name) {
        js::TraceEdge(trc, thingp, name);
    }
    static bool needsSweep(js::ReadBarriered<T>* thingp) {
        return js::gc::IsAboutToBeFinalized(thingp);
    }
};

} // namespace JS

#endif // gc_Policy_h
