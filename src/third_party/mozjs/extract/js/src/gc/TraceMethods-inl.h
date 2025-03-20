/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Trace methods for all GC things, defined in a separate header to allow
 * inlining.
 *
 * This also includes eager inline marking versions. Both paths must end up
 * traversing equivalent subgraphs.
 */

#ifndef gc_TraceMethods_inl_h
#define gc_TraceMethods_inl_h

#include "gc/GCMarker.h"
#include "gc/Tracer.h"
#include "jit/JitCode.h"
#include "vm/BigIntType.h"
#include "vm/GetterSetter.h"
#include "vm/GlobalObject.h"
#include "vm/JSScript.h"
#include "vm/PropMap.h"
#include "vm/Realm.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"
#include "wasm/WasmJS.h"

#include "gc/Marking-inl.h"
#include "vm/StringType-inl.h"

inline void js::BaseScript::traceChildren(JSTracer* trc) {
  TraceNullableEdge(trc, &function_, "function");
  TraceEdge(trc, &sourceObject_, "sourceObject");

  warmUpData_.trace(trc);

  if (data_) {
    data_->trace(trc);
  }
}

inline void js::Shape::traceChildren(JSTracer* trc) {
  TraceCellHeaderEdge(trc, this, "base");
  if (isNative()) {
    asNative().traceChildren(trc);
  }
}

inline void js::NativeShape::traceChildren(JSTracer* trc) {
  TraceNullableEdge(trc, &propMap_, "propertymap");
}

template <uint32_t opts>
void js::GCMarker::eagerlyMarkChildren(Shape* shape) {
  MOZ_ASSERT(shape->isMarked(markColor()));

  BaseShape* base = shape->base();
  checkTraversedEdge(shape, base);
  if (mark<opts>(base)) {
    base->traceChildren(tracer());
  }

  if (shape->isNative()) {
    if (PropMap* map = shape->asNative().propMap()) {
      markAndTraverseEdge<opts>(shape, map);
    }
  }
}

inline void JSString::traceChildren(JSTracer* trc) {
  if (hasBase()) {
    traceBase(trc);
  } else if (isRope()) {
    asRope().traceChildren(trc);
  }
}
inline void JSString::traceBaseFromStoreBuffer(JSTracer* trc) {
  MOZ_ASSERT(!d.s.u3.base->isTenured());

  // Contract the base chain so that this tenured dependent string points
  // directly at the root base that owns its chars.
  JSLinearString* root = asDependent().rootBaseDuringMinorGC();
  d.s.u3.base = root;
  if (!root->isTenured()) {
    js::TraceManuallyBarrieredEdge(trc, &root, "base");
    // Do not update the actual base to the tenured string yet. This string will
    // need to be swept in order to update its chars ptr to be relative to the
    // root, and that update requires information from the overlay.
  }
}
template <uint32_t opts>
void js::GCMarker::eagerlyMarkChildren(JSString* str) {
  if (str->isLinear()) {
    eagerlyMarkChildren<opts>(&str->asLinear());
  } else {
    eagerlyMarkChildren<opts>(&str->asRope());
  }
}

inline void JSString::traceBase(JSTracer* trc) {
  MOZ_ASSERT(hasBase());
  js::TraceManuallyBarrieredEdge(trc, &d.s.u3.base, "base");
}
template <uint32_t opts>
void js::GCMarker::eagerlyMarkChildren(JSLinearString* linearStr) {
  gc::AssertShouldMarkInZone(this, linearStr);
  MOZ_ASSERT(linearStr->isMarkedAny());
  MOZ_ASSERT(linearStr->JSString::isLinear());

  // Use iterative marking to avoid blowing out the stack.
  while (linearStr->hasBase()) {
    linearStr = linearStr->base();

    // It's possible to observe a rope as the base of a linear string if we
    // process barriers during rope flattening. See the assignment of base in
    // JSRope::flattenInternal's finish_node section.
    if (static_cast<JSString*>(linearStr)->isRope()) {
      MOZ_ASSERT(!JS::RuntimeHeapIsMajorCollecting());
      break;
    }

    MOZ_ASSERT(linearStr->JSString::isLinear());
    gc::AssertShouldMarkInZone(this, linearStr);
    if (!mark<opts>(static_cast<JSString*>(linearStr))) {
      break;
    }
  }
}

inline void JSRope::traceChildren(JSTracer* trc) {
  js::TraceManuallyBarrieredEdge(trc, &d.s.u2.left, "left child");
  js::TraceManuallyBarrieredEdge(trc, &d.s.u3.right, "right child");
}
template <uint32_t opts>
void js::GCMarker::eagerlyMarkChildren(JSRope* rope) {
  // This function tries to scan the whole rope tree using the marking stack
  // as temporary storage. If that becomes full, the unscanned ropes are
  // added to the delayed marking list. When the function returns, the
  // marking stack is at the same depth as it was on entry. This way we avoid
  // using tags when pushing ropes to the stack as ropes never leak to other
  // users of the stack. This also assumes that a rope can only point to
  // other ropes or linear strings, it cannot refer to GC things of other
  // types.
  size_t savedPos = stack.position();
  MOZ_DIAGNOSTIC_ASSERT(rope->getTraceKind() == JS::TraceKind::String);
  while (true) {
    MOZ_DIAGNOSTIC_ASSERT(rope->getTraceKind() == JS::TraceKind::String);
    MOZ_DIAGNOSTIC_ASSERT(rope->JSString::isRope());
    gc::AssertShouldMarkInZone(this, rope);
    MOZ_ASSERT(rope->isMarkedAny());
    JSRope* next = nullptr;

    JSString* right = rope->rightChild();
    if (mark<opts>(right)) {
      MOZ_ASSERT(!right->isPermanentAtom());
      if (right->isLinear()) {
        eagerlyMarkChildren<opts>(&right->asLinear());
      } else {
        next = &right->asRope();
      }
    }

    JSString* left = rope->leftChild();
    if (mark<opts>(left)) {
      MOZ_ASSERT(!left->isPermanentAtom());
      if (left->isLinear()) {
        eagerlyMarkChildren<opts>(&left->asLinear());
      } else {
        // When both children are ropes, set aside the right one to
        // scan it later.
        if (next && !stack.pushTempRope(next)) {
          delayMarkingChildrenOnOOM(next);
        }
        next = &left->asRope();
      }
    }
    if (next) {
      rope = next;
    } else if (savedPos != stack.position()) {
      MOZ_ASSERT(savedPos < stack.position());
      rope = stack.popPtr().asTempRope();
    } else {
      break;
    }
  }
  MOZ_ASSERT(savedPos == stack.position());
}

inline void JS::Symbol::traceChildren(JSTracer* trc) {
  js::TraceNullableCellHeaderEdge(trc, this, "symbol description");
}

template <typename SlotInfo>
void js::RuntimeScopeData<SlotInfo>::trace(JSTracer* trc) {
  TraceBindingNames(trc, GetScopeDataTrailingNamesPointer(this), length);
}

inline void js::FunctionScope::RuntimeData::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &canonicalFunction, "scope canonical function");
  TraceNullableBindingNames(trc, GetScopeDataTrailingNamesPointer(this),
                            length);
}
inline void js::ModuleScope::RuntimeData::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &module, "scope module");
  TraceBindingNames(trc, GetScopeDataTrailingNamesPointer(this), length);
}
inline void js::WasmInstanceScope::RuntimeData::trace(JSTracer* trc) {
  TraceNullableEdge(trc, &instance, "wasm instance");
  TraceBindingNames(trc, GetScopeDataTrailingNamesPointer(this), length);
}

inline void js::Scope::traceChildren(JSTracer* trc) {
  TraceNullableEdge(trc, &environmentShape_, "scope env shape");
  TraceNullableEdge(trc, &enclosingScope_, "scope enclosing");
  applyScopeDataTyped([trc](auto data) { data->trace(trc); });
}

template <uint32_t opts>
void js::GCMarker::eagerlyMarkChildren(Scope* scope) {
  do {
    if (Shape* shape = scope->environmentShape()) {
      markAndTraverseEdge<opts>(scope, shape);
    }
    mozilla::Span<AbstractBindingName<JSAtom>> names;
    switch (scope->kind()) {
      case ScopeKind::Function: {
        FunctionScope::RuntimeData& data = scope->as<FunctionScope>().data();
        if (data.canonicalFunction) {
          markAndTraverseObjectEdge<opts>(scope, data.canonicalFunction);
        }
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::FunctionBodyVar: {
        VarScope::RuntimeData& data = scope->as<VarScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
      case ScopeKind::FunctionLexical: {
        LexicalScope::RuntimeData& data = scope->as<LexicalScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::ClassBody: {
        ClassBodyScope::RuntimeData& data = scope->as<ClassBodyScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Global:
      case ScopeKind::NonSyntactic: {
        GlobalScope::RuntimeData& data = scope->as<GlobalScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Eval:
      case ScopeKind::StrictEval: {
        EvalScope::RuntimeData& data = scope->as<EvalScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::Module: {
        ModuleScope::RuntimeData& data = scope->as<ModuleScope>().data();
        if (data.module) {
          markAndTraverseObjectEdge<opts>(scope, data.module);
        }
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::With:
        break;

      case ScopeKind::WasmInstance: {
        WasmInstanceScope::RuntimeData& data =
            scope->as<WasmInstanceScope>().data();
        markAndTraverseObjectEdge<opts>(scope, data.instance);
        names = GetScopeDataTrailingNames(&data);
        break;
      }

      case ScopeKind::WasmFunction: {
        WasmFunctionScope::RuntimeData& data =
            scope->as<WasmFunctionScope>().data();
        names = GetScopeDataTrailingNames(&data);
        break;
      }
    }
    if (scope->kind_ == ScopeKind::Function) {
      for (auto& binding : names) {
        if (JSAtom* name = binding.name()) {
          markAndTraverseStringEdge<opts>(scope, name);
        }
      }
    } else {
      for (auto& binding : names) {
        markAndTraverseStringEdge<opts>(scope, binding.name());
      }
    }
    scope = scope->enclosing();
  } while (scope && mark<opts>(scope));
}

inline void js::BaseShape::traceChildren(JSTracer* trc) {
  // Note: the realm's global can be nullptr if we GC while creating the global.
  if (JSObject* global = realm()->unsafeUnbarrieredMaybeGlobal()) {
    TraceManuallyBarrieredEdge(trc, &global, "baseshape_global");
  }

  if (proto_.isObject()) {
    TraceEdge(trc, &proto_, "baseshape_proto");
  }
}

inline void js::GetterSetter::traceChildren(JSTracer* trc) {
  if (getter()) {
    TraceCellHeaderEdge(trc, this, "gettersetter_getter");
  }
  if (setter()) {
    TraceEdge(trc, &setter_, "gettersetter_setter");
  }
}

inline void js::PropMap::traceChildren(JSTracer* trc) {
  if (hasPrevious()) {
    TraceEdge(trc, &asLinked()->data_.previous, "propmap_previous");
  }

  if (isShared()) {
    SharedPropMap::TreeData& treeData = asShared()->treeDataRef();
    if (SharedPropMap* parent = treeData.parent.maybeMap()) {
      TraceManuallyBarrieredEdge(trc, &parent, "propmap_parent");
      if (parent != treeData.parent.map()) {
        treeData.setParent(parent, treeData.parent.index());
      }
    }
  }

  for (uint32_t i = 0; i < PropMap::Capacity; i++) {
    if (hasKey(i)) {
      TraceEdge(trc, &keys_[i], "propmap_key");
    }
  }

  if (canHaveTable() && asLinked()->hasTable()) {
    asLinked()->data_.table->trace(trc);
  }
}

template <uint32_t opts>
void js::GCMarker::eagerlyMarkChildren(PropMap* map) {
  MOZ_ASSERT(map->isMarkedAny());
  do {
    for (uint32_t i = 0; i < PropMap::Capacity; i++) {
      if (map->hasKey(i)) {
        markAndTraverseEdge<opts>(map, map->getKey(i));
      }
    }

    if (map->canHaveTable()) {
      // Special case: if a map has a table then all its pointers must point to
      // this map or an ancestor. Since these pointers will be traced by this
      // loop they do not need to be traced here as well.
      MOZ_ASSERT(map->asLinked()->canSkipMarkingTable());
    }

    if (map->isDictionary()) {
      map = map->asDictionary()->previous();
    } else {
      // For shared maps follow the |parent| link and not the |previous| link.
      // They're different when a map had a branch that wasn't at the end of the
      // map, but in this case they must have the same |previous| map. This is
      // asserted in SharedPropMap::addChild. In other words, marking all
      // |parent| maps will also mark all |previous| maps.
      map = map->asShared()->treeDataRef().parent.maybeMap();
    }
  } while (map && mark<opts>(map));
}

inline void JS::BigInt::traceChildren(JSTracer* trc) {}

// JitCode::traceChildren is not defined inline due to its dependence on
// MacroAssembler.

#endif  // gc_TraceMethods_inl_h
