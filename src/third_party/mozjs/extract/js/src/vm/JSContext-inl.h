/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSContext_inl_h
#define vm_JSContext_inl_h

#include "vm/JSContext.h"

#include <type_traits>

#include "gc/Marking.h"
#include "gc/Zone.h"
#include "jit/JitFrames.h"
#include "util/DiagnosticAssertions.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/Realm.h"

#include "vm/Activation-inl.h"  // js::Activation::hasWasmExitFP

namespace js {

class ContextChecks {
  JSContext* cx;

  JS::Realm* realm() const { return cx->realm(); }
  JS::Compartment* compartment() const { return cx->compartment(); }
  JS::Zone* zone() const { return cx->zone(); }

 public:
  explicit ContextChecks(JSContext* cx) : cx(cx) {
#ifdef DEBUG
    if (realm()) {
      GlobalObject* global = realm()->unsafeUnbarrieredMaybeGlobal();
      if (global) {
        checkObject(global);
      }
    }
#endif
  }

  /*
   * Set a breakpoint here (break js::ContextChecks::fail) to debug
   * realm/compartment/zone mismatches.
   */
  static void fail(JS::Realm* r1, JS::Realm* r2, int argIndex) {
    MOZ_CRASH_UNSAFE_PRINTF("*** Realm mismatch %p vs. %p at argument %d", r1,
                            r2, argIndex);
  }
  static void fail(JS::Compartment* c1, JS::Compartment* c2, int argIndex) {
    MOZ_CRASH_UNSAFE_PRINTF("*** Compartment mismatch %p vs. %p at argument %d",
                            c1, c2, argIndex);
  }
  static void fail(JS::Zone* z1, JS::Zone* z2, int argIndex) {
    MOZ_CRASH_UNSAFE_PRINTF("*** Zone mismatch %p vs. %p at argument %d", z1,
                            z2, argIndex);
  }

  void check(JS::Realm* r, int argIndex) {
    if (r && r != realm()) {
      fail(realm(), r, argIndex);
    }
  }

  void check(JS::Compartment* c, int argIndex) {
    if (c && c != compartment()) {
      fail(compartment(), c, argIndex);
    }
  }

  void check(JS::Zone* z, int argIndex) {
    if (zone() && z != zone()) {
      fail(zone(), z, argIndex);
    }
  }

  void check(JSObject* obj, int argIndex) {
    if (obj) {
      checkObject(obj);
      check(obj->compartment(), argIndex);
    }
  }

  void checkObject(JSObject* obj) {
    JS::AssertObjectIsNotGray(obj);
    MOZ_ASSERT(!js::gc::IsAboutToBeFinalizedUnbarriered(obj));
  }

  template <typename T>
  void checkAtom(T* thing, int argIndex) {
    static_assert(std::is_same_v<T, JSAtom> || std::is_same_v<T, JS::Symbol>,
                  "Should only be called with JSAtom* or JS::Symbol* argument");

#ifdef DEBUG
    // Atoms which move across zone boundaries need to be marked in the new
    // zone, see JS_MarkCrossZoneId.
    if (zone()) {
      if (!cx->runtime()->gc.atomMarking.atomIsMarked(zone(), thing)) {
        MOZ_CRASH_UNSAFE_PRINTF(
            "*** Atom not marked for zone %p at argument %d", zone(), argIndex);
      }
    }
#endif
  }

  void check(JSString* str, int argIndex) {
    JS::AssertCellIsNotGray(str);
    if (str->isAtom()) {
      checkAtom(&str->asAtom(), argIndex);
    } else {
      check(str->zone(), argIndex);
    }
  }

  void check(JS::Symbol* symbol, int argIndex) { checkAtom(symbol, argIndex); }

  void check(JS::BigInt* bi, int argIndex) { check(bi->zone(), argIndex); }

  void check(const js::Value& v, int argIndex) {
    if (v.isObject()) {
      check(&v.toObject(), argIndex);
    } else if (v.isString()) {
      check(v.toString(), argIndex);
    } else if (v.isSymbol()) {
      check(v.toSymbol(), argIndex);
    } else if (v.isBigInt()) {
      check(v.toBigInt(), argIndex);
    }
  }

  // Check the contents of any container class that supports the C++
  // iteration protocol, eg GCVector<jsid>.
  template <typename Container>
  std::enable_if_t<std::is_same_v<decltype(std::declval<Container>().begin()),
                                  decltype(std::declval<Container>().end())>>
  check(const Container& container, int argIndex) {
    for (auto i : container) {
      check(i, argIndex);
    }
  }

  void check(const JS::HandleValueArray& arr, int argIndex) {
    for (size_t i = 0; i < arr.length(); i++) {
      check(arr[i], argIndex);
    }
  }

  void check(const CallArgs& args, int argIndex) {
    for (Value* p = args.base(); p != args.end(); ++p) {
      check(*p, argIndex);
    }
  }

  void check(jsid id, int argIndex) {
    if (id.isAtom()) {
      checkAtom(id.toAtom(), argIndex);
    } else if (id.isSymbol()) {
      checkAtom(id.toSymbol(), argIndex);
    } else {
      MOZ_ASSERT(!id.isGCThing());
    }
  }

  void check(JSScript* script, int argIndex) {
    JS::AssertCellIsNotGray(script);
    if (script) {
      check(script->realm(), argIndex);
    }
  }

  void check(AbstractFramePtr frame, int argIndex);

  void check(const PropertyDescriptor& desc, int argIndex) {
    if (desc.hasGetter()) {
      check(desc.getter(), argIndex);
    }
    if (desc.hasSetter()) {
      check(desc.setter(), argIndex);
    }
    if (desc.hasValue()) {
      check(desc.value(), argIndex);
    }
  }

  void check(Handle<mozilla::Maybe<Value>> maybe, int argIndex) {
    if (maybe.get().isSome()) {
      check(maybe.get().ref(), argIndex);
    }
  }

  void check(Handle<mozilla::Maybe<PropertyDescriptor>> maybe, int argIndex) {
    if (maybe.get().isSome()) {
      check(maybe.get().ref(), argIndex);
    }
  }
};

}  // namespace js

template <class... Args>
inline void JSContext::checkImpl(const Args&... args) {
  int argIndex = 0;
  (..., js::ContextChecks(this).check(args, argIndex++));
}

template <class... Args>
inline void JSContext::check(const Args&... args) {
#ifdef JS_CRASH_DIAGNOSTICS
  if (contextChecksEnabled()) {
    checkImpl(args...);
  }
#endif
}

template <class... Args>
inline void JSContext::releaseCheck(const Args&... args) {
  if (contextChecksEnabled()) {
    checkImpl(args...);
  }
}

template <class... Args>
MOZ_ALWAYS_INLINE void JSContext::debugOnlyCheck(const Args&... args) {
#if defined(DEBUG) && defined(JS_CRASH_DIAGNOSTICS)
  if (contextChecksEnabled()) {
    checkImpl(args...);
  }
#endif
}

namespace js {

STATIC_PRECONDITION_ASSUME(ubound(args.argv_) >= argc)
MOZ_ALWAYS_INLINE bool CallNativeImpl(JSContext* cx, NativeImpl impl,
                                      const CallArgs& args) {
#ifdef DEBUG
  bool alreadyThrowing = cx->isExceptionPending();
#endif
  cx->check(args);
  bool ok = impl(cx, args);
  if (ok) {
    cx->check(args.rval());
    MOZ_ASSERT_IF(!alreadyThrowing, !cx->isExceptionPending());
  }
  return ok;
}

MOZ_ALWAYS_INLINE bool CheckForInterrupt(JSContext* cx) {
  MOZ_ASSERT(!cx->isExceptionPending());
  // Add an inline fast-path since we have to check for interrupts in some hot
  // C++ loops of library builtins.
  if (MOZ_UNLIKELY(cx->hasAnyPendingInterrupt())) {
    return cx->handleInterrupt();
  }

  JS_INTERRUPT_POSSIBLY_FAIL();

  return true;
}

} /* namespace js */

inline js::Nursery& JSContext::nursery() { return runtime()->gc.nursery(); }

inline void JSContext::minorGC(JS::GCReason reason) {
  runtime()->gc.minorGC(reason);
}

inline bool JSContext::runningWithTrustedPrincipals() {
  if (!realm()) {
    return true;
  }
  if (!runtime()->trustedPrincipals()) {
    return false;
  }
  return realm()->principals() == runtime()->trustedPrincipals();
}

inline void JSContext::enterRealm(JS::Realm* realm) {
  // We should never enter a realm while in the atoms zone.
  MOZ_ASSERT_IF(zone(), !zone()->isAtomsZone());

  realm->enter();
  setRealm(realm);
}

inline void JSContext::enterAtomsZone() {
  realm_ = nullptr;
  setZone(runtime_->unsafeAtomsZone());
}

inline void JSContext::setZone(js::Zone* zone) {
  MOZ_ASSERT(!isHelperThreadContext());
  zone_ = zone;
}

inline void JSContext::enterRealmOf(JSObject* target) {
  JS::AssertCellIsNotGray(target);
  enterRealm(target->nonCCWRealm());
}

inline void JSContext::enterRealmOf(JSScript* target) {
  JS::AssertCellIsNotGray(target);
  enterRealm(target->realm());
}

inline void JSContext::enterRealmOf(js::Shape* target) {
  JS::AssertCellIsNotGray(target);
  enterRealm(target->realm());
}

inline void JSContext::enterNullRealm() {
  // We should never enter a realm while in the atoms zone.
  MOZ_ASSERT_IF(zone(), !zone()->isAtomsZone());

  setRealm(nullptr);
}

inline void JSContext::leaveRealm(JS::Realm* oldRealm) {
  // Only call leave() after we've setRealm()-ed away from the current realm.
  JS::Realm* startingRealm = realm_;

  // The current realm should be marked as entered-from-C++ at this point.
  MOZ_ASSERT_IF(startingRealm, startingRealm->hasBeenEnteredIgnoringJit());

  setRealm(oldRealm);

  if (startingRealm) {
    startingRealm->leave();
  }
}

inline void JSContext::leaveAtomsZone(JS::Realm* oldRealm) {
  setRealm(oldRealm);
}

inline void JSContext::setRealm(JS::Realm* realm) {
  realm_ = realm;
  if (realm) {
    // This thread must have exclusive access to the zone.
    MOZ_ASSERT(CurrentThreadCanAccessZone(realm->zone()));
    MOZ_ASSERT(!realm->zone()->isAtomsZone());
    setZone(realm->zone());
  } else {
    setZone(nullptr);
  }
}

inline void JSContext::setRealmForJitExceptionHandler(JS::Realm* realm) {
  // JIT code enters (same-compartment) realms without calling realm->enter()
  // so we don't call realm->leave() here.
  MOZ_ASSERT(realm->compartment() == compartment());
  realm_ = realm;
}

inline JSScript* JSContext::currentScript(
    jsbytecode** ppc, AllowCrossRealm allowCrossRealm) const {
  if (ppc) {
    *ppc = nullptr;
  }

  js::Activation* act = activation();
  if (!act) {
    return nullptr;
  }

  MOZ_ASSERT(act->cx() == this);

  // Cross-compartment implies cross-realm.
  if (allowCrossRealm == AllowCrossRealm::DontAllow &&
      act->compartment() != compartment()) {
    return nullptr;
  }

  JSScript* script = nullptr;
  jsbytecode* pc = nullptr;
  if (act->isJit()) {
    if (act->hasWasmExitFP()) {
      return nullptr;
    }
    js::jit::GetPcScript(const_cast<JSContext*>(this), &script, &pc);
  } else {
    js::InterpreterFrame* fp = act->asInterpreter()->current();
    MOZ_ASSERT(!fp->runningInJit());
    script = fp->script();
    pc = act->asInterpreter()->regs().pc;
  }

  MOZ_ASSERT(script->containsPC(pc));

  if (allowCrossRealm == AllowCrossRealm::DontAllow &&
      script->realm() != realm()) {
    return nullptr;
  }

  if (ppc) {
    *ppc = pc;
  }
  return script;
}

inline js::RuntimeCaches& JSContext::caches() { return runtime()->caches(); }

#endif /* vm_JSContext_inl_h */
