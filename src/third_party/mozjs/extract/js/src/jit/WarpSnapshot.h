/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_WarpSnapshot_h
#define jit_WarpSnapshot_h

#include "mozilla/LinkedList.h"
#include "mozilla/Variant.h"

#include "builtin/ModuleObject.h"
#include "gc/Policy.h"
#include "jit/JitAllocPolicy.h"
#include "jit/JitContext.h"
#include "jit/TypeData.h"
#include "vm/EnvironmentObject.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags

namespace js {

class ArgumentsObject;
class CallObject;
class GlobalLexicalEnvironmentObject;
class LexicalEnvironmentObject;
class ModuleEnvironmentObject;
class NamedLambdaObject;

namespace jit {

class CacheIRStubInfo;
class CompileInfo;
class WarpScriptSnapshot;

#define WARP_OP_SNAPSHOT_LIST(_) \
  _(WarpArguments)               \
  _(WarpRegExp)                  \
  _(WarpBuiltinObject)           \
  _(WarpGetIntrinsic)            \
  _(WarpGetImport)               \
  _(WarpRest)                    \
  _(WarpBindGName)               \
  _(WarpVarEnvironment)          \
  _(WarpLexicalEnvironment)      \
  _(WarpClassBodyEnvironment)    \
  _(WarpBailout)                 \
  _(WarpCacheIR)                 \
  _(WarpInlinedCall)             \
  _(WarpPolymorphicTypes)

// Wrapper for GC things stored in WarpSnapshot. Asserts the GC pointer is not
// nursery-allocated. These pointers must be traced using TraceWarpGCPtr.
template <typename T>
class WarpGCPtr {
  // Note: no pre-barrier is needed because this is a constant. No post-barrier
  // is needed because the value is always tenured.
  const T ptr_;

 public:
  explicit WarpGCPtr(const T& ptr) : ptr_(ptr) {
    MOZ_ASSERT(JS::GCPolicy<T>::isTenured(ptr),
               "WarpSnapshot pointers must be tenured");
  }
  WarpGCPtr(const WarpGCPtr<T>& other) = default;

  operator T() const { return ptr_; }
  T operator->() const { return ptr_; }

 private:
  WarpGCPtr() = delete;
  void operator=(WarpGCPtr<T>& other) = delete;
};

// WarpOpSnapshot is the base class for data attached to a single bytecode op by
// WarpOracle. This is typically data that WarpBuilder can't read off-thread
// without racing.
class WarpOpSnapshot : public TempObject,
                       public mozilla::LinkedListElement<WarpOpSnapshot> {
 public:
  enum class Kind : uint16_t {
#define DEF_KIND(KIND) KIND,
    WARP_OP_SNAPSHOT_LIST(DEF_KIND)
#undef DEF_KIND
  };

 private:
  // Bytecode offset.
  uint32_t offset_ = 0;

  Kind kind_;

 protected:
  WarpOpSnapshot(Kind kind, uint32_t offset) : offset_(offset), kind_(kind) {}

 public:
  uint32_t offset() const { return offset_; }
  Kind kind() const { return kind_; }

  template <typename T>
  const T* as() const {
    MOZ_ASSERT(kind_ == T::ThisKind);
    return static_cast<const T*>(this);
  }

  template <typename T>
  T* as() {
    MOZ_ASSERT(kind_ == T::ThisKind);
    return static_cast<T*>(this);
  }

  void trace(JSTracer* trc);

#ifdef JS_JITSPEW
  void dump(GenericPrinter& out, JSScript* script) const;
#endif
};

using WarpOpSnapshotList = mozilla::LinkedList<WarpOpSnapshot>;

// Template object for JSOp::Arguments.
class WarpArguments : public WarpOpSnapshot {
  // Note: this can be nullptr if the realm has no template object yet.
  WarpGCPtr<ArgumentsObject*> templateObj_;

 public:
  static constexpr Kind ThisKind = Kind::WarpArguments;

  WarpArguments(uint32_t offset, ArgumentsObject* templateObj)
      : WarpOpSnapshot(ThisKind, offset), templateObj_(templateObj) {}
  ArgumentsObject* templateObj() const { return templateObj_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// The "has RegExpShared" state for JSOp::RegExp's template object.
class WarpRegExp : public WarpOpSnapshot {
  bool hasShared_;

 public:
  static constexpr Kind ThisKind = Kind::WarpRegExp;

  WarpRegExp(uint32_t offset, bool hasShared)
      : WarpOpSnapshot(ThisKind, offset), hasShared_(hasShared) {}
  bool hasShared() const { return hasShared_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// The object for JSOp::BuiltinObject if it exists at compile-time.
class WarpBuiltinObject : public WarpOpSnapshot {
  WarpGCPtr<JSObject*> builtin_;

 public:
  static constexpr Kind ThisKind = Kind::WarpBuiltinObject;

  WarpBuiltinObject(uint32_t offset, JSObject* builtin)
      : WarpOpSnapshot(ThisKind, offset), builtin_(builtin) {
    MOZ_ASSERT(builtin);
  }
  JSObject* builtin() const { return builtin_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// The intrinsic for JSOp::GetIntrinsic if it exists at compile-time.
class WarpGetIntrinsic : public WarpOpSnapshot {
  WarpGCPtr<Value> intrinsic_;

 public:
  static constexpr Kind ThisKind = Kind::WarpGetIntrinsic;

  WarpGetIntrinsic(uint32_t offset, const Value& intrinsic)
      : WarpOpSnapshot(ThisKind, offset), intrinsic_(intrinsic) {}
  Value intrinsic() const { return intrinsic_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Target module environment and slot information for JSOp::GetImport.
class WarpGetImport : public WarpOpSnapshot {
  WarpGCPtr<ModuleEnvironmentObject*> targetEnv_;
  uint32_t numFixedSlots_;
  uint32_t slot_;
  bool needsLexicalCheck_;

 public:
  static constexpr Kind ThisKind = Kind::WarpGetImport;

  WarpGetImport(uint32_t offset, ModuleEnvironmentObject* targetEnv,
                uint32_t numFixedSlots, uint32_t slot, bool needsLexicalCheck)
      : WarpOpSnapshot(ThisKind, offset),
        targetEnv_(targetEnv),
        numFixedSlots_(numFixedSlots),
        slot_(slot),
        needsLexicalCheck_(needsLexicalCheck) {}
  ModuleEnvironmentObject* targetEnv() const { return targetEnv_; }
  uint32_t numFixedSlots() const { return numFixedSlots_; }
  uint32_t slot() const { return slot_; }
  bool needsLexicalCheck() const { return needsLexicalCheck_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Informs WarpBuilder that an IC site is cold and execution should bail out.
class WarpBailout : public WarpOpSnapshot {
 public:
  static constexpr Kind ThisKind = Kind::WarpBailout;

  explicit WarpBailout(uint32_t offset) : WarpOpSnapshot(ThisKind, offset) {}

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Information from a Baseline IC stub.
class WarpCacheIR : public WarpOpSnapshot {
  // Baseline stub code. Stored here to keep the CacheIRStubInfo alive.
  WarpGCPtr<JitCode*> stubCode_;
  const CacheIRStubInfo* stubInfo_;

  // Copied Baseline stub data. Allocated in the same LifoAlloc.
  const uint8_t* stubData_;

 public:
  static constexpr Kind ThisKind = Kind::WarpCacheIR;

  WarpCacheIR(uint32_t offset, JitCode* stubCode,
              const CacheIRStubInfo* stubInfo, const uint8_t* stubData)
      : WarpOpSnapshot(ThisKind, offset),
        stubCode_(stubCode),
        stubInfo_(stubInfo),
        stubData_(stubData) {}

  const CacheIRStubInfo* stubInfo() const { return stubInfo_; }
  const uint8_t* stubData() const { return stubData_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// [SMDOC] Warp Nursery Object support
//
// CacheIR stub data can contain nursery allocated objects. This can happen for
// example for GuardSpecificObject/GuardSpecificFunction or GuardProto.
//
// To support nursery GCs in parallel with off-thread compilation, we use the
// following mechanism:
//
// * When WarpOracle copies stub data, it builds a Vector of nursery objects.
//   The nursery object pointers in the stub data are replaced with the
//   corresponding index into this Vector.
//   See WarpScriptOracle::replaceNurseryPointers.
//
// * The Vector is copied to the snapshot and, at the end of compilation, to
//   the IonScript. The Vector is only accessed on the main thread.
//
// * The MIR backend never accesses the raw JSObject*. Instead, it uses
//   MNurseryObject which will load the object at runtime from the IonScript.
//
// WarpObjectField is a helper class to encode/decode a stub data field that
// either stores an object or a nursery index.
class WarpObjectField {
  // This is a nursery index if the low bit is set. Else it's a JSObject*.
  static constexpr uintptr_t NurseryIndexTag = 0x1;
  static constexpr uintptr_t NurseryIndexShift = 1;

  uintptr_t data_;

  explicit WarpObjectField(uintptr_t data) : data_(data) {}

 public:
  static WarpObjectField fromData(uintptr_t data) {
    return WarpObjectField(data);
  }
  static WarpObjectField fromObject(JSObject* obj) {
    return WarpObjectField(uintptr_t(obj));
  }
  static WarpObjectField fromNurseryIndex(uint32_t index) {
    uintptr_t data = (uintptr_t(index) << NurseryIndexShift) | NurseryIndexTag;
    return WarpObjectField(data);
  }

  uintptr_t rawData() const { return data_; }

  bool isNurseryIndex() const { return (data_ & NurseryIndexTag) != 0; }

  uint32_t toNurseryIndex() const {
    MOZ_ASSERT(isNurseryIndex());
    return data_ >> NurseryIndexShift;
  }

  JSObject* toObject() const {
    MOZ_ASSERT(!isNurseryIndex());
    return reinterpret_cast<JSObject*>(data_);
  }
};

// Information for inlining a scripted call IC.
class WarpInlinedCall : public WarpOpSnapshot {
  // Used for generating the correct guards.
  WarpCacheIR* cacheIRSnapshot_;

  // Used for generating the inlined code.
  WarpScriptSnapshot* scriptSnapshot_;
  CompileInfo* info_;

 public:
  static constexpr Kind ThisKind = Kind::WarpInlinedCall;

  WarpInlinedCall(uint32_t offset, WarpCacheIR* cacheIRSnapshot,
                  WarpScriptSnapshot* scriptSnapshot, CompileInfo* info)
      : WarpOpSnapshot(ThisKind, offset),
        cacheIRSnapshot_(cacheIRSnapshot),
        scriptSnapshot_(scriptSnapshot),
        info_(info) {}

  WarpCacheIR* cacheIRSnapshot() const { return cacheIRSnapshot_; }
  WarpScriptSnapshot* scriptSnapshot() const { return scriptSnapshot_; }
  CompileInfo* info() const { return info_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Information for inlining an ordered set of types
class WarpPolymorphicTypes : public WarpOpSnapshot {
  TypeDataList list_;

 public:
  static constexpr Kind ThisKind = Kind::WarpPolymorphicTypes;

  WarpPolymorphicTypes(uint32_t offset, TypeDataList list)
      : WarpOpSnapshot(ThisKind, offset), list_(list) {}

  const TypeDataList& list() const { return list_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Shape for JSOp::Rest.
class WarpRest : public WarpOpSnapshot {
  WarpGCPtr<Shape*> shape_;

 public:
  static constexpr Kind ThisKind = Kind::WarpRest;

  WarpRest(uint32_t offset, Shape* shape)
      : WarpOpSnapshot(ThisKind, offset), shape_(shape) {}

  Shape* shape() const { return shape_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Global environment for BindGName
class WarpBindGName : public WarpOpSnapshot {
  WarpGCPtr<JSObject*> globalEnv_;

 public:
  static constexpr Kind ThisKind = Kind::WarpBindGName;

  WarpBindGName(uint32_t offset, JSObject* globalEnv)
      : WarpOpSnapshot(ThisKind, offset), globalEnv_(globalEnv) {}

  JSObject* globalEnv() const { return globalEnv_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Block environment for PushVarEnv
class WarpVarEnvironment : public WarpOpSnapshot {
  WarpGCPtr<VarEnvironmentObject*> templateObj_;

 public:
  static constexpr Kind ThisKind = Kind::WarpVarEnvironment;

  WarpVarEnvironment(uint32_t offset, VarEnvironmentObject* templateObj)
      : WarpOpSnapshot(ThisKind, offset), templateObj_(templateObj) {}

  VarEnvironmentObject* templateObj() const { return templateObj_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Block environment for PushLexicalEnv, FreshenLexicalEnv, RecreateLexicalEnv
class WarpLexicalEnvironment : public WarpOpSnapshot {
  WarpGCPtr<BlockLexicalEnvironmentObject*> templateObj_;

 public:
  static constexpr Kind ThisKind = Kind::WarpLexicalEnvironment;

  WarpLexicalEnvironment(uint32_t offset,
                         BlockLexicalEnvironmentObject* templateObj)
      : WarpOpSnapshot(ThisKind, offset), templateObj_(templateObj) {}

  BlockLexicalEnvironmentObject* templateObj() const { return templateObj_; }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

// Class body lexical environment for PushClassBodyEnv
class WarpClassBodyEnvironment : public WarpOpSnapshot {
  WarpGCPtr<ClassBodyLexicalEnvironmentObject*> templateObj_;

 public:
  static constexpr Kind ThisKind = Kind::WarpClassBodyEnvironment;

  WarpClassBodyEnvironment(uint32_t offset,
                           ClassBodyLexicalEnvironmentObject* templateObj)
      : WarpOpSnapshot(ThisKind, offset), templateObj_(templateObj) {}

  ClassBodyLexicalEnvironmentObject* templateObj() const {
    return templateObj_;
  }

  void traceData(JSTracer* trc);

#ifdef JS_JITSPEW
  void dumpData(GenericPrinter& out) const;
#endif
};

struct NoEnvironment {};
using ConstantObjectEnvironment = WarpGCPtr<JSObject*>;
struct FunctionEnvironment {
  WarpGCPtr<CallObject*> callObjectTemplate;
  WarpGCPtr<NamedLambdaObject*> namedLambdaTemplate;

 public:
  FunctionEnvironment(CallObject* callObjectTemplate,
                      NamedLambdaObject* namedLambdaTemplate)
      : callObjectTemplate(callObjectTemplate),
        namedLambdaTemplate(namedLambdaTemplate) {}
};

// Snapshot data for the environment object(s) created in the script's prologue.
//
// One of:
//
// * NoEnvironment: No environment object should be set. Leave the slot
//   initialized to |undefined|.
//
// * ConstantObjectEnvironment: Use this JSObject* as environment object.
//
// * FunctionEnvironment: Use the callee's environment chain. Optionally
//   allocate a new NamedLambdaObject and/or CallObject based on
//   namedLambdaTemplate and callObjectTemplate.
using WarpEnvironment =
    mozilla::Variant<NoEnvironment, ConstantObjectEnvironment,
                     FunctionEnvironment>;

// Snapshot data for a single JSScript.
class WarpScriptSnapshot
    : public TempObject,
      public mozilla::LinkedListElement<WarpScriptSnapshot> {
  WarpGCPtr<JSScript*> script_;
  WarpEnvironment environment_;
  WarpOpSnapshotList opSnapshots_;

  // If the script has a JSOp::ImportMeta op, this is the module to bake in.
  WarpGCPtr<ModuleObject*> moduleObject_;

  // Whether this script is for an arrow function.
  bool isArrowFunction_;

 public:
  WarpScriptSnapshot(JSScript* script, const WarpEnvironment& env,
                     WarpOpSnapshotList&& opSnapshots,
                     ModuleObject* moduleObject);

  JSScript* script() const { return script_; }
  const WarpEnvironment& environment() const { return environment_; }
  const WarpOpSnapshotList& opSnapshots() const { return opSnapshots_; }
  ModuleObject* moduleObject() const { return moduleObject_; }

  bool isArrowFunction() const { return isArrowFunction_; }

  void trace(JSTracer* trc);

#ifdef JS_JITSPEW
  void dump(GenericPrinter& out) const;
#endif
};

// Captures information from previous bailouts to prevent bailout/recompile
// loops.
class WarpBailoutInfo {
  // True if any script in the compilation has the failedBoundsCheck flag. In
  // this case mark bounds checks as non-movable to prevent hoisting them in
  // TryEliminateBoundsCheck.
  bool failedBoundsCheck_ = false;

  // True if any script in the compilation has the failedLexicalCheck flag. In
  // this case mark lexical checks as non-movable.
  bool failedLexicalCheck_ = false;

 public:
  bool failedBoundsCheck() const { return failedBoundsCheck_; }
  void setFailedBoundsCheck() { failedBoundsCheck_ = true; }

  bool failedLexicalCheck() const { return failedLexicalCheck_; }
  void setFailedLexicalCheck() { failedLexicalCheck_ = true; }
};

using WarpScriptSnapshotList = mozilla::LinkedList<WarpScriptSnapshot>;

// Data allocated by WarpOracle on the main thread that's used off-thread by
// WarpBuilder to build the MIR graph.
class WarpSnapshot : public TempObject {
  // The scripts being compiled.
  WarpScriptSnapshotList scriptSnapshots_;

  // The global lexical environment and its thisObject(). We don't inline
  // cross-realm calls so this can be stored once per snapshot.
  WarpGCPtr<GlobalLexicalEnvironmentObject*> globalLexicalEnv_;
  WarpGCPtr<JSObject*> globalLexicalEnvThis_;

  const WarpBailoutInfo bailoutInfo_;

  // List of (originally) nursery-allocated objects. Must only be accessed on
  // the main thread. See also WarpObjectField.
  using NurseryObjectVector = Vector<JSObject*, 0, JitAllocPolicy>;
  NurseryObjectVector nurseryObjects_;

#ifdef JS_CACHEIR_SPEW
  bool needsFinalWarmUpCount_ = false;
#endif

#ifdef DEBUG
  // A hash of the stub pointers and entry counts for each of the ICs
  // in this snapshot.
  mozilla::HashNumber icHash_ = 0;
#endif

 public:
  explicit WarpSnapshot(JSContext* cx, TempAllocator& alloc,
                        WarpScriptSnapshotList&& scriptSnapshots,
                        const WarpBailoutInfo& bailoutInfo,
                        bool recordWarmUpCount);

  WarpScriptSnapshot* rootScript() { return scriptSnapshots_.getFirst(); }
  const WarpScriptSnapshotList& scripts() const { return scriptSnapshots_; }

  GlobalLexicalEnvironmentObject* globalLexicalEnv() const {
    return globalLexicalEnv_;
  }
  JSObject* globalLexicalEnvThis() const { return globalLexicalEnvThis_; }

  void trace(JSTracer* trc);

  const WarpBailoutInfo& bailoutInfo() const { return bailoutInfo_; }

  NurseryObjectVector& nurseryObjects() { return nurseryObjects_; }
  const NurseryObjectVector& nurseryObjects() const { return nurseryObjects_; }

#ifdef DEBUG
  mozilla::HashNumber icHash() const { return icHash_; }
  void setICHash(mozilla::HashNumber hash) { icHash_ = hash; }
#endif

#ifdef JS_JITSPEW
  void dump() const;
  void dump(GenericPrinter& out) const;
#endif

#ifdef JS_CACHEIR_SPEW
  bool needsFinalWarmUpCount() const { return needsFinalWarmUpCount_; }
#endif
};

}  // namespace jit
}  // namespace js

#endif /* jit_WarpSnapshot_h */
