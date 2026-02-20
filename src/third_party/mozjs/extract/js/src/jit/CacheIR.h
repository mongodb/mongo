/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIR_h
#define jit_CacheIR_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"

#include "jit/CacheIROpsGenerated.h"
#include "js/GCAnnotations.h"
#include "js/Value.h"

struct JS_PUBLIC_API JSContext;

namespace js {
namespace jit {

// [SMDOC] CacheIR
//
// CacheIR is an (extremely simple) linear IR language for inline caches.
// From this IR, we can generate machine code for Baseline or Ion IC stubs.
//
// IRWriter
// --------
// CacheIR bytecode is written using IRWriter. This class also records some
// metadata that's used by the Baseline and Ion code generators to generate
// (efficient) machine code.
//
// Sharing Baseline stub code
// --------------------------
// Baseline stores data (like Shape* and fixed slot offsets) inside the ICStub
// structure, instead of embedding them directly in the JitCode. This makes
// Baseline IC code slightly slower, but allows us to share IC code between
// caches. CacheIR makes it easy to share code between stubs: stubs that have
// the same CacheIR (and CacheKind), will have the same Baseline stub code.
//
// Baseline stubs that share JitCode also share a CacheIRStubInfo structure.
// This class stores the CacheIR and the location of GC things stored in the
// stub, for the GC.
//
// JitZone has a CacheIRStubInfo* -> JitCode* weak map that's used to share both
// the IR and JitCode between Baseline CacheIR stubs. This HashMap owns the
// stubInfo (it uses UniquePtr), so once there are no references left to the
// shared stub code, we can also free the CacheIRStubInfo.
//
// Ion stubs
// ---------
// Unlike Baseline stubs, Ion stubs do not share stub code, and data stored in
// the IonICStub is baked into JIT code. This is one of the reasons Ion stubs
// are faster than Baseline stubs. Also note that Ion ICs contain more state
// (see IonGetPropertyIC for example) and use dynamic input/output registers,
// so sharing stub code for Ion would be much more difficult.

// An OperandId represents either a cache input or a value returned by a
// CacheIR instruction. Most code should use the ValOperandId and ObjOperandId
// classes below. The ObjOperandId class represents an operand that's known to
// be an object, just as StringOperandId represents a known string, etc.
class OperandId {
 protected:
  static const uint16_t InvalidId = UINT16_MAX;
  uint16_t id_;

  explicit OperandId(uint16_t id) : id_(id) {}

 public:
  OperandId() : id_(InvalidId) {}
  uint16_t id() const { return id_; }
  bool valid() const { return id_ != InvalidId; }
};

class ValOperandId : public OperandId {
 public:
  ValOperandId() = default;
  explicit ValOperandId(uint16_t id) : OperandId(id) {}
};

class ValueTagOperandId : public OperandId {
 public:
  ValueTagOperandId() = default;
  explicit ValueTagOperandId(uint16_t id) : OperandId(id) {}
};

class IntPtrOperandId : public OperandId {
 public:
  IntPtrOperandId() = default;
  explicit IntPtrOperandId(uint16_t id) : OperandId(id) {}
};

class ObjOperandId : public OperandId {
 public:
  ObjOperandId() = default;
  explicit ObjOperandId(uint16_t id) : OperandId(id) {}

  bool operator==(const ObjOperandId& other) const { return id_ == other.id_; }
  bool operator!=(const ObjOperandId& other) const { return id_ != other.id_; }
};

class NumberOperandId : public ValOperandId {
 public:
  NumberOperandId() = default;
  explicit NumberOperandId(uint16_t id) : ValOperandId(id) {}
};

class StringOperandId : public OperandId {
 public:
  StringOperandId() = default;
  explicit StringOperandId(uint16_t id) : OperandId(id) {}
};

class SymbolOperandId : public OperandId {
 public:
  SymbolOperandId() = default;
  explicit SymbolOperandId(uint16_t id) : OperandId(id) {}
};

class BigIntOperandId : public OperandId {
 public:
  BigIntOperandId() = default;
  explicit BigIntOperandId(uint16_t id) : OperandId(id) {}
};

class BooleanOperandId : public OperandId {
 public:
  BooleanOperandId() = default;
  explicit BooleanOperandId(uint16_t id) : OperandId(id) {}
};

class Int32OperandId : public OperandId {
 public:
  Int32OperandId() = default;
  explicit Int32OperandId(uint16_t id) : OperandId(id) {}
};

class TypedOperandId : public OperandId {
  JSValueType type_;

 public:
  MOZ_IMPLICIT TypedOperandId(ObjOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_OBJECT) {}
  MOZ_IMPLICIT TypedOperandId(StringOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_STRING) {}
  MOZ_IMPLICIT TypedOperandId(SymbolOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_SYMBOL) {}
  MOZ_IMPLICIT TypedOperandId(BigIntOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_BIGINT) {}
  MOZ_IMPLICIT TypedOperandId(BooleanOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_BOOLEAN) {}
  MOZ_IMPLICIT TypedOperandId(Int32OperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_INT32) {}

  MOZ_IMPLICIT TypedOperandId(ValueTagOperandId val)
      : OperandId(val.id()), type_(JSVAL_TYPE_UNKNOWN) {}
  MOZ_IMPLICIT TypedOperandId(IntPtrOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_UNKNOWN) {}

  TypedOperandId(ValOperandId val, JSValueType type)
      : OperandId(val.id()), type_(type) {}

  JSValueType type() const { return type_; }
};

#define CACHE_IR_KINDS(_) \
  _(GetProp)              \
  _(GetElem)              \
  _(GetName)              \
  _(GetPropSuper)         \
  _(GetElemSuper)         \
  _(GetImport)            \
  _(LazyConstant)         \
  _(SetProp)              \
  _(SetElem)              \
  _(BindName)             \
  _(In)                   \
  _(HasOwn)               \
  _(CheckPrivateField)    \
  _(TypeOf)               \
  _(TypeOfEq)             \
  _(ToPropertyKey)        \
  _(InstanceOf)           \
  _(GetIterator)          \
  _(CloseIter)            \
  _(OptimizeGetIterator)  \
  _(OptimizeSpreadCall)   \
  _(Compare)              \
  _(ToBool)               \
  _(Call)                 \
  _(UnaryArith)           \
  _(BinaryArith)          \
  _(NewObject)            \
  _(NewArray)             \
  _(Lambda)

enum class CacheKind : uint8_t {
#define DEFINE_KIND(kind) kind,
  CACHE_IR_KINDS(DEFINE_KIND)
#undef DEFINE_KIND
};

extern const char* const CacheKindNames[];

extern size_t NumInputsForCacheKind(CacheKind kind);

enum class CacheOp : uint16_t {
#define DEFINE_OP(op, ...) op,
  CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP
      NumOpcodes,
};

// CacheIR opcode info that's read in performance-sensitive code. Stored as a
// single byte per op for better cache locality.
struct CacheIROpInfo {
  uint8_t argLength : 7;
  bool transpile : 1;
};
static_assert(sizeof(CacheIROpInfo) == 1);
extern const CacheIROpInfo CacheIROpInfos[];

extern const char* const CacheIROpNames[];

inline const char* CacheIRCodeName(CacheOp op) {
  return CacheIROpNames[static_cast<size_t>(op)];
}

extern const uint32_t CacheIROpHealth[];

class StubField {
 public:
  enum class Type : uint8_t {
    // These fields take up a single word.
    RawInt32,
    RawPointer,
    Shape,
    WeakShape,
    WeakGetterSetter,
    JSObject,
    WeakObject,
    Symbol,
    String,
    WeakBaseScript,
    JitCode,

    Id,
    AllocSite,

    // These fields take up 64 bits on all platforms.
    RawInt64,
    First64BitType = RawInt64,
    Value,
    Double,

    Limit
  };

  static bool sizeIsWord(Type type) {
    MOZ_ASSERT(type != Type::Limit);
    return type < Type::First64BitType;
  }

  static bool sizeIsInt64(Type type) {
    MOZ_ASSERT(type != Type::Limit);
    return type >= Type::First64BitType;
  }

  static size_t sizeInBytes(Type type) {
    if (sizeIsWord(type)) {
      return sizeof(uintptr_t);
    }
    MOZ_ASSERT(sizeIsInt64(type));
    return sizeof(int64_t);
  }

 private:
  uint64_t data_;
  Type type_;

 public:
  StubField(uint64_t data, Type type) : data_(data), type_(type) {
    MOZ_ASSERT_IF(sizeIsWord(), data <= UINTPTR_MAX);
  }

  Type type() const { return type_; }

  bool sizeIsWord() const { return sizeIsWord(type_); }
  bool sizeIsInt64() const { return sizeIsInt64(type_); }

  size_t sizeInBytes() const { return sizeInBytes(type_); }

  uintptr_t asWord() const {
    MOZ_ASSERT(sizeIsWord());
    return uintptr_t(data_);
  }
  uint64_t asInt64() const {
    MOZ_ASSERT(sizeIsInt64());
    return data_;
  }
  uint64_t rawData() const { return data_; }
} JS_HAZ_GC_POINTER;

inline const char* StubFieldTypeName(StubField::Type ty) {
  switch (ty) {
    case StubField::Type::RawInt32:
      return "RawInt32";
    case StubField::Type::RawPointer:
      return "RawPointer";
    case StubField::Type::Shape:
      return "Shape";
    case StubField::Type::WeakShape:
      return "WeakShape";
    case StubField::Type::WeakGetterSetter:
      return "WeakGetterSetter";
    case StubField::Type::JSObject:
      return "JSObject";
    case StubField::Type::WeakObject:
      return "WeakObject";
    case StubField::Type::Symbol:
      return "Symbol";
    case StubField::Type::String:
      return "String";
    case StubField::Type::WeakBaseScript:
      return "WeakBaseScript";
    case StubField::Type::JitCode:
      return "JitCode";
    case StubField::Type::Id:
      return "Id";
    case StubField::Type::AllocSite:
      return "AllocSite";
    case StubField::Type::RawInt64:
      return "RawInt64";
    case StubField::Type::Value:
      return "Value";
    case StubField::Type::Double:
      return "Double";
    case StubField::Type::Limit:
      return "Limit";
  }
  MOZ_CRASH("Unknown StubField::Type");
}

// This class is used to wrap up information about a call to make it
// easier to convey from one function to another. (In particular,
// CacheIRWriter encodes the CallFlags in CacheIR, and CacheIRReader
// decodes them and uses them for compilation.)
class CallFlags {
 public:
  enum ArgFormat : uint8_t {
    Unknown,
    Standard,
    Spread,
    FunCall,
    FunApplyArgsObj,
    FunApplyArray,
    FunApplyNullUndefined,
    LastArgFormat = FunApplyNullUndefined
  };

  CallFlags() = default;
  explicit CallFlags(ArgFormat format) : argFormat_(format) {}
  CallFlags(ArgFormat format, bool isConstructing, bool isSameRealm,
            bool needsUninitializedThis)
      : argFormat_(format),
        isConstructing_(isConstructing),
        isSameRealm_(isSameRealm),
        needsUninitializedThis_(needsUninitializedThis) {}
  CallFlags(bool isConstructing, bool isSpread, bool isSameRealm = false,
            bool needsUninitializedThis = false)
      : argFormat_(isSpread ? Spread : Standard),
        isConstructing_(isConstructing),
        isSameRealm_(isSameRealm),
        needsUninitializedThis_(needsUninitializedThis) {}

  ArgFormat getArgFormat() const { return argFormat_; }
  bool isConstructing() const {
    MOZ_ASSERT_IF(isConstructing_,
                  argFormat_ == Standard || argFormat_ == Spread);
    return isConstructing_;
  }
  bool isSameRealm() const { return isSameRealm_; }
  void setIsSameRealm() { isSameRealm_ = true; }

  bool needsUninitializedThis() const { return needsUninitializedThis_; }
  void setNeedsUninitializedThis() { needsUninitializedThis_ = true; }

  uint8_t toByte() const {
    // See CacheIRReader::callFlags()
    MOZ_ASSERT(argFormat_ != ArgFormat::Unknown);
    uint8_t value = getArgFormat();
    if (isConstructing()) {
      value |= CallFlags::IsConstructing;
    }
    if (isSameRealm()) {
      value |= CallFlags::IsSameRealm;
    }
    if (needsUninitializedThis()) {
      value |= CallFlags::NeedsUninitializedThis;
    }
    return value;
  }

 private:
  ArgFormat argFormat_ = ArgFormat::Unknown;
  bool isConstructing_ = false;
  bool isSameRealm_ = false;
  bool needsUninitializedThis_ = false;

  // Used for encoding/decoding
  static const uint8_t ArgFormatBits = 4;
  static const uint8_t ArgFormatMask = (1 << ArgFormatBits) - 1;
  static_assert(LastArgFormat <= ArgFormatMask, "Not enough arg format bits");
  static const uint8_t IsConstructing = 1 << 5;
  static const uint8_t IsSameRealm = 1 << 6;
  static const uint8_t NeedsUninitializedThis = 1 << 7;

  friend class CacheIRReader;
  friend class CacheIRWriter;
};

// In baseline, we have to copy args onto the stack. Below this threshold, we
// will unroll the arg copy loop. We need to clamp this before providing it as
// an arg to a CacheIR op so that everything 5 or greater can share an IC.
const uint32_t MaxUnrolledArgCopy = 5;
inline uint32_t ClampFixedArgc(uint32_t argc) {
  return std::min(argc, MaxUnrolledArgCopy);
}

enum class AttachDecision {
  // We cannot attach a stub.
  NoAction,

  // We can attach a stub.
  Attach,

  // We cannot currently attach a stub, but we expect to be able to do so in the
  // future. In this case, we do not call trackNotAttached().
  TemporarilyUnoptimizable,

  // We want to attach a stub, but the result of the operation is
  // needed to generate that stub. For example, AddSlot needs to know
  // the resulting shape. Note: the attached stub will inspect the
  // inputs to the operation, so most input checks should be done
  // before the actual operation, with only minimal checks remaining
  // for the deferred portion. This prevents arbitrary scripted code
  // run by the operation from interfering with the conditions being
  // checked.
  Deferred
};

// If the input expression evaluates to an AttachDecision other than NoAction,
// return that AttachDecision. If it is NoAction, do nothing.
#define TRY_ATTACH(expr)                                    \
  do {                                                      \
    AttachDecision tryAttachTempResult_ = expr;             \
    if (tryAttachTempResult_ != AttachDecision::NoAction) { \
      return tryAttachTempResult_;                          \
    }                                                       \
  } while (0)

// Set of arguments supported by GetIndexOfArgument.
// Support for higher argument indices can be added easily, but is currently
// unneeded.
enum class ArgumentKind : uint8_t {
  Callee,
  This,
  NewTarget,
  Arg0,
  Arg1,
  Arg2,
  Arg3,
  Arg4,
  Arg5,
  Arg6,
  Arg7,
  NumKinds
};

const uint8_t ArgumentKindArgIndexLimit =
    uint8_t(ArgumentKind::NumKinds) - uint8_t(ArgumentKind::Arg0);

inline ArgumentKind ArgumentKindForArgIndex(uint32_t idx) {
  MOZ_ASSERT(idx < ArgumentKindArgIndexLimit);
  return ArgumentKind(uint32_t(ArgumentKind::Arg0) + idx);
}

// This function calculates the index of an argument based on the call flags.
// addArgc is an out-parameter, indicating whether the value of argc should
// be added to the return value to find the actual index.
inline int32_t GetIndexOfArgument(ArgumentKind kind, CallFlags flags,
                                  bool* addArgc) {
  // *** STACK LAYOUT (bottom to top) ***        ******** INDEX ********
  //   Callee                                <-- argc+1 + isConstructing
  //   ThisValue                             <-- argc   + isConstructing
  //   Args: | Arg0 |        |  ArgArray  |  <-- argc-1 + isConstructing
  //         | Arg1 | --or-- |            |  <-- argc-2 + isConstructing
  //         | ...  |        | (if spread |  <-- ...
  //         | ArgN |        |  call)     |  <-- 0      + isConstructing
  //   NewTarget (only if constructing)      <-- 0 (if it exists)
  //
  // If this is a spread call, then argc is always 1, and we can calculate the
  // index directly. If this is not a spread call, then the index of any
  // argument other than NewTarget depends on argc.

  // First we determine whether the caller needs to add argc.
  switch (flags.getArgFormat()) {
    case CallFlags::Standard:
      *addArgc = true;
      break;
    case CallFlags::Spread:
      // Spread calls do not have Arg1 or higher.
      MOZ_ASSERT(kind <= ArgumentKind::Arg0);
      *addArgc = false;
      break;
    case CallFlags::Unknown:
    case CallFlags::FunCall:
    case CallFlags::FunApplyArgsObj:
    case CallFlags::FunApplyArray:
    case CallFlags::FunApplyNullUndefined:
      MOZ_CRASH("Currently unreachable");
      break;
  }

  // Second, we determine the offset relative to argc.
  bool hasArgumentArray = !*addArgc;
  switch (kind) {
    case ArgumentKind::Callee:
      return flags.isConstructing() + hasArgumentArray + 1;
    case ArgumentKind::This:
      return flags.isConstructing() + hasArgumentArray;
    case ArgumentKind::Arg0:
      return flags.isConstructing() + hasArgumentArray - 1;
    case ArgumentKind::Arg1:
      return flags.isConstructing() + hasArgumentArray - 2;
    case ArgumentKind::Arg2:
      return flags.isConstructing() + hasArgumentArray - 3;
    case ArgumentKind::Arg3:
      return flags.isConstructing() + hasArgumentArray - 4;
    case ArgumentKind::Arg4:
      return flags.isConstructing() + hasArgumentArray - 5;
    case ArgumentKind::Arg5:
      return flags.isConstructing() + hasArgumentArray - 6;
    case ArgumentKind::Arg6:
      return flags.isConstructing() + hasArgumentArray - 7;
    case ArgumentKind::Arg7:
      return flags.isConstructing() + hasArgumentArray - 8;
    case ArgumentKind::NewTarget:
      MOZ_ASSERT(flags.isConstructing());
      *addArgc = false;
      return 0;
    default:
      MOZ_CRASH("Invalid argument kind");
  }
}

// We use this enum as GuardClass operand, instead of storing Class* pointers
// in the IR, to keep the IR compact and the same size on all platforms.
enum class GuardClassKind : uint8_t {
  Array,
  PlainObject,
  FixedLengthArrayBuffer,
  ResizableArrayBuffer,
  FixedLengthSharedArrayBuffer,
  GrowableSharedArrayBuffer,
  FixedLengthDataView,
  ResizableDataView,
  MappedArguments,
  UnmappedArguments,
  WindowProxy,
  JSFunction,
  BoundFunction,
  Set,
  Map,
  Date,
};

const JSClass* ClassFor(GuardClassKind kind);

enum class ArrayBufferViewKind : uint8_t {
  FixedLength,
  Resizable,
};

inline const char* GuardClassKindEnumName(GuardClassKind kind) {
  switch (kind) {
    case GuardClassKind::Array:
      return "Array";
    case GuardClassKind::PlainObject:
      return "PlainObject";
    case GuardClassKind::FixedLengthArrayBuffer:
      return "FixedLengthArrayBuffer";
    case GuardClassKind::ResizableArrayBuffer:
      return "ResizableArrayBuffer";
    case GuardClassKind::FixedLengthSharedArrayBuffer:
      return "FixedLengthSharedArrayBuffer";
    case GuardClassKind::GrowableSharedArrayBuffer:
      return "GrowableSharedArrayBuffer";
    case GuardClassKind::FixedLengthDataView:
      return "FixedLengthDataView";
    case GuardClassKind::ResizableDataView:
      return "ResizableDataView";
    case GuardClassKind::MappedArguments:
      return "MappedArguments";
    case GuardClassKind::UnmappedArguments:
      return "UnmappedArguments";
    case GuardClassKind::WindowProxy:
      return "WindowProxy";
    case GuardClassKind::JSFunction:
      return "JSFunction";
    case GuardClassKind::BoundFunction:
      return "BoundFunction";
    case GuardClassKind::Set:
      return "Set";
    case GuardClassKind::Map:
      return "Map";
    case GuardClassKind::Date:
      return "Date";
  }
  MOZ_CRASH("Unknown GuardClassKind");
}

}  // namespace jit
}  // namespace js

#endif /* jit_CacheIR_h */
