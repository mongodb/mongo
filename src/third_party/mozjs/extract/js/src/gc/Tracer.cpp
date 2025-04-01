/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Tracer.h"

#include "mozilla/DebugOnly.h"

#include "NamespaceImports.h"

#include "gc/PublicIterators.h"
#include "jit/JitCode.h"
#include "util/Memory.h"
#include "util/Text.h"
#include "vm/BigIntType.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/RegExpShared.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/SymbolType.h"

#include "gc/TraceMethods-inl.h"
#include "vm/Shape-inl.h"

using namespace js;
using namespace js::gc;
using mozilla::DebugOnly;

template void RuntimeScopeData<LexicalScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<ClassBodyScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<VarScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<GlobalScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<EvalScope::SlotInfo>::trace(JSTracer* trc);
template void RuntimeScopeData<WasmFunctionScope::SlotInfo>::trace(
    JSTracer* trc);

void JS::TracingContext::getEdgeName(const char* name, char* buffer,
                                     size_t bufferSize) {
  MOZ_ASSERT(bufferSize > 0);
  if (functor_) {
    (*functor_)(this, buffer, bufferSize);
    return;
  }
  if (index_ != InvalidIndex) {
    snprintf(buffer, bufferSize, "%s[%zu]", name, index_);
    return;
  }
  snprintf(buffer, bufferSize, "%s", name);
}

/*** Public Tracing API *****************************************************/

JS_PUBLIC_API void JS::TraceChildren(JSTracer* trc, GCCellPtr thing) {
  ApplyGCThingTyped(thing.asCell(), thing.kind(), [trc](auto t) {
    MOZ_ASSERT_IF(t->runtimeFromAnyThread() != trc->runtime(),
                  t->isPermanentAndMayBeShared());
    t->traceChildren(trc);
  });
}

void js::gc::TraceIncomingCCWs(JSTracer* trc,
                               const JS::CompartmentSet& compartments) {
  for (CompartmentsIter source(trc->runtime()); !source.done(); source.next()) {
    if (compartments.has(source)) {
      continue;
    }
    // Iterate over all compartments that |source| has wrappers for.
    for (Compartment::WrappedObjectCompartmentEnum dest(source); !dest.empty();
         dest.popFront()) {
      if (!compartments.has(dest)) {
        continue;
      }
      // Iterate over all wrappers from |source| to |dest| compartments.
      for (Compartment::ObjectWrapperEnum e(source, dest); !e.empty();
           e.popFront()) {
        JSObject* obj = e.front().key();
        MOZ_ASSERT(compartments.has(obj->compartment()));
        mozilla::DebugOnly<JSObject*> prior = obj;
        TraceManuallyBarrieredEdge(trc, &obj,
                                   "cross-compartment wrapper target");
        MOZ_ASSERT(obj == prior);
      }
    }
  }
}

/*** Cycle Collector Helpers ************************************************/

// This function is used by the Cycle Collector (CC) to trace through -- or in
// CC parlance, traverse -- a Shape. The CC does not care about Shapes,
// BaseShapes or PropMaps, only the JSObjects held live by them. Thus, we only
// report non-Shape things.
void gc::TraceCycleCollectorChildren(JS::CallbackTracer* trc, Shape* shape) {
  shape->base()->traceChildren(trc);
  // Don't trace the PropMap because the CC doesn't care about PropertyKey.
}

/*** Traced Edge Printer ****************************************************/

static size_t CountDecimalDigits(size_t num) {
  size_t numDigits = 0;
  do {
    num /= 10;
    numDigits++;
  } while (num > 0);

  return numDigits;
}

static const char* StringKindHeader(JSString* str) {
  MOZ_ASSERT(str->isLinear());

  if (str->isAtom()) {
    if (str->isPermanentAtom()) {
      return "permanent atom: ";
    }
    return "atom: ";
  }

  if (str->isExtensible()) {
    return "extensible: ";
  }

  if (str->isInline()) {
    if (str->isFatInline()) {
      return "fat inline: ";
    }
    return "inline: ";
  }

  if (str->isDependent()) {
    return "dependent: ";
  }

  if (str->isExternal()) {
    return "external: ";
  }

  return "linear: ";
}

void js::gc::GetTraceThingInfo(char* buf, size_t bufsize, void* thing,
                               JS::TraceKind kind, bool details) {
  const char* name = nullptr; /* silence uninitialized warning */
  size_t n;

  if (bufsize == 0) {
    return;
  }

  switch (kind) {
    case JS::TraceKind::BaseShape:
      name = "base_shape";
      break;

    case JS::TraceKind::GetterSetter:
      name = "getter_setter";
      break;

    case JS::TraceKind::PropMap:
      name = "prop_map";
      break;

    case JS::TraceKind::JitCode:
      name = "jitcode";
      break;

    case JS::TraceKind::Null:
      name = "null_pointer";
      break;

    case JS::TraceKind::Object: {
      name = static_cast<JSObject*>(thing)->getClass()->name;
      break;
    }

    case JS::TraceKind::RegExpShared:
      name = "reg_exp_shared";
      break;

    case JS::TraceKind::Scope:
      name = "scope";
      break;

    case JS::TraceKind::Script:
      name = "script";
      break;

    case JS::TraceKind::Shape:
      name = "shape";
      break;

    case JS::TraceKind::String:
      name = ((JSString*)thing)->isDependent() ? "substring" : "string";
      break;

    case JS::TraceKind::Symbol:
      name = "symbol";
      break;

    case JS::TraceKind::BigInt:
      name = "BigInt";
      break;

    default:
      name = "INVALID";
      break;
  }

  n = strlen(name);
  if (n > bufsize - 1) {
    n = bufsize - 1;
  }
  js_memcpy(buf, name, n + 1);
  buf += n;
  bufsize -= n;
  *buf = '\0';

  if (details && bufsize > 2) {
    switch (kind) {
      case JS::TraceKind::Object: {
        JSObject* obj = (JSObject*)thing;
        if (obj->is<JSFunction>()) {
          JSFunction* fun = &obj->as<JSFunction>();
          if (fun->maybePartialDisplayAtom()) {
            *buf++ = ' ';
            bufsize--;
            PutEscapedString(buf, bufsize, fun->maybePartialDisplayAtom(), 0);
          }
        } else {
          snprintf(buf, bufsize, " <unknown object>");
        }
        break;
      }

      case JS::TraceKind::Script: {
        auto* script = static_cast<js::BaseScript*>(thing);
        snprintf(buf, bufsize, " %s:%u", script->filename(), script->lineno());
        break;
      }

      case JS::TraceKind::String: {
        *buf++ = ' ';
        bufsize--;
        JSString* str = (JSString*)thing;

        if (str->isLinear()) {
          const char* header = StringKindHeader(str);
          bool willFit = str->length() + strlen("<length > ") + strlen(header) +
                             CountDecimalDigits(str->length()) <
                         bufsize;

          n = snprintf(buf, bufsize, "<%slength %zu%s> ", header, str->length(),
                       willFit ? "" : " (truncated)");
          buf += n;
          bufsize -= n;

          PutEscapedString(buf, bufsize, &str->asLinear(), 0);
        } else {
          snprintf(buf, bufsize, "<rope: length %zu>", str->length());
        }
        break;
      }

      case JS::TraceKind::Symbol: {
        *buf++ = ' ';
        bufsize--;
        auto* sym = static_cast<JS::Symbol*>(thing);
        if (JSAtom* desc = sym->description()) {
          PutEscapedString(buf, bufsize, desc, 0);
        } else {
          snprintf(buf, bufsize, "<null>");
        }
        break;
      }

      case JS::TraceKind::Scope: {
        auto* scope = static_cast<js::Scope*>(thing);
        snprintf(buf, bufsize, " %s", js::ScopeKindString(scope->kind()));
        break;
      }

      default:
        break;
    }
  }
  buf[bufsize - 1] = '\0';
}

JS::CallbackTracer::CallbackTracer(JSContext* cx, JS::TracerKind kind,
                                   JS::TraceOptions options)
    : CallbackTracer(cx->runtime(), kind, options) {}
