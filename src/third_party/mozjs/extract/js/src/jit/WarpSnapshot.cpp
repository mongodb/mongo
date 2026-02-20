/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpSnapshot.h"

#include "mozilla/DebugOnly.h"

#include <type_traits>

#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRSpewer.h"
#include "js/Printer.h"
#include "vm/EnvironmentObject.h"
#include "vm/GetterSetter.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"

using namespace js;
using namespace js::jit;

static_assert(!std::is_polymorphic_v<WarpOpSnapshot>,
              "WarpOpSnapshot should not have any virtual methods");

WarpSnapshot::WarpSnapshot(JSContext* cx, TempAllocator& alloc,
                           WarpScriptSnapshotList&& scriptSnapshots,
                           const WarpZoneStubsSnapshot& zoneStubs,
                           const WarpBailoutInfo& bailoutInfo,
                           bool needsFinalWarmUpCount)
    : scriptSnapshots_(std::move(scriptSnapshots)),
      zoneStubs_(zoneStubs),
      globalLexicalEnv_(&cx->global()->lexicalEnvironment()),
      globalLexicalEnvThis_(globalLexicalEnv_->thisObject()),
      bailoutInfo_(bailoutInfo),
      nurseryObjects_(alloc) {
#ifdef JS_CACHEIR_SPEW
  needsFinalWarmUpCount_ = needsFinalWarmUpCount;
#endif
}

WarpScriptSnapshot::WarpScriptSnapshot(JSScript* script,
                                       const WarpEnvironment& env,
                                       WarpOpSnapshotList&& opSnapshots,
                                       ModuleObject* moduleObject)
    : script_(script),
      environment_(env),
      opSnapshots_(std::move(opSnapshots)),
      moduleObject_(moduleObject),
      isArrowFunction_(script->isFunction() && script->function()->isArrow()) {}

#ifdef JS_JITSPEW
void WarpSnapshot::dump() const {
  Fprinter out(stderr);
  dump(out);
}

void WarpSnapshot::dump(GenericPrinter& out) const {
  out.printf("WarpSnapshot (0x%p)\n", this);
  out.printf("------------------------------\n");
  out.printf("globalLexicalEnv: 0x%p\n", globalLexicalEnv());
  out.printf("globalLexicalEnvThis: 0x%p\n", globalLexicalEnvThis());
  out.printf("failedBoundsCheck: %u\n", bailoutInfo().failedBoundsCheck());
  out.printf("failedLexicalCheck: %u\n", bailoutInfo().failedLexicalCheck());
  out.printf("\n");

  out.printf("JitZone stubs:\n");
  for (const auto& stub : zoneStubs_) {
    unsigned index = &stub - zoneStubs_.begin();
    out.printf("Stub %u: 0x%p\n", index, stub);
  }
  out.printf("\n");

  out.printf("Nursery objects (%u):\n", unsigned(nurseryObjects_.length()));
  for (size_t i = 0; i < nurseryObjects_.length(); i++) {
    out.printf("  %u: 0x%p\n", unsigned(i), nurseryObjects_[i]);
  }
  out.printf("\n");

  for (auto* scriptSnapshot : scriptSnapshots_) {
    scriptSnapshot->dump(out);
  }
}

void WarpScriptSnapshot::dump(GenericPrinter& out) const {
  out.printf("WarpScriptSnapshot (0x%p)\n", this);
  out.printf("------------------------------\n");
  out.printf("Script: %s:%u:%u (0x%p)\n", script_->filename(),
             script_->lineno(), script_->column().oneOriginValue(),
             static_cast<JSScript*>(script_));
  out.printf("  moduleObject: 0x%p\n", moduleObject());
  out.printf("  isArrowFunction: %u\n", isArrowFunction());

  out.printf("  environment: ");
  environment_.match(
      [&](const NoEnvironment&) { out.printf("None\n"); },
      [&](JSObject* obj) { out.printf("Object: 0x%p\n", obj); },
      [&](const FunctionEnvironment& env) {
        out.printf(
            "Function: callobject template 0x%p, named lambda template: 0x%p,"
            " initial heap %u\n",
            static_cast<JSObject*>(env.callObjectTemplate),
            static_cast<JSObject*>(env.namedLambdaTemplate),
            unsigned(env.initialHeap));
      });

  out.printf("\n");
  for (const WarpOpSnapshot* snapshot : opSnapshots()) {
    snapshot->dump(out, script_);
    out.printf("\n");
  }
}

static const char* OpSnapshotKindString(WarpOpSnapshot::Kind kind) {
  static const char* const names[] = {
#  define NAME(x) #x,
      WARP_OP_SNAPSHOT_LIST(NAME)
#  undef NAME
  };
  return names[unsigned(kind)];
}

void WarpOpSnapshot::dump(GenericPrinter& out, JSScript* script) const {
  jsbytecode* pc = script->offsetToPC(offset_);
  out.printf("  %s (offset %u, JSOp::%s)\n", OpSnapshotKindString(kind_),
             offset_, CodeName(JSOp(*pc)));

  // Dispatch to dumpData() methods.
  switch (kind_) {
#  define DUMP(kind)             \
    case Kind::kind:             \
      as<kind>()->dumpData(out); \
      break;
    WARP_OP_SNAPSHOT_LIST(DUMP)
#  undef DUMP
  }
}

void WarpArguments::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObj());
}

void WarpRegExp::dumpData(GenericPrinter& out) const {
  out.printf("    hasShared: %u\n", hasShared());
}

void WarpBuiltinObject::dumpData(GenericPrinter& out) const {
  out.printf("    builtin: 0x%p\n", builtin());
}

void WarpGetIntrinsic::dumpData(GenericPrinter& out) const {
  out.printf("    intrinsic: 0x%016" PRIx64 "\n", intrinsic().asRawBits());
}

void WarpGetImport::dumpData(GenericPrinter& out) const {
  out.printf("    targetEnv: 0x%p\n", targetEnv());
  out.printf("    numFixedSlots: %u\n", numFixedSlots());
  out.printf("    slot: %u\n", slot());
  out.printf("    needsLexicalCheck: %u\n", needsLexicalCheck());
}

void WarpRest::dumpData(GenericPrinter& out) const {
  out.printf("    shape: 0x%p\n", shape());
}

void WarpBindUnqualifiedGName::dumpData(GenericPrinter& out) const {
  out.printf("    globalEnv: 0x%p\n", globalEnv());
}

void WarpVarEnvironment::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObj());
}

void WarpLexicalEnvironment::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObj());
}

void WarpClassBodyEnvironment::dumpData(GenericPrinter& out) const {
  out.printf("    template: 0x%p\n", templateObj());
}

void WarpBailout::dumpData(GenericPrinter& out) const {
  // No fields.
}

void WarpCacheIR::dumpData(GenericPrinter& out) const {
  out.printf("    stubCode: 0x%p\n", static_cast<JitCode*>(stubCode_));
  out.printf("    stubInfo: 0x%p\n", stubInfo_);
  out.printf("    stubData: 0x%p\n", stubData_);
#  ifdef JS_CACHEIR_SPEW
  out.printf("    IR:\n");
  SpewCacheIROps(out, "      ", stubInfo_);
#  else
  out.printf("(CacheIR spew unavailable)\n");
#  endif
}

void WarpInlinedCall::dumpData(GenericPrinter& out) const {
  out.printf("    scriptSnapshot: 0x%p\n", scriptSnapshot_);
  out.printf("    info: 0x%p\n", info_);
  cacheIRSnapshot_->dumpData(out);
}

void WarpPolymorphicTypes::dumpData(GenericPrinter& out) const {
  out.printf("    types:\n");
  for (auto& typeData : list_) {
    out.printf("      %s\n", ValTypeToString(typeData.type()));
  }
}

#endif  // JS_JITSPEW

void WarpSnapshot::trace(JSTracer* trc) {
  // Nursery objects can be tenured in parallel with Warp compilation.
  // Note: don't use TraceOffthreadGCPtr here as that asserts non-moving.
  for (size_t i = 0; i < nurseryObjects_.length(); i++) {
    TraceManuallyBarrieredEdge(trc, &nurseryObjects_[i], "warp-nursery-object");
  }

  // Other GC things are not in the nursery.
  if (trc->runtime()->heapState() == JS::HeapState::MinorCollecting) {
    return;
  }

  for (auto* script : scriptSnapshots_) {
    script->trace(trc);
  }
  for (JitCode* stub : zoneStubs_) {
    if (stub) {
      OffthreadGCPtr<JitCode*> ptr(stub);
      TraceOffthreadGCPtr(trc, ptr, "warp-zone-stub");
    }
  }
  TraceOffthreadGCPtr(trc, globalLexicalEnv_, "warp-lexical");
  TraceOffthreadGCPtr(trc, globalLexicalEnvThis_, "warp-lexicalthis");
}

void WarpScriptSnapshot::trace(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, script_, "warp-script");

  environment_.match([](const NoEnvironment&) {},
                     [trc](OffthreadGCPtr<JSObject*>& obj) {
                       TraceOffthreadGCPtr(trc, obj, "warp-env-object");
                     },
                     [trc](FunctionEnvironment& env) {
                       if (env.callObjectTemplate) {
                         TraceOffthreadGCPtr(trc, env.callObjectTemplate,
                                             "warp-env-callobject");
                       }
                       if (env.namedLambdaTemplate) {
                         TraceOffthreadGCPtr(trc, env.namedLambdaTemplate,
                                             "warp-env-namedlambda");
                       }
                     });

  for (WarpOpSnapshot* snapshot : opSnapshots_) {
    snapshot->trace(trc);
  }

  if (moduleObject_) {
    TraceOffthreadGCPtr(trc, moduleObject_, "warp-module-obj");
  }
}

void WarpOpSnapshot::trace(JSTracer* trc) {
  // Dispatch to traceData() methods.
  switch (kind_) {
#define TRACE(kind)             \
  case Kind::kind:              \
    as<kind>()->traceData(trc); \
    break;
    WARP_OP_SNAPSHOT_LIST(TRACE)
#undef TRACE
  }
}

void WarpArguments::traceData(JSTracer* trc) {
  if (templateObj_) {
    TraceOffthreadGCPtr(trc, templateObj_, "warp-args-template");
  }
}

void WarpRegExp::traceData(JSTracer* trc) {
  // No GC pointers.
}

void WarpBuiltinObject::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, builtin_, "warp-builtin-object");
}

void WarpGetIntrinsic::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, intrinsic_, "warp-intrinsic");
}

void WarpGetImport::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, targetEnv_, "warp-import-env");
}

void WarpRest::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, shape_, "warp-rest-shape");
}

void WarpBindUnqualifiedGName::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, globalEnv_, "warp-bindunqualifiedgname-globalenv");
}

void WarpVarEnvironment::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, templateObj_, "warp-varenv-template");
}

void WarpLexicalEnvironment::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, templateObj_, "warp-lexenv-template");
}

void WarpClassBodyEnvironment::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, templateObj_, "warp-classbodyenv-template");
}

void WarpBailout::traceData(JSTracer* trc) {
  // No GC pointers.
}

void WarpPolymorphicTypes::traceData(JSTracer* trc) {
  // No GC pointers.
}

template <typename T>
static void TraceWarpStubPtr(JSTracer* trc, uintptr_t word, const char* name) {
  T* ptr = reinterpret_cast<T*>(word);
  TraceOffthreadGCPtr(trc, OffthreadGCPtr<T*>(ptr), name);
}

void WarpCacheIR::traceData(JSTracer* trc) {
  TraceOffthreadGCPtr(trc, stubCode_, "warp-stub-code");
  if (stubData_) {
    uint32_t field = 0;
    size_t offset = 0;
    while (true) {
      StubField::Type fieldType = stubInfo_->fieldType(field);
      switch (fieldType) {
        case StubField::Type::RawInt32:
        case StubField::Type::RawPointer:
        case StubField::Type::RawInt64:
        case StubField::Type::Double:
          break;
        case StubField::Type::Shape:
        case StubField::Type::WeakShape: {
          // WeakShape pointers are traced strongly in this context.
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          TraceWarpStubPtr<Shape>(trc, word, "warp-cacheir-shape");
          break;
        }
        case StubField::Type::WeakGetterSetter: {
          // WeakGetterSetter pointers are traced strongly in this context.
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          TraceWarpStubPtr<GetterSetter>(trc, word,
                                         "warp-cacheir-getter-setter");
          break;
        }
        case StubField::Type::JSObject:
        case StubField::Type::WeakObject: {
          // WeakObject pointers are traced strongly in this context.
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          WarpObjectField field = WarpObjectField::fromData(word);
          if (!field.isNurseryIndex()) {
            TraceWarpStubPtr<JSObject>(trc, word, "warp-cacheir-object");
          }
          break;
        }
        case StubField::Type::Symbol: {
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          TraceWarpStubPtr<JS::Symbol>(trc, word, "warp-cacheir-symbol");
          break;
        }
        case StubField::Type::String: {
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          TraceWarpStubPtr<JSString>(trc, word, "warp-cacheir-string");
          break;
        }
        case StubField::Type::WeakBaseScript: {
          // WeakBaseScript pointers are traced strongly in this context.
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          TraceWarpStubPtr<BaseScript>(trc, word, "warp-cacheir-script");
          break;
        }
        case StubField::Type::JitCode: {
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          TraceWarpStubPtr<JitCode>(trc, word, "warp-cacheir-jitcode");
          break;
        }
        case StubField::Type::Id: {
          uintptr_t word = stubInfo_->getStubRawWord(stubData_, offset);
          jsid id = jsid::fromRawBits(word);
          TraceOffthreadGCPtr(trc, OffthreadGCPtr<jsid>(id),
                              "warp-cacheir-jsid");
          break;
        }
        case StubField::Type::Value: {
          uint64_t data = stubInfo_->getStubRawInt64(stubData_, offset);
          Value val = Value::fromRawBits(data);
          TraceOffthreadGCPtr(trc, OffthreadGCPtr<Value>(val),
                              "warp-cacheir-value");
          break;
        }
        case StubField::Type::AllocSite: {
          mozilla::DebugOnly<uintptr_t> word =
              stubInfo_->getStubRawWord(stubData_, offset);
          MOZ_ASSERT(word == uintptr_t(gc::Heap::Default) ||
                     word == uintptr_t(gc::Heap::Tenured));
          break;
        }
        case StubField::Type::Limit:
          return;  // Done.
      }
      field++;
      offset += StubField::sizeInBytes(fieldType);
    }
  }
}

void WarpInlinedCall::traceData(JSTracer* trc) {
  // Note: scriptSnapshot_ is traced through WarpSnapshot.
  cacheIRSnapshot_->trace(trc);
}
