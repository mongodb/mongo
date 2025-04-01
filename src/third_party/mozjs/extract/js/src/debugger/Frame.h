/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef debugger_Frame_h
#define debugger_Frame_h

#include "mozilla/Maybe.h"   // for Maybe
#include "mozilla/Range.h"   // for Range
#include "mozilla/Result.h"  // for Result

#include <stddef.h>  // for size_t

#include "NamespaceImports.h"   // for Value, MutableHandleValue, HandleObject
#include "debugger/DebugAPI.h"  // for ResumeMode
#include "debugger/Debugger.h"  // for ResumeMode, Handler, Debugger
#include "gc/Barrier.h"         // for HeapPtr
#include "vm/FrameIter.h"       // for FrameIter
#include "vm/JSObject.h"        // for JSObject
#include "vm/NativeObject.h"    // for NativeObject
#include "vm/Stack.h"           // for AbstractFramePtr

struct JS_PUBLIC_API JSContext;

namespace js {

class AbstractGeneratorObject;
class GlobalObject;

/*
 * An OnStepHandler represents a handler function that is called when a small
 * amount of progress is made in a frame.
 */
struct OnStepHandler : Handler {
  /*
   * If we have made a small amount of progress in a frame, this method is
   * called with the frame as argument. If succesful, this method should
   * return true, with `resumeMode` and `vp` set to a resumption value
   * specifiying how execution should continue.
   */
  virtual bool onStep(JSContext* cx, Handle<DebuggerFrame*> frame,
                      ResumeMode& resumeMode, MutableHandleValue vp) = 0;
};

class ScriptedOnStepHandler final : public OnStepHandler {
 public:
  explicit ScriptedOnStepHandler(JSObject* object);
  virtual JSObject* object() const override;
  virtual void hold(JSObject* owner) override;
  virtual void drop(JS::GCContext* gcx, JSObject* owner) override;
  virtual void trace(JSTracer* tracer) override;
  virtual size_t allocSize() const override;
  virtual bool onStep(JSContext* cx, Handle<DebuggerFrame*> frame,
                      ResumeMode& resumeMode, MutableHandleValue vp) override;

 private:
  const HeapPtr<JSObject*> object_;
};

/*
 * An OnPopHandler represents a handler function that is called just before a
 * frame is popped.
 */
struct OnPopHandler : Handler {
  /*
   * The given `frame` is about to be popped; `completion` explains why.
   *
   * When this method returns true, it must set `resumeMode` and `vp` to a
   * resumption value specifying how execution should continue.
   *
   * When this method returns false, it should set an exception on `cx`.
   */
  virtual bool onPop(JSContext* cx, Handle<DebuggerFrame*> frame,
                     const Completion& completion, ResumeMode& resumeMode,
                     MutableHandleValue vp) = 0;
};

class ScriptedOnPopHandler final : public OnPopHandler {
 public:
  explicit ScriptedOnPopHandler(JSObject* object);
  virtual JSObject* object() const override;
  virtual void hold(JSObject* owner) override;
  virtual void drop(JS::GCContext* gcx, JSObject* owner) override;
  virtual void trace(JSTracer* tracer) override;
  virtual size_t allocSize() const override;
  virtual bool onPop(JSContext* cx, Handle<DebuggerFrame*> frame,
                     const Completion& completion, ResumeMode& resumeMode,
                     MutableHandleValue vp) override;

 private:
  const HeapPtr<JSObject*> object_;
};

enum class DebuggerFrameType { Eval, Global, Call, Module, WasmCall };

enum class DebuggerFrameImplementation { Interpreter, Baseline, Ion, Wasm };

class DebuggerArguments : public NativeObject {
 public:
  static const JSClass class_;

  static DebuggerArguments* create(JSContext* cx, HandleObject proto,
                                   Handle<DebuggerFrame*> frame);

 private:
  enum { FRAME_SLOT };

  static const unsigned RESERVED_SLOTS = 1;
};

class DebuggerFrame : public NativeObject {
  friend class DebuggerArguments;
  friend class ScriptedOnStepHandler;
  friend class ScriptedOnPopHandler;

 public:
  static const JSClass class_;

  enum {
    FRAME_ITER_SLOT = 0,
    OWNER_SLOT,
    ARGUMENTS_SLOT,
    ONSTEP_HANDLER_SLOT,
    ONPOP_HANDLER_SLOT,

    // If this is a frame for a generator call, and the generator object has
    // been created (which doesn't happen until after default argument
    // evaluation and destructuring), then this is a PrivateValue pointing to a
    // GeneratorInfo struct that points to the call's AbstractGeneratorObject.
    // This allows us to implement Debugger.Frame methods even while the call is
    // suspended, and we have no FrameIter::Data.
    //
    // While Debugger::generatorFrames maps an AbstractGeneratorObject to its
    // Debugger.Frame, this link represents the reverse relation, from a
    // Debugger.Frame to its generator object. This slot is set if and only if
    // there is a corresponding entry in generatorFrames.
    GENERATOR_INFO_SLOT,

    RESERVED_SLOTS,
  };

  void trace(JSTracer* trc);

  static NativeObject* initClass(JSContext* cx, Handle<GlobalObject*> global,
                                 HandleObject dbgCtor);
  static DebuggerFrame* create(JSContext* cx, HandleObject proto,
                               Handle<NativeObject*> debugger,
                               const FrameIter* maybeIter,
                               Handle<AbstractGeneratorObject*> maybeGenerator);

  [[nodiscard]] static bool getArguments(
      JSContext* cx, Handle<DebuggerFrame*> frame,
      MutableHandle<DebuggerArguments*> result);
  [[nodiscard]] static bool getCallee(JSContext* cx,
                                      Handle<DebuggerFrame*> frame,
                                      MutableHandle<DebuggerObject*> result);
  [[nodiscard]] static bool getIsConstructing(JSContext* cx,
                                              Handle<DebuggerFrame*> frame,
                                              bool& result);
  [[nodiscard]] static bool getEnvironment(
      JSContext* cx, Handle<DebuggerFrame*> frame,
      MutableHandle<DebuggerEnvironment*> result);
  [[nodiscard]] static bool getOffset(JSContext* cx,
                                      Handle<DebuggerFrame*> frame,
                                      size_t& result);
  [[nodiscard]] static bool getOlder(JSContext* cx,
                                     Handle<DebuggerFrame*> frame,
                                     MutableHandle<DebuggerFrame*> result);
  [[nodiscard]] static bool getAsyncPromise(
      JSContext* cx, Handle<DebuggerFrame*> frame,
      MutableHandle<DebuggerObject*> result);
  [[nodiscard]] static bool getOlderSavedFrame(
      JSContext* cx, Handle<DebuggerFrame*> frame,
      MutableHandle<SavedFrame*> result);
  [[nodiscard]] static bool getThis(JSContext* cx, Handle<DebuggerFrame*> frame,
                                    MutableHandleValue result);
  static DebuggerFrameType getType(Handle<DebuggerFrame*> frame);
  static DebuggerFrameImplementation getImplementation(
      Handle<DebuggerFrame*> frame);
  [[nodiscard]] static bool setOnStepHandler(JSContext* cx,
                                             Handle<DebuggerFrame*> frame,
                                             UniquePtr<OnStepHandler> handler);

  [[nodiscard]] static JS::Result<Completion> eval(
      JSContext* cx, Handle<DebuggerFrame*> frame,
      mozilla::Range<const char16_t> chars, HandleObject bindings,
      const EvalOptions& options);

  [[nodiscard]] static DebuggerFrame* check(JSContext* cx, HandleValue thisv);

  bool isOnStack() const;

  bool isSuspended() const;

  OnStepHandler* onStepHandler() const;
  OnPopHandler* onPopHandler() const;
  void setOnPopHandler(JSContext* cx, OnPopHandler* handler);

  inline bool hasGeneratorInfo() const;

  // If hasGeneratorInfo(), return an direct cross-compartment reference to this
  // Debugger.Frame's generator object.
  AbstractGeneratorObject& unwrappedGenerator() const;

#ifdef DEBUG
  JSScript* generatorScript() const;
#endif

  /*
   * Associate the generator object genObj with this Debugger.Frame. This
   * association allows the Debugger.Frame to track the generator's execution
   * across suspensions and resumptions, and to implement some methods even
   * while the generator is suspended.
   *
   * The context `cx` must be in the Debugger.Frame's realm, and `genObj` must
   * be in a debuggee realm.
   *
   * Technically, the generator activation need not actually be on the stack
   * right now; it's okay to call this method on a Debugger.Frame that has no
   * ScriptFrameIter::Data at present. However, this function has no way to
   * verify that genObj really is the generator associated with the call for
   * which this Debugger.Frame was originally created, so it's best to make the
   * association while the call is on the stack, and the relationships are easy
   * to discern.
   */
  [[nodiscard]] static bool setGeneratorInfo(
      JSContext* cx, Handle<DebuggerFrame*> frame,
      Handle<AbstractGeneratorObject*> genObj);

  /*
   * Undo the effects of a prior call to setGenerator.
   *
   * If provided, owner must be the Debugger to which this Debugger.Frame
   * belongs; remove this frame's entry from its generatorFrames map, and clean
   * up its cross-compartment wrapper table entry. The owner must be passed
   * unless this method is being called from the Debugger.Frame's finalizer. (In
   * that case, the owner is not reliably available, and is not actually
   * necessary.)
   *
   * If maybeGeneratorFramesEnum is non-null, use it to remove this frame's
   * entry from the Debugger's generatorFrames weak map. In this case, this
   * function will not otherwise disturb generatorFrames. Passing the enum
   * allows this function to be used while iterating over generatorFrames.
   */
  void clearGeneratorInfo(JS::GCContext* gcx);

  /*
   * Called after a generator/async frame is resumed, before exposing this
   * Debugger.Frame object to any hooks.
   */
  bool resume(const FrameIter& iter);

  bool hasAnyHooks() const;

  Debugger* owner() const;

 private:
  static const JSClassOps classOps_;

  static const JSPropertySpec properties_[];
  static const JSFunctionSpec methods_[];

  static void finalize(JS::GCContext* gcx, JSObject* obj);

  static AbstractFramePtr getReferent(Handle<DebuggerFrame*> frame);
  [[nodiscard]] static bool requireScriptReferent(JSContext* cx,
                                                  Handle<DebuggerFrame*> frame);

  [[nodiscard]] static bool construct(JSContext* cx, unsigned argc, Value* vp);

  struct CallData;

  [[nodiscard]] bool incrementStepperCounter(JSContext* cx,
                                             AbstractFramePtr referent);
  [[nodiscard]] bool incrementStepperCounter(JSContext* cx,
                                             HandleScript script);
  void decrementStepperCounter(JS::GCContext* gcx, JSScript* script);
  void decrementStepperCounter(JS::GCContext* gcx, AbstractFramePtr referent);

  FrameIter::Data* frameIterData() const;
  void setFrameIterData(FrameIter::Data*);
  void freeFrameIterData(JS::GCContext* gcx);

 public:
  FrameIter getFrameIter(JSContext* cx);

  void terminate(JS::GCContext* gcx, AbstractFramePtr frame);
  void onGeneratorClosed(JS::GCContext* gcx);
  void suspend(JS::GCContext* gcx);

  [[nodiscard]] bool replaceFrameIterData(JSContext* cx, const FrameIter&);

  class GeneratorInfo;
  inline GeneratorInfo* generatorInfo() const;
};

} /* namespace js */

#endif /* debugger_Frame_h */
