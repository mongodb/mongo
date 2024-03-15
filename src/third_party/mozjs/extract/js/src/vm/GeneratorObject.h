/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_GeneratorObject_h
#define vm_GeneratorObject_h

#include "js/Class.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayObject.h"
#include "vm/BytecodeUtil.h"
#include "vm/GeneratorResumeKind.h"  // GeneratorResumeKind
#include "vm/JSObject.h"
#include "vm/Stack.h"

namespace js {

class InterpreterActivation;

namespace frontend {
class TaggedParserAtomIndex;
}

extern const JSClass GeneratorFunctionClass;

class AbstractGeneratorObject : public NativeObject {
 public:
  // Magic value stored in the resumeIndex slot when the generator is
  // running or closing. See the resumeIndex comment below.
  static const int32_t RESUME_INDEX_RUNNING = INT32_MAX;

  enum {
    CALLEE_SLOT = 0,
    ENV_CHAIN_SLOT,
    ARGS_OBJ_SLOT,
    STACK_STORAGE_SLOT,
    RESUME_INDEX_SLOT,
    RESERVED_SLOTS
  };

  // Maximum number of fixed stack slots in a generator or async function
  // script. If a script would have more, we instead store some variables in
  // heap EnvironmentObjects.
  //
  // This limit is a performance heuristic. Stack slots reduce allocations,
  // and `Local` opcodes are a bit faster than `AliasedVar` ones; but at each
  // `yield` or `await` the stack slots must be memcpy'd into a
  // GeneratorObject. At some point the memcpy is too much. The limit is
  // plenty for typical human-authored code.
  static constexpr uint32_t FixedSlotLimit = 256;

 private:
  static JSObject* createModuleGenerator(JSContext* cx, AbstractFramePtr frame);

 public:
  static JSObject* createFromFrame(JSContext* cx, AbstractFramePtr frame);
  static AbstractGeneratorObject* create(JSContext* cx, HandleFunction callee,
                                         HandleScript script,
                                         HandleObject environmentChain,
                                         Handle<ArgumentsObject*> argsObject);

  static bool resume(JSContext* cx, InterpreterActivation& activation,
                     Handle<AbstractGeneratorObject*> genObj, HandleValue arg,
                     HandleValue resumeKind);

  static bool suspend(JSContext* cx, HandleObject obj, AbstractFramePtr frame,
                      const jsbytecode* pc, unsigned nvalues);

  static void finalSuspend(HandleObject obj);

  JSFunction& callee() const {
    return getFixedSlot(CALLEE_SLOT).toObject().as<JSFunction>();
  }
  void setCallee(JSFunction& callee) {
    setFixedSlot(CALLEE_SLOT, ObjectValue(callee));
  }

  JSObject& environmentChain() const {
    return getFixedSlot(ENV_CHAIN_SLOT).toObject();
  }
  void setEnvironmentChain(JSObject& envChain) {
    setFixedSlot(ENV_CHAIN_SLOT, ObjectValue(envChain));
  }

  bool hasArgsObj() const { return getFixedSlot(ARGS_OBJ_SLOT).isObject(); }
  ArgumentsObject& argsObj() const {
    return getFixedSlot(ARGS_OBJ_SLOT).toObject().as<ArgumentsObject>();
  }
  void setArgsObj(ArgumentsObject& argsObj) {
    setFixedSlot(ARGS_OBJ_SLOT, ObjectValue(argsObj));
  }

  bool hasStackStorage() const {
    return getFixedSlot(STACK_STORAGE_SLOT).isObject();
  }
  bool isStackStorageEmpty() const {
    return stackStorage().getDenseInitializedLength() == 0;
  }
  ArrayObject& stackStorage() const {
    return getFixedSlot(STACK_STORAGE_SLOT).toObject().as<ArrayObject>();
  }
  void setStackStorage(ArrayObject& stackStorage) {
    setFixedSlot(STACK_STORAGE_SLOT, ObjectValue(stackStorage));
  }

  // Access stack storage. Requires `hasStackStorage() && isSuspended()`.
  // `slot` is the index of the desired local in the stack frame when this
  // generator is *not* suspended.
  const Value& getUnaliasedLocal(uint32_t slot) const;
  void setUnaliasedLocal(uint32_t slot, const Value& value);

  // The resumeIndex slot is abused for a few purposes.  It's undefined if
  // it hasn't been set yet (before the initial yield), and null if the
  // generator is closed. If the generator is running, the resumeIndex is
  // RESUME_INDEX_RUNNING.
  //
  // If the generator is suspended, it's the resumeIndex (stored as
  // JSOp::InitialYield/JSOp::Yield/JSOp::Await operand) of the yield
  // instruction that suspended the generator. The resumeIndex can be mapped to
  // the bytecode offset (interpreter) or to the native code offset (JIT).

  bool isBeforeInitialYield() const {
    return getFixedSlot(RESUME_INDEX_SLOT).isUndefined();
  }
  bool isRunning() const {
    return getFixedSlot(RESUME_INDEX_SLOT) == Int32Value(RESUME_INDEX_RUNNING);
  }
  bool isSuspended() const {
    // Note: also update Baseline's IsSuspendedGenerator code if this
    // changes.
    Value resumeIndex = getFixedSlot(RESUME_INDEX_SLOT);
    return resumeIndex.isInt32() &&
           resumeIndex.toInt32() < RESUME_INDEX_RUNNING;
  }
  void setRunning() {
    MOZ_ASSERT(isSuspended());
    setFixedSlot(RESUME_INDEX_SLOT, Int32Value(RESUME_INDEX_RUNNING));
  }
  void setResumeIndex(const jsbytecode* pc) {
    MOZ_ASSERT(JSOp(*pc) == JSOp::InitialYield || JSOp(*pc) == JSOp::Yield ||
               JSOp(*pc) == JSOp::Await);

    MOZ_ASSERT_IF(JSOp(*pc) == JSOp::InitialYield,
                  getFixedSlot(RESUME_INDEX_SLOT).isUndefined());
    MOZ_ASSERT_IF(JSOp(*pc) != JSOp::InitialYield, isRunning());

    uint32_t resumeIndex = GET_UINT24(pc);
    MOZ_ASSERT(resumeIndex < uint32_t(RESUME_INDEX_RUNNING));

    setFixedSlot(RESUME_INDEX_SLOT, Int32Value(resumeIndex));
    MOZ_ASSERT(isSuspended());
  }
  void setResumeIndex(int32_t resumeIndex) {
    setFixedSlot(RESUME_INDEX_SLOT, Int32Value(resumeIndex));
  }
  uint32_t resumeIndex() const {
    MOZ_ASSERT(isSuspended());
    return getFixedSlot(RESUME_INDEX_SLOT).toInt32();
  }
  bool isClosed() const { return getFixedSlot(CALLEE_SLOT).isNull(); }
  void setClosed() {
    setFixedSlot(CALLEE_SLOT, NullValue());
    setFixedSlot(ENV_CHAIN_SLOT, NullValue());
    setFixedSlot(ARGS_OBJ_SLOT, NullValue());
    setFixedSlot(STACK_STORAGE_SLOT, NullValue());
    setFixedSlot(RESUME_INDEX_SLOT, NullValue());
  }

  bool isAfterYield();
  bool isAfterAwait();

 private:
  bool isAfterYieldOrAwait(JSOp op);

 public:
  void trace(JSTracer* trc);

  static size_t offsetOfCalleeSlot() { return getFixedSlotOffset(CALLEE_SLOT); }
  static size_t offsetOfEnvironmentChainSlot() {
    return getFixedSlotOffset(ENV_CHAIN_SLOT);
  }
  static size_t offsetOfArgsObjSlot() {
    return getFixedSlotOffset(ARGS_OBJ_SLOT);
  }
  static size_t offsetOfResumeIndexSlot() {
    return getFixedSlotOffset(RESUME_INDEX_SLOT);
  }
  static size_t offsetOfStackStorageSlot() {
    return getFixedSlotOffset(STACK_STORAGE_SLOT);
  }

  static size_t calleeSlot() { return CALLEE_SLOT; }
  static size_t envChainSlot() { return ENV_CHAIN_SLOT; }
  static size_t argsObjectSlot() { return ARGS_OBJ_SLOT; }
  static size_t stackStorageSlot() { return STACK_STORAGE_SLOT; }
  static size_t resumeIndexSlot() { return RESUME_INDEX_SLOT; }

#ifdef DEBUG
  void dump() const;
#endif
};

class GeneratorObject : public AbstractGeneratorObject {
 public:
  enum { RESERVED_SLOTS = AbstractGeneratorObject::RESERVED_SLOTS };

  static const JSClass class_;
  static const JSClassOps classOps_;

  static GeneratorObject* create(JSContext* cx, HandleFunction fun);
};

bool GeneratorThrowOrReturn(JSContext* cx, AbstractFramePtr frame,
                            Handle<AbstractGeneratorObject*> obj,
                            HandleValue val, GeneratorResumeKind resumeKind);

/**
 * Return the generator object associated with the given frame. The frame must
 * be a call frame for a generator.
 *
 * This may return nullptr at certain points in the generator lifecycle:
 *
 * - While a generator call evaluates default argument values and performs
 *   destructuring, which occurs before the generator object is created.
 *
 * - Between the `Generator` instruction and the `SetAliasedVar .generator`
 *   instruction, at which point the generator object does exist, but is held
 *   only on the stack, and not the `.generator` pseudo-variable this function
 *   consults.
 */
AbstractGeneratorObject* GetGeneratorObjectForFrame(JSContext* cx,
                                                    AbstractFramePtr frame);

/**
 * If `env` or any enclosing environment is a `CallObject` associated with a
 * generator object, return the generator.
 *
 * Otherwise `env` is not in a generator or async function, or the generator
 * object hasn't been created yet; return nullptr with no pending exception.
 */
AbstractGeneratorObject* GetGeneratorObjectForEnvironment(JSContext* cx,
                                                          HandleObject env);

GeneratorResumeKind ParserAtomToResumeKind(
    frontend::TaggedParserAtomIndex atom);
JSAtom* ResumeKindToAtom(JSContext* cx, GeneratorResumeKind kind);

}  // namespace js

template <>
bool JSObject::is<js::AbstractGeneratorObject>() const;

#endif /* vm_GeneratorObject_h */
