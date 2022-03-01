/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SavedStacks.h"

#include "mozilla/Attributes.h"
#include "mozilla/DebugOnly.h"

#include <algorithm>
#include <math.h>
#include <utility>

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsmath.h"
#include "jsnum.h"

#include "gc/FreeOp.h"
#include "gc/HashUtil.h"
#include "gc/Marking.h"
#include "gc/Policy.h"
#include "gc/Rooting.h"
#include "js/CharacterEncoding.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/SavedFrameAPI.h"
#include "js/Vector.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuffer.h"
#include "vm/GeckoProfiler.h"
#include "vm/JSScript.h"
#include "vm/Realm.h"
#include "vm/SavedFrame.h"
#include "vm/Time.h"
#include "vm/WrapperObject.h"

#include "debugger/DebugAPI-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/Stack-inl.h"

using mozilla::AddToHash;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

namespace js {

/**
 * Maximum number of saved frames returned for an async stack.
 */
const uint32_t ASYNC_STACK_MAX_FRAME_COUNT = 60;

void LiveSavedFrameCache::trace(JSTracer* trc) {
  if (!initialized()) {
    return;
  }

  for (auto* entry = frames->begin(); entry < frames->end(); entry++) {
    TraceEdge(trc, &entry->savedFrame,
              "LiveSavedFrameCache::frames SavedFrame");
  }
}

bool LiveSavedFrameCache::insert(JSContext* cx, FramePtr&& framePtr,
                                 const jsbytecode* pc,
                                 HandleSavedFrame savedFrame) {
  MOZ_ASSERT(savedFrame);
  MOZ_ASSERT(initialized());

#ifdef DEBUG
  // There should not already be an entry for this frame. Checking the full
  // stack really slows down some tests, so just check the first and last five
  // hundred.
  size_t limit = std::min(frames->length() / 2, size_t(500));
  for (size_t i = 0; i < limit; i++) {
    MOZ_ASSERT(Key(framePtr) != (*frames)[i].key);
    MOZ_ASSERT(Key(framePtr) != (*frames)[frames->length() - 1 - i].key);
  }
#endif

  if (!frames->emplaceBack(framePtr, pc, savedFrame)) {
    ReportOutOfMemory(cx);
    return false;
  }

  framePtr.setHasCachedSavedFrame();

  return true;
}

void LiveSavedFrameCache::find(JSContext* cx, FramePtr& framePtr,
                               const jsbytecode* pc,
                               MutableHandleSavedFrame frame) const {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(framePtr.hasCachedSavedFrame());

  // The assertions here check that either 1) frames' hasCachedSavedFrame flags
  // accurately indicate the presence of a cache entry for that frame (ignoring
  // pc mismatches), or 2) the cache is completely empty, having been flushed
  // for a realm mismatch.

  // If we flushed the cache due to a realm mismatch, then we shouldn't
  // expect to find any frames in the cache.
  if (frames->empty()) {
    frame.set(nullptr);
    return;
  }

  // All our SavedFrames should be in the same realm. If the last
  // entry's SavedFrame's realm doesn't match cx's, flush the cache.
  if (frames->back().savedFrame->realm() != cx->realm()) {
#ifdef DEBUG
    // Check that they are, indeed, all in the same realm.
    auto realm = frames->back().savedFrame->realm();
    for (const auto& f : (*frames)) {
      MOZ_ASSERT(realm == f.savedFrame->realm());
    }
#endif
    frames->clear();
    frame.set(nullptr);
    return;
  }

  Key key(framePtr);
  while (key != frames->back().key) {
    MOZ_ASSERT(frames->back().savedFrame->realm() == cx->realm());

    // framePtr must have an entry, but apparently it's below this one on the
    // stack; frames->back() must correspond to a frame younger than framePtr's.
    // SavedStacks::insertFrames is going to push new cache entries for
    // everything younger than framePtr, so this entry should be popped.
    frames->popBack();

    // If the frame's bit was set, the frame should always have an entry in
    // the cache. (If we purged the entire cache because its SavedFrames had
    // been captured for a different realm, then we would have
    // returned early above.)
    MOZ_RELEASE_ASSERT(!frames->empty());
  }

  // The youngest valid frame may have run some code, so its current pc may
  // not match its cache entry's pc. In this case, just treat it as a miss. No
  // older frame has executed any code; it would have been necessary to pop
  // this frame for that to happen, but this frame's bit is set.
  if (pc != frames->back().pc) {
    frames->popBack();
    frame.set(nullptr);
    return;
  }

  frame.set(frames->back().savedFrame);
}

void LiveSavedFrameCache::findWithoutInvalidation(
    const FramePtr& framePtr, MutableHandleSavedFrame frame) const {
  MOZ_ASSERT(initialized());
  MOZ_ASSERT(framePtr.hasCachedSavedFrame());

  Key key(framePtr);
  for (auto& entry : (*frames)) {
    if (entry.key == key) {
      frame.set(entry.savedFrame);
      return;
    }
  }

  frame.set(nullptr);
}

struct MOZ_STACK_CLASS SavedFrame::Lookup {
  Lookup(JSAtom* source, uint32_t sourceId, uint32_t line, uint32_t column,
         JSAtom* functionDisplayName, JSAtom* asyncCause, SavedFrame* parent,
         JSPrincipals* principals, bool mutedErrors,
         const Maybe<LiveSavedFrameCache::FramePtr>& framePtr = Nothing(),
         jsbytecode* pc = nullptr, Activation* activation = nullptr)
      : source(source),
        sourceId(sourceId),
        line(line),
        column(column),
        functionDisplayName(functionDisplayName),
        asyncCause(asyncCause),
        parent(parent),
        principals(principals),
        mutedErrors(mutedErrors),
        framePtr(framePtr),
        pc(pc),
        activation(activation) {
    MOZ_ASSERT(source);
    MOZ_ASSERT_IF(framePtr.isSome(), activation);
    if (js::SupportDifferentialTesting()) {
      column = 0;
    }
  }

  explicit Lookup(SavedFrame& savedFrame)
      : source(savedFrame.getSource()),
        sourceId(savedFrame.getSourceId()),
        line(savedFrame.getLine()),
        column(savedFrame.getColumn()),
        functionDisplayName(savedFrame.getFunctionDisplayName()),
        asyncCause(savedFrame.getAsyncCause()),
        parent(savedFrame.getParent()),
        principals(savedFrame.getPrincipals()),
        mutedErrors(savedFrame.getMutedErrors()),
        framePtr(Nothing()),
        pc(nullptr),
        activation(nullptr) {
    MOZ_ASSERT(source);
  }

  JSAtom* source;
  uint32_t sourceId;
  uint32_t line;
  uint32_t column;
  JSAtom* functionDisplayName;
  JSAtom* asyncCause;
  SavedFrame* parent;
  JSPrincipals* principals;
  bool mutedErrors;

  // These are used only by the LiveSavedFrameCache and not used for identity or
  // hashing.
  Maybe<LiveSavedFrameCache::FramePtr> framePtr;
  jsbytecode* pc;
  Activation* activation;

  void trace(JSTracer* trc) {
    TraceRoot(trc, &source, "SavedFrame::Lookup::source");
    TraceNullableRoot(trc, &functionDisplayName,
                      "SavedFrame::Lookup::functionDisplayName");
    TraceNullableRoot(trc, &asyncCause, "SavedFrame::Lookup::asyncCause");
    TraceNullableRoot(trc, &parent, "SavedFrame::Lookup::parent");
  }
};

using GCLookupVector =
    GCVector<SavedFrame::Lookup, ASYNC_STACK_MAX_FRAME_COUNT>;

template <class Wrapper>
class WrappedPtrOperations<SavedFrame::Lookup, Wrapper> {
  const SavedFrame::Lookup& value() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  JSAtom* source() { return value().source; }
  uint32_t sourceId() { return value().sourceId; }
  uint32_t line() { return value().line; }
  uint32_t column() { return value().column; }
  JSAtom* functionDisplayName() { return value().functionDisplayName; }
  JSAtom* asyncCause() { return value().asyncCause; }
  SavedFrame* parent() { return value().parent; }
  JSPrincipals* principals() { return value().principals; }
  bool mutedErrors() { return value().mutedErrors; }
  Maybe<LiveSavedFrameCache::FramePtr> framePtr() { return value().framePtr; }
  jsbytecode* pc() { return value().pc; }
  Activation* activation() { return value().activation; }
};

template <typename Wrapper>
class MutableWrappedPtrOperations<SavedFrame::Lookup, Wrapper>
    : public WrappedPtrOperations<SavedFrame::Lookup, Wrapper> {
  SavedFrame::Lookup& value() { return static_cast<Wrapper*>(this)->get(); }

 public:
  void setParent(SavedFrame* parent) { value().parent = parent; }

  void setAsyncCause(HandleAtom asyncCause) { value().asyncCause = asyncCause; }
};

/* static */
bool SavedFrame::HashPolicy::hasHash(const Lookup& l) {
  return SavedFramePtrHasher::hasHash(l.parent);
}

/* static */
bool SavedFrame::HashPolicy::ensureHash(const Lookup& l) {
  return SavedFramePtrHasher::ensureHash(l.parent);
}

/* static */
HashNumber SavedFrame::HashPolicy::hash(const Lookup& lookup) {
  JS::AutoCheckCannotGC nogc;
  // Assume that we can take line mod 2^32 without losing anything of
  // interest.  If that assumption changes, we'll just need to start with 0
  // and add another overload of AddToHash with more arguments.
  return AddToHash(lookup.line, lookup.column, lookup.source,
                   lookup.functionDisplayName, lookup.asyncCause,
                   lookup.mutedErrors, SavedFramePtrHasher::hash(lookup.parent),
                   JSPrincipalsPtrHasher::hash(lookup.principals));
}

/* static */
bool SavedFrame::HashPolicy::match(SavedFrame* existing, const Lookup& lookup) {
  MOZ_ASSERT(existing);

  if (existing->getLine() != lookup.line) {
    return false;
  }

  if (existing->getColumn() != lookup.column) {
    return false;
  }

  if (existing->getParent() != lookup.parent) {
    return false;
  }

  if (existing->getPrincipals() != lookup.principals) {
    return false;
  }

  JSAtom* source = existing->getSource();
  if (source != lookup.source) {
    return false;
  }

  JSAtom* functionDisplayName = existing->getFunctionDisplayName();
  if (functionDisplayName != lookup.functionDisplayName) {
    return false;
  }

  JSAtom* asyncCause = existing->getAsyncCause();
  if (asyncCause != lookup.asyncCause) {
    return false;
  }

  return true;
}

/* static */
void SavedFrame::HashPolicy::rekey(Key& key, const Key& newKey) {
  key = newKey;
}

/* static */
bool SavedFrame::finishSavedFrameInit(JSContext* cx, HandleObject ctor,
                                      HandleObject proto) {
  return FreezeObject(cx, proto);
}

static const JSClassOps SavedFrameClassOps = {
    nullptr,               // addProperty
    nullptr,               // delProperty
    nullptr,               // enumerate
    nullptr,               // newEnumerate
    nullptr,               // resolve
    nullptr,               // mayResolve
    SavedFrame::finalize,  // finalize
    nullptr,               // call
    nullptr,               // hasInstance
    nullptr,               // construct
    nullptr,               // trace
};

const ClassSpec SavedFrame::classSpec_ = {
    GenericCreateConstructor<SavedFrame::construct, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<SavedFrame>,
    SavedFrame::staticFunctions,
    nullptr,
    SavedFrame::protoFunctions,
    SavedFrame::protoAccessors,
    SavedFrame::finishSavedFrameInit,
    ClassSpec::DontDefineConstructor};

/* static */ const JSClass SavedFrame::class_ = {
    "SavedFrame",
    JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(SavedFrame::JSSLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_SavedFrame) |
        JSCLASS_FOREGROUND_FINALIZE,
    &SavedFrameClassOps, &SavedFrame::classSpec_};

const JSClass SavedFrame::protoClass_ = {
    "SavedFrame.prototype", JSCLASS_HAS_CACHED_PROTO(JSProto_SavedFrame),
    JS_NULL_CLASS_OPS, &SavedFrame::classSpec_};

/* static */ const JSFunctionSpec SavedFrame::staticFunctions[] = {JS_FS_END};

/* static */ const JSFunctionSpec SavedFrame::protoFunctions[] = {
    JS_FN("constructor", SavedFrame::construct, 0, 0),
    JS_FN("toString", SavedFrame::toStringMethod, 0, 0), JS_FS_END};

/* static */ const JSPropertySpec SavedFrame::protoAccessors[] = {
    JS_PSG("source", SavedFrame::sourceProperty, 0),
    JS_PSG("sourceId", SavedFrame::sourceIdProperty, 0),
    JS_PSG("line", SavedFrame::lineProperty, 0),
    JS_PSG("column", SavedFrame::columnProperty, 0),
    JS_PSG("functionDisplayName", SavedFrame::functionDisplayNameProperty, 0),
    JS_PSG("asyncCause", SavedFrame::asyncCauseProperty, 0),
    JS_PSG("asyncParent", SavedFrame::asyncParentProperty, 0),
    JS_PSG("parent", SavedFrame::parentProperty, 0),
    JS_STRING_SYM_PS(toStringTag, "SavedFrame", JSPROP_READONLY),
    JS_PS_END};

/* static */
void SavedFrame::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());
  JSPrincipals* p = obj->as<SavedFrame>().getPrincipals();
  if (p) {
    JSRuntime* rt = obj->runtimeFromMainThread();
    JS_DropPrincipals(rt->mainContextFromOwnThread(), p);
  }
}

JSAtom* SavedFrame::getSource() {
  const Value& v = getReservedSlot(JSSLOT_SOURCE);
  JSString* s = v.toString();
  return &s->asAtom();
}

uint32_t SavedFrame::getSourceId() {
  const Value& v = getReservedSlot(JSSLOT_SOURCEID);
  return v.toPrivateUint32();
}

uint32_t SavedFrame::getLine() {
  const Value& v = getReservedSlot(JSSLOT_LINE);
  return v.toPrivateUint32();
}

uint32_t SavedFrame::getColumn() {
  const Value& v = getReservedSlot(JSSLOT_COLUMN);
  return v.toPrivateUint32();
}

JSAtom* SavedFrame::getFunctionDisplayName() {
  const Value& v = getReservedSlot(JSSLOT_FUNCTIONDISPLAYNAME);
  if (v.isNull()) {
    return nullptr;
  }
  JSString* s = v.toString();
  return &s->asAtom();
}

JSAtom* SavedFrame::getAsyncCause() {
  const Value& v = getReservedSlot(JSSLOT_ASYNCCAUSE);
  if (v.isNull()) {
    return nullptr;
  }
  JSString* s = v.toString();
  return &s->asAtom();
}

SavedFrame* SavedFrame::getParent() const {
  const Value& v = getReservedSlot(JSSLOT_PARENT);
  return v.isObject() ? &v.toObject().as<SavedFrame>() : nullptr;
}

JSPrincipals* SavedFrame::getPrincipals() {
  const Value& v = getReservedSlot(JSSLOT_PRINCIPALS);
  if (v.isUndefined()) {
    return nullptr;
  }
  return reinterpret_cast<JSPrincipals*>(uintptr_t(v.toPrivate()) & ~0b1);
}

bool SavedFrame::getMutedErrors() {
  const Value& v = getReservedSlot(JSSLOT_PRINCIPALS);
  if (v.isUndefined()) {
    return true;
  }
  return bool(uintptr_t(v.toPrivate()) & 0b1);
}

void SavedFrame::initSource(JSAtom* source) {
  MOZ_ASSERT(source);
  initReservedSlot(JSSLOT_SOURCE, StringValue(source));
}

void SavedFrame::initSourceId(uint32_t sourceId) {
  initReservedSlot(JSSLOT_SOURCEID, PrivateUint32Value(sourceId));
}

void SavedFrame::initLine(uint32_t line) {
  initReservedSlot(JSSLOT_LINE, PrivateUint32Value(line));
}

void SavedFrame::initColumn(uint32_t column) {
  if (js::SupportDifferentialTesting()) {
    column = 0;
  }
  initReservedSlot(JSSLOT_COLUMN, PrivateUint32Value(column));
}

void SavedFrame::initPrincipalsAndMutedErrors(JSPrincipals* principals,
                                              bool mutedErrors) {
  if (principals) {
    JS_HoldPrincipals(principals);
  }
  initPrincipalsAlreadyHeldAndMutedErrors(principals, mutedErrors);
}

void SavedFrame::initPrincipalsAlreadyHeldAndMutedErrors(
    JSPrincipals* principals, bool mutedErrors) {
  MOZ_ASSERT_IF(principals, principals->refcount > 0);
  uintptr_t ptr = uintptr_t(principals) | mutedErrors;
  initReservedSlot(JSSLOT_PRINCIPALS,
                   PrivateValue(reinterpret_cast<void*>(ptr)));
}

void SavedFrame::initFunctionDisplayName(JSAtom* maybeName) {
  initReservedSlot(JSSLOT_FUNCTIONDISPLAYNAME,
                   maybeName ? StringValue(maybeName) : NullValue());
}

void SavedFrame::initAsyncCause(JSAtom* maybeCause) {
  initReservedSlot(JSSLOT_ASYNCCAUSE,
                   maybeCause ? StringValue(maybeCause) : NullValue());
}

void SavedFrame::initParent(SavedFrame* maybeParent) {
  initReservedSlot(JSSLOT_PARENT, ObjectOrNullValue(maybeParent));
}

void SavedFrame::initFromLookup(JSContext* cx, Handle<Lookup> lookup) {
  // Make sure any atoms used in the lookup are marked in the current zone.
  // Normally we would try to keep these mark bits up to date around the
  // points where the context moves between compartments, but Lookups live on
  // the stack (where the atoms are kept alive regardless) and this is a
  // more convenient pinchpoint.
  if (lookup.source()) {
    cx->markAtom(lookup.source());
  }
  if (lookup.functionDisplayName()) {
    cx->markAtom(lookup.functionDisplayName());
  }
  if (lookup.asyncCause()) {
    cx->markAtom(lookup.asyncCause());
  }

  initSource(lookup.source());
  initSourceId(lookup.sourceId());
  initLine(lookup.line());
  initColumn(lookup.column());
  initFunctionDisplayName(lookup.functionDisplayName());
  initAsyncCause(lookup.asyncCause());
  initParent(lookup.parent());
  initPrincipalsAndMutedErrors(lookup.principals(), lookup.mutedErrors());
}

/* static */
SavedFrame* SavedFrame::create(JSContext* cx) {
  RootedGlobalObject global(cx, cx->global());
  cx->check(global);

  // Ensure that we don't try to capture the stack again in the
  // `SavedStacksMetadataBuilder` for this new SavedFrame object, and
  // accidentally cause O(n^2) behavior.
  SavedStacks::AutoReentrancyGuard guard(cx->realm()->savedStacks());

  RootedObject proto(cx,
                     GlobalObject::getOrCreateSavedFramePrototype(cx, global));
  if (!proto) {
    return nullptr;
  }
  cx->check(proto);

  return NewTenuredObjectWithGivenProto<SavedFrame>(cx, proto);
}

bool SavedFrame::isSelfHosted(JSContext* cx) {
  JSAtom* source = getSource();
  return source == cx->names().selfHosted;
}

bool SavedFrame::isWasm() {
  // See WasmFrameIter::computeLine() comment.
  return bool(getColumn() & wasm::WasmFrameIter::ColumnBit);
}

uint32_t SavedFrame::wasmFuncIndex() {
  // See WasmFrameIter::computeLine() comment.
  MOZ_ASSERT(isWasm());
  return getColumn() & ~wasm::WasmFrameIter::ColumnBit;
}

uint32_t SavedFrame::wasmBytecodeOffset() {
  // See WasmFrameIter::computeLine() comment.
  MOZ_ASSERT(isWasm());
  return getLine();
}

/* static */
bool SavedFrame::construct(JSContext* cx, unsigned argc, Value* vp) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_NO_CONSTRUCTOR,
                            "SavedFrame");
  return false;
}

static bool SavedFrameSubsumedByPrincipals(JSContext* cx,
                                           JSPrincipals* principals,
                                           HandleSavedFrame frame) {
  auto subsumes = cx->runtime()->securityCallbacks->subsumes;
  if (!subsumes) {
    return true;
  }

  MOZ_ASSERT(!ReconstructedSavedFramePrincipals::is(principals));

  auto framePrincipals = frame->getPrincipals();

  // Handle SavedFrames that have been reconstructed from stacks in a heap
  // snapshot.
  if (framePrincipals == &ReconstructedSavedFramePrincipals::IsSystem) {
    return cx->runningWithTrustedPrincipals();
  }
  if (framePrincipals == &ReconstructedSavedFramePrincipals::IsNotSystem) {
    return true;
  }

  return subsumes(principals, framePrincipals);
}

// Return the first SavedFrame in the chain that starts with |frame| whose
// for which the given match function returns true. If there is no such frame,
// return nullptr. |skippedAsync| is set to true if any of the skipped frames
// had the |asyncCause| property set, otherwise it is explicitly set to false.
template <typename Matcher>
static SavedFrame* GetFirstMatchedFrame(JSContext* cx, JSPrincipals* principals,
                                        Matcher& matches,
                                        HandleSavedFrame frame,
                                        JS::SavedFrameSelfHosted selfHosted,
                                        bool& skippedAsync) {
  skippedAsync = false;

  RootedSavedFrame rootedFrame(cx, frame);
  while (rootedFrame) {
    if ((selfHosted == JS::SavedFrameSelfHosted::Include ||
         !rootedFrame->isSelfHosted(cx)) &&
        matches(cx, principals, rootedFrame)) {
      return rootedFrame;
    }

    if (rootedFrame->getAsyncCause()) {
      skippedAsync = true;
    }

    rootedFrame = rootedFrame->getParent();
  }

  return nullptr;
}

// Return the first SavedFrame in the chain that starts with |frame| whose
// principals are subsumed by |principals|, according to |subsumes|. If there is
// no such frame, return nullptr. |skippedAsync| is set to true if any of the
// skipped frames had the |asyncCause| property set, otherwise it is explicitly
// set to false.
static SavedFrame* GetFirstSubsumedFrame(JSContext* cx,
                                         JSPrincipals* principals,
                                         HandleSavedFrame frame,
                                         JS::SavedFrameSelfHosted selfHosted,
                                         bool& skippedAsync) {
  return GetFirstMatchedFrame(cx, principals, SavedFrameSubsumedByPrincipals,
                              frame, selfHosted, skippedAsync);
}

JS_PUBLIC_API JSObject* GetFirstSubsumedSavedFrame(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    JS::SavedFrameSelfHosted selfHosted) {
  if (!savedFrame) {
    return nullptr;
  }

  auto subsumes = cx->runtime()->securityCallbacks->subsumes;
  if (!subsumes) {
    return nullptr;
  }

  auto matcher = [subsumes](JSContext* cx, JSPrincipals* principals,
                            HandleSavedFrame frame) -> bool {
    return subsumes(principals, frame->getPrincipals());
  };

  bool skippedAsync;
  RootedSavedFrame frame(cx, &savedFrame->as<SavedFrame>());
  return GetFirstMatchedFrame(cx, principals, matcher, frame, selfHosted,
                              skippedAsync);
}

[[nodiscard]] static bool SavedFrame_checkThis(JSContext* cx, CallArgs& args,
                                               const char* fnName,
                                               MutableHandleObject frame) {
  const Value& thisValue = args.thisv();

  if (!thisValue.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_OBJECT_REQUIRED,
                              InformalValueTypeName(thisValue));
    return false;
  }

  if (!thisValue.toObject().canUnwrapAs<SavedFrame>()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, SavedFrame::class_.name,
                              fnName, "object");
    return false;
  }

  // Now set "frame" to the actual object we were invoked in (which may be a
  // wrapper), not the unwrapped version.  Consumers will need to know what
  // that original object was, and will do principal checks as needed.
  frame.set(&thisValue.toObject());
  return true;
}

// Get the SavedFrame * from the current this value and handle any errors that
// might occur therein.
//
// These parameters must already exist when calling this macro:
//   - JSContext* cx
//   - unsigned   argc
//   - Value*     vp
//   - const char* fnName
// These parameters will be defined after calling this macro:
//   - CallArgs args
//   - Rooted<SavedFrame*> frame (will be non-null)
#define THIS_SAVEDFRAME(cx, argc, vp, fnName, args, frame) \
  CallArgs args = CallArgsFromVp(argc, vp);                \
  RootedObject frame(cx);                                  \
  if (!SavedFrame_checkThis(cx, args, fnName, &frame)) return false;

} /* namespace js */

js::SavedFrame* js::UnwrapSavedFrame(JSContext* cx, JSPrincipals* principals,
                                     HandleObject obj,
                                     JS::SavedFrameSelfHosted selfHosted,
                                     bool& skippedAsync) {
  if (!obj) {
    return nullptr;
  }

  RootedSavedFrame frame(cx, obj->maybeUnwrapAs<SavedFrame>());
  if (!frame) {
    return nullptr;
  }

  return GetFirstSubsumedFrame(cx, principals, frame, selfHosted, skippedAsync);
}

namespace JS {

JS_PUBLIC_API SavedFrameResult GetSavedFrameSource(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    MutableHandleString sourcep,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  {
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                    selfHosted, skippedAsync));
    if (!frame) {
      sourcep.set(cx->runtime()->emptyString);
      return SavedFrameResult::AccessDenied;
    }
    sourcep.set(frame->getSource());
  }
  if (sourcep->isAtom()) {
    cx->markAtom(&sourcep->asAtom());
  }
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameSourceId(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    uint32_t* sourceIdp,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  bool skippedAsync;
  js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                  selfHosted, skippedAsync));
  if (!frame) {
    *sourceIdp = 0;
    return SavedFrameResult::AccessDenied;
  }
  *sourceIdp = frame->getSourceId();
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameLine(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    uint32_t* linep,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());
  MOZ_ASSERT(linep);

  bool skippedAsync;
  js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                  selfHosted, skippedAsync));
  if (!frame) {
    *linep = 0;
    return SavedFrameResult::AccessDenied;
  }
  *linep = frame->getLine();
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameColumn(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    uint32_t* columnp,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());
  MOZ_ASSERT(columnp);

  bool skippedAsync;
  js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                  selfHosted, skippedAsync));
  if (!frame) {
    *columnp = 0;
    return SavedFrameResult::AccessDenied;
  }
  *columnp = frame->getColumn();
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameFunctionDisplayName(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    MutableHandleString namep,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  {
    bool skippedAsync;
    js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                    selfHosted, skippedAsync));
    if (!frame) {
      namep.set(nullptr);
      return SavedFrameResult::AccessDenied;
    }
    namep.set(frame->getFunctionDisplayName());
  }
  if (namep && namep->isAtom()) {
    cx->markAtom(&namep->asAtom());
  }
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameAsyncCause(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    MutableHandleString asyncCausep,
    SavedFrameSelfHosted unused_ /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  {
    bool skippedAsync;
    // This function is always called with self-hosted frames excluded by
    // GetValueIfNotCached in dom/bindings/Exceptions.cpp. However, we want
    // to include them because our Promise implementation causes us to have
    // the async cause on a self-hosted frame. So we just ignore the
    // parameter and always include self-hosted frames.
    js::RootedSavedFrame frame(
        cx, UnwrapSavedFrame(cx, principals, savedFrame,
                             SavedFrameSelfHosted::Include, skippedAsync));
    if (!frame) {
      asyncCausep.set(nullptr);
      return SavedFrameResult::AccessDenied;
    }
    asyncCausep.set(frame->getAsyncCause());
    if (!asyncCausep && skippedAsync) {
      asyncCausep.set(cx->names().Async);
    }
  }
  if (asyncCausep && asyncCausep->isAtom()) {
    cx->markAtom(&asyncCausep->asAtom());
  }
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameAsyncParent(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    MutableHandleObject asyncParentp,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  bool skippedAsync;
  js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                  selfHosted, skippedAsync));
  if (!frame) {
    asyncParentp.set(nullptr);
    return SavedFrameResult::AccessDenied;
  }
  js::RootedSavedFrame parent(cx, frame->getParent());

  // The current value of |skippedAsync| is not interesting, because we are
  // interested in whether we would cross any async parents to get from here
  // to the first subsumed parent frame instead.
  js::RootedSavedFrame subsumedParent(
      cx,
      GetFirstSubsumedFrame(cx, principals, parent, selfHosted, skippedAsync));

  // Even if |parent| is not subsumed, we still want to return a pointer to it
  // rather than |subsumedParent| so it can pick up any |asyncCause| from the
  // inaccessible part of the chain.
  if (subsumedParent && (subsumedParent->getAsyncCause() || skippedAsync)) {
    asyncParentp.set(parent);
  } else {
    asyncParentp.set(nullptr);
  }
  return SavedFrameResult::Ok;
}

JS_PUBLIC_API SavedFrameResult GetSavedFrameParent(
    JSContext* cx, JSPrincipals* principals, HandleObject savedFrame,
    MutableHandleObject parentp,
    SavedFrameSelfHosted selfHosted /* = SavedFrameSelfHosted::Include */) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  bool skippedAsync;
  js::RootedSavedFrame frame(cx, UnwrapSavedFrame(cx, principals, savedFrame,
                                                  selfHosted, skippedAsync));
  if (!frame) {
    parentp.set(nullptr);
    return SavedFrameResult::AccessDenied;
  }
  js::RootedSavedFrame parent(cx, frame->getParent());

  // The current value of |skippedAsync| is not interesting, because we are
  // interested in whether we would cross any async parents to get from here
  // to the first subsumed parent frame instead.
  js::RootedSavedFrame subsumedParent(
      cx,
      GetFirstSubsumedFrame(cx, principals, parent, selfHosted, skippedAsync));

  // Even if |parent| is not subsumed, we still want to return a pointer to it
  // rather than |subsumedParent| so it can pick up any |asyncCause| from the
  // inaccessible part of the chain.
  if (subsumedParent && !(subsumedParent->getAsyncCause() || skippedAsync)) {
    parentp.set(parent);
  } else {
    parentp.set(nullptr);
  }
  return SavedFrameResult::Ok;
}

static bool FormatStackFrameLine(JSContext* cx, js::StringBuffer& sb,
                                 js::HandleSavedFrame frame) {
  if (frame->isWasm()) {
    // See comment in WasmFrameIter::computeLine().
    return sb.append("wasm-function[") &&
           NumberValueToStringBuffer(cx, NumberValue(frame->wasmFuncIndex()),
                                     sb) &&
           sb.append(']');
  }

  return NumberValueToStringBuffer(cx, NumberValue(frame->getLine()), sb);
}

static bool FormatStackFrameColumn(JSContext* cx, js::StringBuffer& sb,
                                   js::HandleSavedFrame frame) {
  if (frame->isWasm()) {
    // See comment in WasmFrameIter::computeLine().
    js::ToCStringBuf cbuf;
    const char* cstr =
        NumberToCString(cx, &cbuf, frame->wasmBytecodeOffset(), 16);
    if (!cstr) {
      return false;
    }

    return sb.append("0x") && sb.append(cstr, strlen(cstr));
  }

  return NumberValueToStringBuffer(cx, NumberValue(frame->getColumn()), sb);
}

static bool FormatSpiderMonkeyStackFrame(JSContext* cx, js::StringBuffer& sb,
                                         js::HandleSavedFrame frame,
                                         size_t indent, bool skippedAsync) {
  RootedString asyncCause(cx, frame->getAsyncCause());
  if (!asyncCause && skippedAsync) {
    asyncCause.set(cx->names().Async);
  }

  js::RootedAtom name(cx, frame->getFunctionDisplayName());
  return (!indent || sb.appendN(' ', indent)) &&
         (!asyncCause || (sb.append(asyncCause) && sb.append('*'))) &&
         (!name || sb.append(name)) && sb.append('@') &&
         sb.append(frame->getSource()) && sb.append(':') &&
         FormatStackFrameLine(cx, sb, frame) && sb.append(':') &&
         FormatStackFrameColumn(cx, sb, frame) && sb.append('\n');
}

static bool FormatV8StackFrame(JSContext* cx, js::StringBuffer& sb,
                               js::HandleSavedFrame frame, size_t indent,
                               bool lastFrame) {
  js::RootedAtom name(cx, frame->getFunctionDisplayName());
  return sb.appendN(' ', indent + 4) && sb.append('a') && sb.append('t') &&
         sb.append(' ') &&
         (!name || (sb.append(name) && sb.append(' ') && sb.append('('))) &&
         sb.append(frame->getSource()) && sb.append(':') &&
         FormatStackFrameLine(cx, sb, frame) && sb.append(':') &&
         FormatStackFrameColumn(cx, sb, frame) && (!name || sb.append(')')) &&
         (lastFrame || sb.append('\n'));
}

JS_PUBLIC_API bool BuildStackString(JSContext* cx, JSPrincipals* principals,
                                    HandleObject stack,
                                    MutableHandleString stringp, size_t indent,
                                    js::StackFormat format) {
  js::AssertHeapIsIdle();
  CHECK_THREAD(cx);
  MOZ_RELEASE_ASSERT(cx->realm());

  js::JSStringBuilder sb(cx);

  if (format == js::StackFormat::Default) {
    format = cx->runtime()->stackFormat();
  }
  MOZ_ASSERT(format != js::StackFormat::Default);

  // Enter a new block to constrain the scope of possibly entering the stack's
  // realm. This ensures that when we finish the StringBuffer, we are back in
  // the cx's original compartment, and fulfill our contract with callers to
  // place the output string in the cx's current realm.
  {
    bool skippedAsync;
    js::RootedSavedFrame frame(
        cx, UnwrapSavedFrame(cx, principals, stack,
                             SavedFrameSelfHosted::Exclude, skippedAsync));
    if (!frame) {
      stringp.set(cx->runtime()->emptyString);
      return true;
    }

    js::RootedSavedFrame parent(cx);
    do {
      MOZ_ASSERT(SavedFrameSubsumedByPrincipals(cx, principals, frame));
      MOZ_ASSERT(!frame->isSelfHosted(cx));

      parent = frame->getParent();
      bool skippedNextAsync;
      js::RootedSavedFrame nextFrame(
          cx, js::GetFirstSubsumedFrame(cx, principals, parent,
                                        SavedFrameSelfHosted::Exclude,
                                        skippedNextAsync));

      switch (format) {
        case js::StackFormat::SpiderMonkey:
          if (!FormatSpiderMonkeyStackFrame(cx, sb, frame, indent,
                                            skippedAsync)) {
            return false;
          }
          break;
        case js::StackFormat::V8:
          if (!FormatV8StackFrame(cx, sb, frame, indent, !nextFrame)) {
            return false;
          }
          break;
        case js::StackFormat::Default:
          MOZ_CRASH("Unexpected value");
          break;
      }

      frame = nextFrame;
      skippedAsync = skippedNextAsync;
    } while (frame);
  }

  JSString* str = sb.finishString();
  if (!str) {
    return false;
  }
  cx->check(str);
  stringp.set(str);
  return true;
}

JS_PUBLIC_API bool IsMaybeWrappedSavedFrame(JSObject* obj) {
  MOZ_ASSERT(obj);
  return obj->canUnwrapAs<js::SavedFrame>();
}

JS_PUBLIC_API bool IsUnwrappedSavedFrame(JSObject* obj) {
  MOZ_ASSERT(obj);
  return obj->is<js::SavedFrame>();
}

static bool AssignProperty(JSContext* cx, HandleObject dst, HandleObject src,
                           const char* property) {
  RootedValue v(cx);
  return JS_GetProperty(cx, src, property, &v) &&
         JS_DefineProperty(cx, dst, property, v, JSPROP_ENUMERATE);
}

JS_PUBLIC_API JSObject* ConvertSavedFrameToPlainObject(
    JSContext* cx, HandleObject savedFrameArg,
    SavedFrameSelfHosted selfHosted) {
  MOZ_ASSERT(savedFrameArg);

  RootedObject savedFrame(cx, savedFrameArg);
  RootedObject baseConverted(cx), lastConverted(cx);
  RootedValue v(cx);

  baseConverted = lastConverted = JS_NewObject(cx, nullptr);
  if (!baseConverted) {
    return nullptr;
  }

  bool foundParent;
  do {
    if (!AssignProperty(cx, lastConverted, savedFrame, "source") ||
        !AssignProperty(cx, lastConverted, savedFrame, "sourceId") ||
        !AssignProperty(cx, lastConverted, savedFrame, "line") ||
        !AssignProperty(cx, lastConverted, savedFrame, "column") ||
        !AssignProperty(cx, lastConverted, savedFrame, "functionDisplayName") ||
        !AssignProperty(cx, lastConverted, savedFrame, "asyncCause")) {
      return nullptr;
    }

    const char* parentProperties[] = {"parent", "asyncParent"};
    foundParent = false;
    for (const char* prop : parentProperties) {
      if (!JS_GetProperty(cx, savedFrame, prop, &v)) {
        return nullptr;
      }
      if (v.isObject()) {
        RootedObject nextConverted(cx, JS_NewObject(cx, nullptr));
        if (!nextConverted ||
            !JS_DefineProperty(cx, lastConverted, prop, nextConverted,
                               JSPROP_ENUMERATE)) {
          return nullptr;
        }
        lastConverted = nextConverted;
        savedFrame = &v.toObject();
        foundParent = true;
        break;
      }
    }
  } while (foundParent);

  return baseConverted;
}

} /* namespace JS */

namespace js {

/* static */
bool SavedFrame::sourceProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get source)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  RootedString source(cx);
  if (JS::GetSavedFrameSource(cx, principals, frame, &source) ==
      JS::SavedFrameResult::Ok) {
    if (!cx->compartment()->wrap(cx, &source)) {
      return false;
    }
    args.rval().setString(source);
  } else {
    args.rval().setNull();
  }
  return true;
}

/* static */
bool SavedFrame::sourceIdProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get sourceId)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  uint32_t sourceId;
  if (JS::GetSavedFrameSourceId(cx, principals, frame, &sourceId) ==
      JS::SavedFrameResult::Ok) {
    args.rval().setNumber(sourceId);
  } else {
    args.rval().setNull();
  }
  return true;
}

/* static */
bool SavedFrame::lineProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get line)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  uint32_t line;
  if (JS::GetSavedFrameLine(cx, principals, frame, &line) ==
      JS::SavedFrameResult::Ok) {
    args.rval().setNumber(line);
  } else {
    args.rval().setNull();
  }
  return true;
}

/* static */
bool SavedFrame::columnProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get column)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  uint32_t column;
  if (JS::GetSavedFrameColumn(cx, principals, frame, &column) ==
      JS::SavedFrameResult::Ok) {
    args.rval().setNumber(column);
  } else {
    args.rval().setNull();
  }
  return true;
}

/* static */
bool SavedFrame::functionDisplayNameProperty(JSContext* cx, unsigned argc,
                                             Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get functionDisplayName)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  RootedString name(cx);
  JS::SavedFrameResult result =
      JS::GetSavedFrameFunctionDisplayName(cx, principals, frame, &name);
  if (result == JS::SavedFrameResult::Ok && name) {
    if (!cx->compartment()->wrap(cx, &name)) {
      return false;
    }
    args.rval().setString(name);
  } else {
    args.rval().setNull();
  }
  return true;
}

/* static */
bool SavedFrame::asyncCauseProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get asyncCause)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  RootedString asyncCause(cx);
  JS::SavedFrameResult result =
      JS::GetSavedFrameAsyncCause(cx, principals, frame, &asyncCause);
  if (result == JS::SavedFrameResult::Ok && asyncCause) {
    if (!cx->compartment()->wrap(cx, &asyncCause)) {
      return false;
    }
    args.rval().setString(asyncCause);
  } else {
    args.rval().setNull();
  }
  return true;
}

/* static */
bool SavedFrame::asyncParentProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get asyncParent)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  RootedObject asyncParent(cx);
  (void)JS::GetSavedFrameAsyncParent(cx, principals, frame, &asyncParent);
  if (!cx->compartment()->wrap(cx, &asyncParent)) {
    return false;
  }
  args.rval().setObjectOrNull(asyncParent);
  return true;
}

/* static */
bool SavedFrame::parentProperty(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "(get parent)", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  RootedObject parent(cx);
  (void)JS::GetSavedFrameParent(cx, principals, frame, &parent);
  if (!cx->compartment()->wrap(cx, &parent)) {
    return false;
  }
  args.rval().setObjectOrNull(parent);
  return true;
}

/* static */
bool SavedFrame::toStringMethod(JSContext* cx, unsigned argc, Value* vp) {
  THIS_SAVEDFRAME(cx, argc, vp, "toString", args, frame);
  JSPrincipals* principals = cx->realm()->principals();
  RootedString string(cx);
  if (!JS::BuildStackString(cx, principals, frame, &string)) {
    return false;
  }
  args.rval().setString(string);
  return true;
}

bool SavedStacks::saveCurrentStack(
    JSContext* cx, MutableHandleSavedFrame frame,
    JS::StackCapture&& capture /* = JS::StackCapture(JS::AllFrames()) */) {
  MOZ_RELEASE_ASSERT(cx->realm());
  MOZ_DIAGNOSTIC_ASSERT(&cx->realm()->savedStacks() == this);

  if (creatingSavedFrame || cx->isExceptionPending() || !cx->global() ||
      !cx->global()->isStandardClassResolved(JSProto_Object)) {
    frame.set(nullptr);
    return true;
  }

  AutoGeckoProfilerEntry labelFrame(cx, "js::SavedStacks::saveCurrentStack");
  return insertFrames(cx, frame, std::move(capture));
}

bool SavedStacks::copyAsyncStack(JSContext* cx, HandleObject asyncStack,
                                 HandleString asyncCause,
                                 MutableHandleSavedFrame adoptedStack,
                                 const Maybe<size_t>& maxFrameCount) {
  MOZ_RELEASE_ASSERT(cx->realm());
  MOZ_DIAGNOSTIC_ASSERT(&cx->realm()->savedStacks() == this);

  RootedAtom asyncCauseAtom(cx, AtomizeString(cx, asyncCause));
  if (!asyncCauseAtom) {
    return false;
  }

  RootedSavedFrame asyncStackObj(cx,
                                 asyncStack->maybeUnwrapAs<js::SavedFrame>());
  MOZ_RELEASE_ASSERT(asyncStackObj);
  adoptedStack.set(asyncStackObj);

  if (!adoptAsyncStack(cx, adoptedStack, asyncCauseAtom, maxFrameCount)) {
    return false;
  }

  return true;
}

void SavedStacks::traceWeak(JSTracer* trc) {
  frames.traceWeak(trc);
  pcLocationMap.traceWeak(trc);
}

void SavedStacks::trace(JSTracer* trc) { pcLocationMap.trace(trc); }

uint32_t SavedStacks::count() { return frames.count(); }

void SavedStacks::clear() { frames.clear(); }

size_t SavedStacks::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  return frames.shallowSizeOfExcludingThis(mallocSizeOf) +
         pcLocationMap.shallowSizeOfExcludingThis(mallocSizeOf);
}

// Given that we have captured a stack frame with the given principals and
// source, return true if the requested `StackCapture` has been satisfied and
// stack walking can halt. Return false otherwise (and stack walking and frame
// capturing should continue).
static inline bool captureIsSatisfied(JSContext* cx, JSPrincipals* principals,
                                      const JSAtom* source,
                                      JS::StackCapture& capture) {
  class Matcher {
    JSContext* cx_;
    JSPrincipals* framePrincipals_;
    const JSAtom* frameSource_;

   public:
    Matcher(JSContext* cx, JSPrincipals* principals, const JSAtom* source)
        : cx_(cx), framePrincipals_(principals), frameSource_(source) {}

    bool operator()(JS::FirstSubsumedFrame& target) {
      auto subsumes = cx_->runtime()->securityCallbacks->subsumes;
      return (!subsumes || subsumes(target.principals, framePrincipals_)) &&
             (!target.ignoreSelfHosted ||
              frameSource_ != cx_->names().selfHosted);
    }

    bool operator()(JS::MaxFrames& target) { return target.maxFrames == 1; }

    bool operator()(JS::AllFrames&) { return false; }
  };

  Matcher m(cx, principals, source);
  return capture.match(m);
}

bool SavedStacks::insertFrames(JSContext* cx, MutableHandleSavedFrame frame,
                               JS::StackCapture&& capture) {
  // In order to look up a cached SavedFrame object, we need to have its parent
  // SavedFrame, which means we need to walk the stack from oldest frame to
  // youngest. However, FrameIter walks the stack from youngest frame to
  // oldest. The solution is to append stack frames to a vector as we walk the
  // stack with FrameIter, and then do a second pass through that vector in
  // reverse order after the traversal has completed and get or create the
  // SavedFrame objects at that time.
  //
  // To avoid making many copies of FrameIter (whose copy constructor is
  // relatively slow), we use a vector of `SavedFrame::Lookup` objects, which
  // only contain the FrameIter data we need. The `SavedFrame::Lookup`
  // objects are partially initialized with everything except their parent
  // pointers on the first pass, and then we fill in the parent pointers as we
  // return in the second pass.

  // Accumulate the vector of Lookup objects here, youngest to oldest.
  Rooted<js::GCLookupVector> stackChain(cx, js::GCLookupVector(cx));

  // If we find a cached saved frame, then that supplies the parent of the
  // frames we have placed in stackChain. If we walk the stack all the way
  // to the end, this remains null.
  RootedSavedFrame cachedParentFrame(cx, nullptr);

  // Choose the right frame iteration strategy to accomodate both
  // evalInFramePrev links and the LiveSavedFrameCache. For background, see
  // the LiveSavedFrameCache comments in Stack.h.
  //
  // If we're using the LiveSavedFrameCache, then don't handle evalInFramePrev
  // links by skipping over the frames altogether; that violates the cache's
  // assumptions. Instead, traverse the entire stack, but choose each
  // SavedFrame's parent as directed by the evalInFramePrev link, if any.
  //
  // If we're not using the LiveSavedFrameCache, it's hard to recover the
  // frame to which the evalInFramePrev link refers, so we just let FrameIter
  // skip those frames. Then each SavedFrame's parent is simply the frame that
  // follows it in the stackChain vector, even when it has an evalInFramePrev
  // link.
  FrameIter iter(cx, capture.is<JS::AllFrames>()
                         ? FrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK
                         : FrameIter::FOLLOW_DEBUGGER_EVAL_PREV_LINK);

  // Once we've seen one frame with its hasCachedSavedFrame bit set, all its
  // parents (that can be cached) ought to have it set too.
  DebugOnly<bool> seenCached = false;

  bool seenDebuggerEvalFrame = false;

  while (!iter.done()) {
    Activation& activation = *iter.activation();
    Maybe<LiveSavedFrameCache::FramePtr> framePtr =
        LiveSavedFrameCache::FramePtr::create(iter);

    if (framePtr) {
      // See the comment in Stack.h for why RematerializedFrames
      // are a special case here.
      MOZ_ASSERT_IF(seenCached, framePtr->hasCachedSavedFrame() ||
                                    framePtr->isRematerializedFrame());
      seenCached |= framePtr->hasCachedSavedFrame();

      seenDebuggerEvalFrame |=
          (framePtr->isInterpreterFrame() &&
           framePtr->asInterpreterFrame().isDebuggerEvalFrame());
    }

    if (capture.is<JS::AllFrames>() && framePtr &&
        framePtr->hasCachedSavedFrame()) {
      auto* cache = activation.getLiveSavedFrameCache(cx);
      if (!cache) {
        return false;
      }
      cache->find(cx, *framePtr, iter.pc(), &cachedParentFrame);

      // Even though iter.hasCachedSavedFrame() was true, we may still get a
      // cache miss, if the frame's pc doesn't match the cache entry's, or if
      // the cache was emptied due to a realm mismatch.
      if (cachedParentFrame) {
        break;
      }

      // This frame doesn't have a cache entry, despite its hasCachedSavedFrame
      // flag being set. If this was due to a pc mismatch, we can clear the flag
      // here and set things right. If the cache was emptied due to a realm
      // mismatch, we should clear all the frames' flags as we walk to the
      // bottom of the stack, so that they are all clear before we start pushing
      // any new entries.
      framePtr->clearHasCachedSavedFrame();
    }

    // We'll be pushing this frame onto stackChain. Gather the information
    // needed to construct the SavedFrame::Lookup.
    Rooted<LocationValue> location(cx);
    {
      AutoRealmUnchecked ar(cx, iter.realm());
      if (!cx->realm()->savedStacks().getLocation(cx, iter, &location)) {
        return false;
      }
    }

    RootedAtom displayAtom(cx, iter.maybeFunctionDisplayAtom());

    auto principals = iter.realm()->principals();
    MOZ_ASSERT_IF(framePtr && !iter.isWasm(), iter.pc());

    if (!stackChain.emplaceBack(location.source(), location.sourceId(),
                                location.line(), location.column(), displayAtom,
                                nullptr,  // asyncCause
                                nullptr,  // parent (not known yet)
                                principals, iter.mutedErrors(), framePtr,
                                iter.pc(), &activation)) {
      ReportOutOfMemory(cx);
      return false;
    }

    if (captureIsSatisfied(cx, principals, location.source(), capture)) {
      break;
    }

    ++iter;
    framePtr = LiveSavedFrameCache::FramePtr::create(iter);

    if (iter.activation() != &activation && capture.is<JS::AllFrames>()) {
      // If there were no cache hits in the entire activation, clear its
      // cache so we'll be able to push new ones when we build the
      // SavedFrame chain.
      activation.clearLiveSavedFrameCache();
    }

    // If we have crossed into a new activation, check whether the prior
    // activation had an async parent set.
    //
    // If the async call was explicit (async function resumptions, most
    // testing facilities), then the async parent stack has priority over
    // any actual frames still on the JavaScript stack. If the async call
    // was implicit (DOM CallbackObject::CallSetup calls), then the async
    // parent stack is used only if there were no other frames on the
    // stack.
    //
    // Captures using FirstSubsumedFrame expect us to ignore async parents.
    if (iter.activation() != &activation && activation.asyncStack() &&
        (activation.asyncCallIsExplicit() || iter.done()) &&
        !capture.is<JS::FirstSubsumedFrame>()) {
      // Atomize the async cause string. There should only be a few
      // different strings used.
      const char* cause = activation.asyncCause();
      RootedAtom causeAtom(cx, AtomizeUTF8Chars(cx, cause, strlen(cause)));
      if (!causeAtom) {
        return false;
      }

      // Translate our capture into a frame count limit for
      // adoptAsyncStack, which will impose further limits.
      Maybe<size_t> maxFrames =
          !capture.is<JS::MaxFrames>() ? Nothing()
          : capture.as<JS::MaxFrames>().maxFrames == 0
              ? Nothing()
              : Some(capture.as<JS::MaxFrames>().maxFrames);

      // Clip the stack if needed, attach the async cause string to the
      // top frame, and copy it into our compartment if necessary.
      RootedSavedFrame asyncParent(cx, activation.asyncStack());
      if (!adoptAsyncStack(cx, &asyncParent, causeAtom, maxFrames)) {
        return false;
      }
      stackChain[stackChain.length() - 1].setParent(asyncParent);
      if (!(capture.is<JS::AllFrames>() && seenDebuggerEvalFrame)) {
        // In the case of a JS::AllFrames capture, we will be populating the
        // LiveSavedFrameCache in the second loop. In the case where there is
        // a debugger eval frame on the stack, the second loop will use
        // checkForEvalInFramePrev to skip from the eval frame to the "prev"
        // frame and assert that when this happens, the "prev"
        // frame is in the cache. In cases where there is an async stack
        // activation between the debugger eval frame and the "prev" frame,
        // breaking here would not populate the "prev" cache entry, causing
        // checkForEvalInFramePrev to fail.
        break;
      }
    }

    if (capture.is<JS::MaxFrames>()) {
      capture.as<JS::MaxFrames>().maxFrames--;
    }
  }

  // Iterate through |stackChain| in reverse order and get or create the
  // actual SavedFrame instances.
  frame.set(cachedParentFrame);
  for (size_t i = stackChain.length(); i != 0; i--) {
    MutableHandle<SavedFrame::Lookup> lookup = stackChain[i - 1];
    if (!lookup.parent()) {
      // The frame may already have an async parent frame set explicitly
      // on its activation.
      lookup.setParent(frame);
    }

    // If necessary, adjust the parent of a debugger eval frame to point to
    // the frame in whose scope the eval occurs - if we're using
    // LiveSavedFrameCache. Otherwise, we simply ask the FrameIter to follow
    // evalInFramePrev links, so that the parent is always the last frame we
    // created.
    if (capture.is<JS::AllFrames>() && lookup.framePtr()) {
      if (!checkForEvalInFramePrev(cx, lookup)) {
        return false;
      }
    }

    frame.set(getOrCreateSavedFrame(cx, lookup));
    if (!frame) {
      return false;
    }

    if (capture.is<JS::AllFrames>() && lookup.framePtr()) {
      auto* cache = lookup.activation()->getLiveSavedFrameCache(cx);
      if (!cache ||
          !cache->insert(cx, *lookup.framePtr(), lookup.pc(), frame)) {
        return false;
      }
    }
  }

  return true;
}

bool SavedStacks::adoptAsyncStack(JSContext* cx,
                                  MutableHandleSavedFrame asyncStack,
                                  HandleAtom asyncCause,
                                  const Maybe<size_t>& maxFrameCount) {
  MOZ_ASSERT(asyncStack);
  MOZ_ASSERT(asyncCause);

  // If maxFrameCount is Nothing, the caller asked for an unlimited number of
  // stack frames, but async stacks are not limited by the available stack
  // memory, so we need to set an arbitrary limit when collecting them. We
  // still don't enforce an upper limit if the caller requested more frames.
  size_t maxFrames = maxFrameCount.valueOr(ASYNC_STACK_MAX_FRAME_COUNT);

  // Turn the chain of frames starting with asyncStack into a vector of Lookup
  // objects in |stackChain|, youngest to oldest.
  Rooted<js::GCLookupVector> stackChain(cx, js::GCLookupVector(cx));
  SavedFrame* currentSavedFrame = asyncStack;
  while (currentSavedFrame && stackChain.length() < maxFrames) {
    if (!stackChain.emplaceBack(*currentSavedFrame)) {
      ReportOutOfMemory(cx);
      return false;
    }

    currentSavedFrame = currentSavedFrame->getParent();
  }

  // Attach the asyncCause to the youngest frame.
  stackChain[0].setAsyncCause(asyncCause);

  // If we walked the entire stack, and it's in cx's realm, we don't
  // need to rebuild the full chain again using the lookup objects - we can
  // just use the existing chain. Only the asyncCause on the youngest frame
  // needs to be changed.
  if (currentSavedFrame == nullptr && asyncStack->realm() == cx->realm()) {
    MutableHandle<SavedFrame::Lookup> lookup = stackChain[0];
    lookup.setParent(asyncStack->getParent());
    asyncStack.set(getOrCreateSavedFrame(cx, lookup));
    return !!asyncStack;
  }

  // If we captured the maximum number of frames and the caller requested no
  // specific limit, we only return half of them. This means that if we do
  // many subsequent captures with the same async stack, it's likely we can
  // use the optimization above.
  if (maxFrameCount.isNothing() && currentSavedFrame) {
    stackChain.shrinkBy(ASYNC_STACK_MAX_FRAME_COUNT / 2);
  }

  // Iterate through |stackChain| in reverse order and get or create the
  // actual SavedFrame instances.
  asyncStack.set(nullptr);
  while (!stackChain.empty()) {
    Rooted<SavedFrame::Lookup> lookup(cx, stackChain.back());
    lookup.setParent(asyncStack);
    asyncStack.set(getOrCreateSavedFrame(cx, lookup));
    if (!asyncStack) {
      return false;
    }
    stackChain.popBack();
  }

  return true;
}

// Given a |lookup| for which we're about to construct a SavedFrame, if it
// refers to a Debugger eval frame, adjust |lookup|'s parent to be the frame's
// evalInFramePrev target.
//
// Debugger eval frames run code in the scope of some random older frame on the
// stack (the 'target' frame). It is our custom to report the target as the
// immediate parent of the eval frame. The LiveSavedFrameCache requires us not
// to skip frames, so instead we walk the entire stack, and just give Debugger
// eval frames the right parents as we encounter them.
//
// Call this function only if we are using the LiveSavedFrameCache; otherwise,
// FrameIter has already taken care of getting us the right parent.
bool SavedStacks::checkForEvalInFramePrev(
    JSContext* cx, MutableHandle<SavedFrame::Lookup> lookup) {
  MOZ_ASSERT(lookup.framePtr());
  if (!lookup.framePtr()->isInterpreterFrame()) {
    return true;
  }

  InterpreterFrame& interpreterFrame = lookup.framePtr()->asInterpreterFrame();
  if (!interpreterFrame.isDebuggerEvalFrame()) {
    return true;
  }

  FrameIter iter(cx, FrameIter::IGNORE_DEBUGGER_EVAL_PREV_LINK);
  while (!iter.done() &&
         (!iter.hasUsableAbstractFramePtr() ||
          iter.abstractFramePtr() != interpreterFrame.evalInFramePrev())) {
    ++iter;
  }

  Maybe<LiveSavedFrameCache::FramePtr> maybeTarget =
      LiveSavedFrameCache::FramePtr::create(iter);
  MOZ_ASSERT(maybeTarget);

  LiveSavedFrameCache::FramePtr target = *maybeTarget;

  // If we're caching the frame to which |lookup| refers, then we should
  // definitely have the target frame in the cache as well.
  MOZ_ASSERT(target.hasCachedSavedFrame());

  // Search the chain of activations for a LiveSavedFrameCache that has an
  // entry for target.
  RootedSavedFrame saved(cx, nullptr);
  for (Activation* act = lookup.activation(); act; act = act->prev()) {
    // It's okay to force allocation of a cache here; we're about to put
    // something in the top cache, and all the lower ones should exist
    // already.
    auto* cache = act->getLiveSavedFrameCache(cx);
    if (!cache) {
      return false;
    }

    cache->findWithoutInvalidation(target, &saved);
    if (saved) {
      break;
    }
  }

  // Since |target| has its cached bit set, we should have found it.
  MOZ_ALWAYS_TRUE(saved);

  // Because we use findWithoutInvalidation here, we can technically get a
  // SavedFrame here for any realm. That shouldn't happen here because
  // checkForEvalInFramePrev is only called _after_ the parent frames have
  // been constructed, but if something prevents the chain from being properly
  // reconstructed, that invariant could be accidentally broken.
  MOZ_ASSERT(saved->realm() == cx->realm());

  lookup.setParent(saved);
  return true;
}

SavedFrame* SavedStacks::getOrCreateSavedFrame(
    JSContext* cx, Handle<SavedFrame::Lookup> lookup) {
  const SavedFrame::Lookup& lookupInstance = lookup.get();
  DependentAddPtr<SavedFrame::Set> p(cx, frames, lookupInstance);
  if (p) {
    MOZ_ASSERT(*p);
    return *p;
  }

  RootedSavedFrame frame(cx, createFrameFromLookup(cx, lookup));
  if (!frame) {
    return nullptr;
  }

  if (!p.add(cx, frames, lookupInstance, frame)) {
    return nullptr;
  }

  return frame;
}

SavedFrame* SavedStacks::createFrameFromLookup(
    JSContext* cx, Handle<SavedFrame::Lookup> lookup) {
  RootedSavedFrame frame(cx, SavedFrame::create(cx));
  if (!frame) {
    return nullptr;
  }
  frame->initFromLookup(cx, lookup);

  if (!FreezeObject(cx, frame)) {
    return nullptr;
  }

  return frame;
}

bool SavedStacks::getLocation(JSContext* cx, const FrameIter& iter,
                              MutableHandle<LocationValue> locationp) {
  // We should only ever be caching location values for scripts in this
  // compartment. Otherwise, we would get dead cross-compartment scripts in
  // the cache because our compartment's sweep method isn't called when their
  // compartment gets collected.
  MOZ_DIAGNOSTIC_ASSERT(&cx->realm()->savedStacks() == this);
  cx->check(iter.compartment());

  // When we have a |JSScript| for this frame, use a potentially memoized
  // location from our PCLocationMap and copy it into |locationp|. When we do
  // not have a |JSScript| for this frame (wasm frames), we take a slow path
  // that doesn't employ memoization, and update |locationp|'s slots directly.

  if (iter.isWasm()) {
    // Only asm.js has a displayURL.
    if (const char16_t* displayURL = iter.displayURL()) {
      locationp.setSource(AtomizeChars(cx, displayURL, js_strlen(displayURL)));
    } else {
      const char* filename = iter.filename() ? iter.filename() : "";
      locationp.setSource(Atomize(cx, filename, strlen(filename)));
    }
    if (!locationp.source()) {
      return false;
    }

    // See WasmFrameIter::computeLine() comment.
    uint32_t column = 0;
    locationp.setLine(iter.computeLine(&column));
    locationp.setColumn(column);
    return true;
  }

  RootedScript script(cx, iter.script());
  jsbytecode* pc = iter.pc();

  PCKey key(script, pc);
  PCLocationMap::AddPtr p = pcLocationMap.lookupForAdd(key);

  if (!p) {
    RootedAtom source(cx);
    if (const char16_t* displayURL = iter.displayURL()) {
      source = AtomizeChars(cx, displayURL, js_strlen(displayURL));
    } else {
      const char* filename = script->filename() ? script->filename() : "";
      source = Atomize(cx, filename, strlen(filename));
    }
    if (!source) {
      return false;
    }

    uint32_t sourceId = script->scriptSource()->id();
    uint32_t column;
    uint32_t line = PCToLineNumber(script, pc, &column);

    // Make the column 1-based. See comment above.
    LocationValue value(source, sourceId, line, column + 1);
    if (!pcLocationMap.add(p, key, value)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  locationp.set(p->value());
  return true;
}

void SavedStacks::chooseSamplingProbability(Realm* realm) {
  {
    JSRuntime* runtime = realm->runtimeFromMainThread();
    if (runtime->recordAllocationCallback) {
      // The runtime is tracking allocations across all realms, in this case
      // ignore all of the debugger values, and use the runtime's probability.
      this->setSamplingProbability(runtime->allocationSamplingProbability);
      return;
    }
  }

  // Use unbarriered version to prevent triggering read barrier while
  // collecting, this is safe as long as global does not escape.
  GlobalObject* global = realm->unsafeUnbarrieredMaybeGlobal();
  if (!global) {
    return;
  }

  Maybe<double> probability = DebugAPI::allocationSamplingProbability(global);
  if (probability.isNothing()) {
    return;
  }

  this->setSamplingProbability(*probability);
}

void SavedStacks::setSamplingProbability(double probability) {
  if (!bernoulliSeeded) {
    mozilla::Array<uint64_t, 2> seed;
    GenerateXorShift128PlusSeed(seed);
    bernoulli.setRandomState(seed[0], seed[1]);
    bernoulliSeeded = true;
  }

  bernoulli.setProbability(probability);
}

JSObject* SavedStacks::MetadataBuilder::build(
    JSContext* cx, HandleObject target,
    AutoEnterOOMUnsafeRegion& oomUnsafe) const {
  RootedObject obj(cx, target);

  SavedStacks& stacks = cx->realm()->savedStacks();
  if (!stacks.bernoulli.trial()) {
    return nullptr;
  }

  RootedSavedFrame frame(cx);
  if (!stacks.saveCurrentStack(cx, &frame)) {
    oomUnsafe.crash("SavedStacksMetadataBuilder");
  }

  if (!DebugAPI::onLogAllocationSite(cx, obj, frame,
                                     mozilla::TimeStamp::Now())) {
    oomUnsafe.crash("SavedStacksMetadataBuilder");
  }

  auto recordAllocationCallback =
      cx->realm()->runtimeFromMainThread()->recordAllocationCallback;
  if (recordAllocationCallback) {
    // The following code translates the JS-specific information, into an
    // RecordAllocationInfo object that can be consumed outside of SpiderMonkey.

    auto node = JS::ubi::Node(obj.get());

    // Pass the non-SpiderMonkey specific information back to the
    // callback to get it out of the JS engine.
    recordAllocationCallback(JS::RecordAllocationInfo{
        node.typeName(), node.jsObjectClassName(), node.descriptiveTypeName(),
        JS::ubi::CoarseTypeToString(node.coarseType()),
        node.size(cx->runtime()->debuggerMallocSizeOf),
        gc::IsInsideNursery(obj)});
  }

  MOZ_ASSERT_IF(frame, !frame->is<WrapperObject>());
  return frame;
}

const SavedStacks::MetadataBuilder SavedStacks::metadataBuilder;

/* static */
ReconstructedSavedFramePrincipals ReconstructedSavedFramePrincipals::IsSystem;
/* static */
ReconstructedSavedFramePrincipals
    ReconstructedSavedFramePrincipals::IsNotSystem;

UniqueChars BuildUTF8StackString(JSContext* cx, JSPrincipals* principals,
                                 HandleObject stack) {
  RootedString stackStr(cx);
  if (!JS::BuildStackString(cx, principals, stack, &stackStr)) {
    return nullptr;
  }

  return JS_EncodeStringToUTF8(cx, stackStr);
}

uint32_t FixupColumnForDisplay(uint32_t column) {
  // As described in WasmFrameIter::computeLine(), for wasm frames, the
  // function index is returned as the column with the high bit set. In paths
  // that format error stacks into strings, this information can be used to
  // synthesize a proper wasm frame. But when raw column numbers are handed
  // out, we just fix them to 1 to avoid confusion.
  if (column & wasm::WasmFrameIter::ColumnBit) {
    return 1;
  }

  // XXX: Make the column 1-based as in other browsers, instead of 0-based
  // which is how SpiderMonkey stores it internally. This will be
  // unnecessary once bug 1144340 is fixed.
  return column + 1;
}

} /* namespace js */

namespace JS {
namespace ubi {

bool ConcreteStackFrame<SavedFrame>::isSystem() const {
  auto trustedPrincipals = get().runtimeFromAnyThread()->trustedPrincipals();
  return get().getPrincipals() == trustedPrincipals ||
         get().getPrincipals() ==
             &js::ReconstructedSavedFramePrincipals::IsSystem;
}

bool ConcreteStackFrame<SavedFrame>::constructSavedFrameStack(
    JSContext* cx, MutableHandleObject outSavedFrameStack) const {
  outSavedFrameStack.set(&get());
  if (!cx->compartment()->wrap(cx, outSavedFrameStack)) {
    outSavedFrameStack.set(nullptr);
    return false;
  }
  return true;
}

// A `mozilla::Variant` matcher that converts the inner value of a
// `JS::ubi::AtomOrTwoByteChars` string to a `JSAtom*`.
struct MOZ_STACK_CLASS AtomizingMatcher {
  JSContext* cx;
  size_t length;

  explicit AtomizingMatcher(JSContext* cx, size_t length)
      : cx(cx), length(length) {}

  JSAtom* operator()(JSAtom* atom) {
    MOZ_ASSERT(atom);
    return atom;
  }

  JSAtom* operator()(const char16_t* chars) {
    MOZ_ASSERT(chars);
    return AtomizeChars(cx, chars, length);
  }
};

JS_PUBLIC_API bool ConstructSavedFrameStackSlow(
    JSContext* cx, JS::ubi::StackFrame& frame,
    MutableHandleObject outSavedFrameStack) {
  Rooted<js::GCLookupVector> stackChain(cx, js::GCLookupVector(cx));
  Rooted<JS::ubi::StackFrame> ubiFrame(cx, frame);

  while (ubiFrame.get()) {
    // Convert the source and functionDisplayName strings to atoms.

    js::RootedAtom source(cx);
    AtomizingMatcher atomizer(cx, ubiFrame.get().sourceLength());
    source = ubiFrame.get().source().match(atomizer);
    if (!source) {
      return false;
    }

    js::RootedAtom functionDisplayName(cx);
    auto nameLength = ubiFrame.get().functionDisplayNameLength();
    if (nameLength > 0) {
      AtomizingMatcher atomizer(cx, nameLength);
      functionDisplayName =
          ubiFrame.get().functionDisplayName().match(atomizer);
      if (!functionDisplayName) {
        return false;
      }
    }

    auto principals =
        js::ReconstructedSavedFramePrincipals::getSingleton(ubiFrame.get());

    if (!stackChain.emplaceBack(source, ubiFrame.get().sourceId(),
                                ubiFrame.get().line(), ubiFrame.get().column(),
                                functionDisplayName,
                                /* asyncCause */ nullptr,
                                /* parent */ nullptr, principals,
                                /* mutedErrors */ true)) {
      ReportOutOfMemory(cx);
      return false;
    }

    ubiFrame = ubiFrame.get().parent();
  }

  js::RootedSavedFrame parentFrame(cx);
  for (size_t i = stackChain.length(); i != 0; i--) {
    MutableHandle<SavedFrame::Lookup> lookup = stackChain[i - 1];
    lookup.setParent(parentFrame);
    parentFrame = cx->realm()->savedStacks().getOrCreateSavedFrame(cx, lookup);
    if (!parentFrame) {
      return false;
    }
  }

  outSavedFrameStack.set(parentFrame);
  return true;
}

}  // namespace ubi
}  // namespace JS
