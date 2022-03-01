/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/friend/DumpFunctions.h"

#include "mozilla/Maybe.h"  // mozilla::Maybe

#include <inttypes.h>  // PRIu64
#include <stddef.h>    // size_t
#include <stdio.h>     // fprintf, fflush

#include "jsfriendapi.h"  // js::WeakMapTracer
#include "jstypes.h"      // JS_PUBLIC_API

#include "gc/Allocator.h"   // js::CanGC
#include "gc/Cell.h"        // js::gc::Cell, js::gc::TenuredCell
#include "gc/GC.h"          // js::TraceRuntimeWithoutEviction
#include "gc/Heap.h"        // js::gc::Arena
#include "gc/Tracer.h"      // js::TraceChildren
#include "gc/WeakMap.h"     // js::IterateHeapUnbarriered, js::WeakMapBase
#include "js/GCAPI.h"       // JS::GCReason
#include "js/GCVector.h"    // JS::RootedVector
#include "js/HeapAPI.h"     // JS::GCCellPtr, js::gc::IsInsideNursery
#include "js/Id.h"          // JS::PropertyKey
#include "js/RootingAPI.h"  // JS::Handle, JS::Rooted
#include "js/TracingAPI.h"  // JS::CallbackTracer, JS_GetTraceThingInfo
#include "js/UbiNode.h"     // JS::ubi::Node
#include "js/Value.h"       // JS::Value
#include "js/Wrapper.h"     // js::UncheckedUnwrapWithoutExpose
#include "vm/BigIntType.h"  // JS::BigInt::dump
#include "vm/FrameIter.h"   // js::AllFramesIter, js::FrameIter
#include "vm/JSContext.h"   // JSContext
#include "vm/JSFunction.h"  // JSFunction
#include "vm/JSObject.h"    // JSObject
#include "vm/JSScript.h"    // JSScript
#include "vm/Printer.h"     // js::GenericPrinter, js::QuoteString, js::Sprinter
#include "vm/Realm.h"       // JS::Realm
#include "vm/Runtime.h"     // JSRuntime
#include "vm/Scope.h"       // js::PositionalFormalParameterIter
#include "vm/Stack.h"       // js::DONT_CHECK_ALIASING
#include "vm/StringType.h"  // JSAtom, JSString, js::ToString

#include "vm/JSObject-inl.h"  // js::IsCallable
#include "vm/Realm-inl.h"     // js::AutoRealm

namespace JS {

class JS_PUBLIC_API AutoRequireNoGC;
class JS_PUBLIC_API Zone;

}  // namespace JS

using JS::Handle;
using JS::MagicValue;
using JS::PropertyKey;
using JS::Realm;
using JS::Rooted;
using JS::RootedVector;
using JS::UniqueChars;
using JS::Value;
using JS::Zone;

using js::AllFramesIter;
using js::AutoRealm;
using js::CanGC;
using js::DONT_CHECK_ALIASING;
using js::FrameIter;
using js::PositionalFormalParameterIter;
using js::Sprinter;
using js::ToString;
using js::UncheckedUnwrapWithoutExpose;
using js::WeakMapTracer;

// We don't want jsfriendapi.h to depend on GenericPrinter,
// so these functions are declared directly in the cpp.

namespace js {

class InterpreterFrame;

extern JS_PUBLIC_API void DumpString(JSString* str, GenericPrinter& out);

extern JS_PUBLIC_API void DumpAtom(JSAtom* atom, GenericPrinter& out);

extern JS_PUBLIC_API void DumpObject(JSObject* obj, GenericPrinter& out);

extern JS_PUBLIC_API void DumpChars(const char16_t* s, size_t n,
                                    GenericPrinter& out);

extern JS_PUBLIC_API void DumpValue(const JS::Value& val, GenericPrinter& out);

extern JS_PUBLIC_API void DumpId(PropertyKey id, GenericPrinter& out);

extern JS_PUBLIC_API void DumpInterpreterFrame(
    JSContext* cx, GenericPrinter& out, InterpreterFrame* start = nullptr);

extern JS_PUBLIC_API void DumpBigInt(JS::BigInt* bi, GenericPrinter& out);

}  // namespace js

void js::DumpString(JSString* str, GenericPrinter& out) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  str->dump(out);
#endif
}

void js::DumpAtom(JSAtom* atom, GenericPrinter& out) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  atom->dump(out);
#endif
}

void js::DumpChars(const char16_t* s, size_t n, GenericPrinter& out) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  out.printf("char16_t * (%p) = ", (void*)s);
  JSString::dumpChars(s, n, out);
  out.putChar('\n');
#endif
}

void js::DumpObject(JSObject* obj, GenericPrinter& out) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  if (!obj) {
    out.printf("NULL\n");
    return;
  }
  obj->dump(out);
#endif
}

void js::DumpBigInt(JS::BigInt* bi, GenericPrinter& out) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  bi->dump(out);
#endif
}

void js::DumpString(JSString* str, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpString(str, out);
#endif
}

void js::DumpAtom(JSAtom* atom, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpAtom(atom, out);
#endif
}

void js::DumpChars(const char16_t* s, size_t n, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpChars(s, n, out);
#endif
}

void js::DumpObject(JSObject* obj, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpObject(obj, out);
#endif
}

void js::DumpBigInt(JS::BigInt* bi, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpBigInt(bi, out);
#endif
}

void js::DumpId(PropertyKey id, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpId(id, out);
#endif
}

void js::DumpValue(const JS::Value& val, FILE* fp) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(fp);
  js::DumpValue(val, out);
#endif
}

void js::DumpString(JSString* str) { DumpString(str, stderr); }
void js::DumpAtom(JSAtom* atom) { DumpAtom(atom, stderr); }
void js::DumpObject(JSObject* obj) { DumpObject(obj, stderr); }
void js::DumpChars(const char16_t* s, size_t n) { DumpChars(s, n, stderr); }
void js::DumpBigInt(JS::BigInt* bi) { DumpBigInt(bi, stderr); }
void js::DumpValue(const JS::Value& val) { DumpValue(val, stderr); }
void js::DumpId(PropertyKey id) { DumpId(id, stderr); }
void js::DumpInterpreterFrame(JSContext* cx, InterpreterFrame* start) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  Fprinter out(stderr);
  DumpInterpreterFrame(cx, out, start);
#endif
}

bool js::DumpPC(JSContext* cx) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  return DumpPC(cx, stdout);
#else
  return true;
#endif
}

bool js::DumpScript(JSContext* cx, JSScript* scriptArg) {
#if defined(DEBUG) || defined(JS_JITSPEW)
  return DumpScript(cx, scriptArg, stdout);
#else
  return true;
#endif
}

static const char* FormatValue(JSContext* cx, Handle<Value> v,
                               UniqueChars& bytes) {
  if (v.isMagic()) {
    MOZ_ASSERT(v.whyMagic() == JS_OPTIMIZED_OUT ||
               v.whyMagic() == JS_UNINITIALIZED_LEXICAL);
    return "[unavailable]";
  }

  if (js::IsCallable(v)) {
    return "[function]";
  }

  if (v.isObject() && js::IsCrossCompartmentWrapper(&v.toObject())) {
    return "[cross-compartment wrapper]";
  }

  JSString* str;
  {
    mozilla::Maybe<AutoRealm> ar;
    if (v.isObject()) {
      ar.emplace(cx, &v.toObject());
    }

    str = ToString<CanGC>(cx, v);
    if (!str) {
      return nullptr;
    }
  }

  bytes = js::QuoteString(cx, str, '"');
  return bytes.get();
}

static bool FormatFrame(JSContext* cx, const FrameIter& iter, Sprinter& sp,
                        int num, bool showArgs, bool showLocals,
                        bool showThisProps) {
  MOZ_ASSERT(!cx->isExceptionPending());
  Rooted<JSScript*> script(cx, iter.script());
  jsbytecode* pc = iter.pc();

  Rooted<JSObject*> envChain(cx, iter.environmentChain(cx));
  JSAutoRealm ar(cx, envChain);

  const char* filename = script->filename();
  unsigned column = 0;
  unsigned lineno = PCToLineNumber(script, pc, &column);
  Rooted<JSFunction*> fun(cx, iter.maybeCallee(cx));
  Rooted<JSString*> funname(cx);
  if (fun) {
    funname = fun->displayAtom();
  }

  Rooted<Value> thisVal(cx);
  if (iter.hasUsableAbstractFramePtr() && iter.isFunctionFrame() && fun &&
      !fun->isArrow() && !fun->isDerivedClassConstructor() &&
      !(fun->isBoundFunction() && iter.isConstructing())) {
    if (!GetFunctionThis(cx, iter.abstractFramePtr(), &thisVal)) {
      return false;
    }
  }

  // print the frame number and function name
  if (funname) {
    UniqueChars funbytes = js::QuoteString(cx, funname);
    if (!funbytes) {
      return false;
    }
    if (!sp.printf("%d %s(", num, funbytes.get())) {
      return false;
    }
  } else if (fun) {
    if (!sp.printf("%d anonymous(", num)) {
      return false;
    }
  } else {
    if (!sp.printf("%d <TOP LEVEL>", num)) {
      return false;
    }
  }

  if (showArgs && iter.hasArgs()) {
    PositionalFormalParameterIter fi(script);
    bool first = true;
    for (unsigned i = 0; i < iter.numActualArgs(); i++) {
      Rooted<Value> arg(cx);
      if (i < iter.numFormalArgs() && fi.closedOver()) {
        if (iter.hasInitialEnvironment(cx)) {
          arg = iter.callObj(cx).aliasedBinding(fi);
        } else {
          arg = MagicValue(JS_OPTIMIZED_OUT);
        }
      } else if (iter.hasUsableAbstractFramePtr()) {
        if (script->argsObjAliasesFormals() && iter.hasArgsObj()) {
          arg = iter.argsObj().arg(i);
        } else {
          arg = iter.unaliasedActual(i, DONT_CHECK_ALIASING);
        }
      } else {
        arg = MagicValue(JS_OPTIMIZED_OUT);
      }

      UniqueChars valueBytes;
      const char* value = FormatValue(cx, arg, valueBytes);
      if (!value) {
        if (cx->isThrowingOutOfMemory()) {
          return false;
        }
        cx->clearPendingException();
      }

      UniqueChars nameBytes;
      const char* name = nullptr;

      if (i < iter.numFormalArgs()) {
        MOZ_ASSERT(fi.argumentSlot() == i);
        if (!fi.isDestructured()) {
          nameBytes = StringToNewUTF8CharsZ(cx, *fi.name());
          name = nameBytes.get();
          if (!name) {
            return false;
          }
        } else {
          name = "(destructured parameter)";
        }
        fi++;
      }

      if (value) {
        if (!sp.printf("%s%s%s%s%s%s", !first ? ", " : "", name ? name : "",
                       name ? " = " : "", arg.isString() ? "\"" : "", value,
                       arg.isString() ? "\"" : "")) {
          return false;
        }

        first = false;
      } else {
        if (!sp.put("    <Failed to get argument while inspecting stack "
                    "frame>\n")) {
          return false;
        }
      }
    }
  }

  // print filename, line number and column
  if (!sp.printf("%s [\"%s\":%u:%u]\n", fun ? ")" : "",
                 filename ? filename : "<unknown>", lineno, column)) {
    return false;
  }

  // Note: Right now we don't dump the local variables anymore, because
  // that is hard to support across all the JITs etc.

  // print the value of 'this'
  if (showLocals) {
    if (!thisVal.isUndefined()) {
      Rooted<JSString*> thisValStr(cx, ToString<CanGC>(cx, thisVal));
      if (!thisValStr) {
        if (cx->isThrowingOutOfMemory()) {
          return false;
        }
        cx->clearPendingException();
      }
      if (thisValStr) {
        UniqueChars thisValBytes = QuoteString(cx, thisValStr);
        if (!thisValBytes) {
          return false;
        }
        if (!sp.printf("    this = %s\n", thisValBytes.get())) {
          return false;
        }
      } else {
        if (!sp.put("    <failed to get 'this' value>\n")) {
          return false;
        }
      }
    }
  }

  if (showThisProps && thisVal.isObject()) {
    Rooted<JSObject*> obj(cx, &thisVal.toObject());

    RootedVector<PropertyKey> keys(cx);
    if (!GetPropertyKeys(cx, obj, JSITER_OWNONLY, &keys)) {
      if (cx->isThrowingOutOfMemory()) {
        return false;
      }
      cx->clearPendingException();
    }

    for (size_t i = 0; i < keys.length(); i++) {
      Rooted<jsid> id(cx, keys[i]);
      Rooted<Value> key(cx, IdToValue(id));
      Rooted<Value> v(cx);

      if (!GetProperty(cx, obj, obj, id, &v)) {
        if (cx->isThrowingOutOfMemory()) {
          return false;
        }
        cx->clearPendingException();
        if (!sp.put("    <Failed to fetch property while inspecting stack "
                    "frame>\n")) {
          return false;
        }
        continue;
      }

      UniqueChars nameBytes;
      const char* name = FormatValue(cx, key, nameBytes);
      if (!name) {
        if (cx->isThrowingOutOfMemory()) {
          return false;
        }
        cx->clearPendingException();
      }

      UniqueChars valueBytes;
      const char* value = FormatValue(cx, v, valueBytes);
      if (!value) {
        if (cx->isThrowingOutOfMemory()) {
          return false;
        }
        cx->clearPendingException();
      }

      if (name && value) {
        if (!sp.printf("    this.%s = %s%s%s\n", name, v.isString() ? "\"" : "",
                       value, v.isString() ? "\"" : "")) {
          return false;
        }
      } else {
        if (!sp.put("    <Failed to format values while inspecting stack "
                    "frame>\n")) {
          return false;
        }
      }
    }
  }

  MOZ_ASSERT(!cx->isExceptionPending());
  return true;
}

static bool FormatWasmFrame(JSContext* cx, const FrameIter& iter, Sprinter& sp,
                            int num) {
  UniqueChars nameStr;
  if (JSAtom* functionDisplayAtom = iter.maybeFunctionDisplayAtom()) {
    nameStr = StringToNewUTF8CharsZ(cx, *functionDisplayAtom);
    if (!nameStr) {
      return false;
    }
  }

  if (!sp.printf("%d %s()", num, nameStr ? nameStr.get() : "<wasm-function>")) {
    return false;
  }

  if (!sp.printf(" [\"%s\":wasm-function[%u]:0x%x]\n",
                 iter.filename() ? iter.filename() : "<unknown>",
                 iter.wasmFuncIndex(), iter.wasmBytecodeOffset())) {
    return false;
  }

  MOZ_ASSERT(!cx->isExceptionPending());
  return true;
}

JS::UniqueChars JS::FormatStackDump(JSContext* cx, bool showArgs,
                                    bool showLocals, bool showThisProps) {
  int num = 0;

  Sprinter sp(cx);
  if (!sp.init()) {
    return nullptr;
  }

  for (AllFramesIter i(cx); !i.done(); ++i) {
    bool ok = i.hasScript() ? FormatFrame(cx, i, sp, num, showArgs, showLocals,
                                          showThisProps)
                            : FormatWasmFrame(cx, i, sp, num);
    if (!ok) {
      return nullptr;
    }
    num++;
  }

  if (num == 0) {
    if (!sp.put("JavaScript stack is empty\n")) {
      return nullptr;
    }
  }

  return sp.release();
}

struct DumpHeapTracer final : public JS::CallbackTracer, public WeakMapTracer {
  const char* prefix;
  FILE* output;
  mozilla::MallocSizeOf mallocSizeOf;

  DumpHeapTracer(FILE* fp, JSContext* cx, mozilla::MallocSizeOf mallocSizeOf)
      : JS::CallbackTracer(cx, JS::TracerKind::Callback,
                           JS::WeakMapTraceAction::Skip),
        WeakMapTracer(cx->runtime()),
        prefix(""),
        output(fp),
        mallocSizeOf(mallocSizeOf) {}

 private:
  void trace(JSObject* map, JS::GCCellPtr key, JS::GCCellPtr value) override {
    JSObject* kdelegate = nullptr;
    if (key.is<JSObject>()) {
      kdelegate = UncheckedUnwrapWithoutExpose(&key.as<JSObject>());
    }

    fprintf(output, "WeakMapEntry map=%p key=%p keyDelegate=%p value=%p\n", map,
            key.asCell(), kdelegate, value.asCell());
  }

  void onChild(const JS::GCCellPtr& thing) override;
};

static char MarkDescriptor(js::gc::Cell* thing) {
  js::gc::TenuredCell* cell = &thing->asTenured();
  if (cell->isMarkedBlack()) {
    return 'B';
  }
  if (cell->isMarkedGray()) {
    return 'G';
  }
  if (cell->isMarkedAny()) {
    return 'X';
  }
  return 'W';
}

static void DumpHeapVisitZone(JSRuntime* rt, void* data, Zone* zone,
                              const JS::AutoRequireNoGC& nogc) {
  DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
  fprintf(dtrc->output, "# zone %p\n", static_cast<void*>(zone));
}

static void DumpHeapVisitRealm(JSContext* cx, void* data, Realm* realm,
                               const JS::AutoRequireNoGC& nogc) {
  char name[1024];
  if (auto nameCallback = cx->runtime()->realmNameCallback) {
    nameCallback(cx, realm, name, sizeof(name), nogc);
  } else {
    strcpy(name, "<unknown>");
  }

  DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
  fprintf(dtrc->output, "# realm %s [in compartment %p, zone %p]\n", name,
          static_cast<void*>(realm->compartment()),
          static_cast<void*>(realm->zone()));
}

static void DumpHeapVisitArena(JSRuntime* rt, void* data, js::gc::Arena* arena,
                               JS::TraceKind traceKind, size_t thingSize,
                               const JS::AutoRequireNoGC& nogc) {
  DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
  fprintf(dtrc->output, "# arena allockind=%u size=%u\n",
          unsigned(arena->getAllocKind()), unsigned(thingSize));
}

static void DumpHeapVisitCell(JSRuntime* rt, void* data, JS::GCCellPtr cellptr,
                              size_t thingSize,
                              const JS::AutoRequireNoGC& nogc) {
  DumpHeapTracer* dtrc = static_cast<DumpHeapTracer*>(data);
  char cellDesc[1024 * 32];
  js::gc::GetTraceThingInfo(cellDesc, sizeof(cellDesc), cellptr.asCell(),
                            cellptr.kind(), true);

  fprintf(dtrc->output, "%p %c %s", cellptr.asCell(),
          MarkDescriptor(cellptr.asCell()), cellDesc);
  if (dtrc->mallocSizeOf) {
    auto size = JS::ubi::Node(cellptr).size(dtrc->mallocSizeOf);
    fprintf(dtrc->output, " SIZE:: %" PRIu64 "\n", size);
  } else {
    fprintf(dtrc->output, "\n");
  }

  JS::TraceChildren(dtrc, cellptr);
}

void DumpHeapTracer::onChild(const JS::GCCellPtr& thing) {
  if (js::gc::IsInsideNursery(thing.asCell())) {
    return;
  }

  char buffer[1024];
  context().getEdgeName(buffer, sizeof(buffer));
  fprintf(output, "%s%p %c %s\n", prefix, thing.asCell(),
          MarkDescriptor(thing.asCell()), buffer);
}

void js::DumpHeap(JSContext* cx, FILE* fp,
                  DumpHeapNurseryBehaviour nurseryBehaviour,
                  mozilla::MallocSizeOf mallocSizeOf) {
  if (nurseryBehaviour == CollectNurseryBeforeDump) {
    cx->runtime()->gc.evictNursery(JS::GCReason::API);
  }

  DumpHeapTracer dtrc(fp, cx, mallocSizeOf);

  fprintf(dtrc.output, "# Roots.\n");
  TraceRuntimeWithoutEviction(&dtrc);

  fprintf(dtrc.output, "# Weak maps.\n");
  WeakMapBase::traceAllMappings(&dtrc);

  fprintf(dtrc.output, "==========\n");

  dtrc.prefix = "> ";
  IterateHeapUnbarriered(cx, &dtrc, DumpHeapVisitZone, DumpHeapVisitRealm,
                         DumpHeapVisitArena, DumpHeapVisitCell);

  fflush(dtrc.output);
}
