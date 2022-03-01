/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIR_h
#define jit_CacheIR_h

#include "mozilla/Maybe.h"

#include "NamespaceImports.h"

#include "gc/Rooting.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CompactBuffer.h"
#include "jit/ICState.h"
#include "jit/Simulator.h"
#include "jit/TypeData.h"
#include "js/experimental/JitInfo.h"
#include "js/friend/XrayJitInfo.h"  // JS::XrayJitInfo
#include "js/ScalarType.h"          // js::Scalar::Type
#include "vm/JSFunction.h"
#include "vm/Shape.h"
#include "wasm/WasmTypes.h"

enum class JSOp : uint8_t;

namespace js {

class ProxyObject;
enum class ReferenceType;
enum class UnaryMathFunction : uint8_t;

namespace jit {

enum class BaselineCacheIRStubKind;
enum class InlinableNative : uint16_t;

class BaselineFrame;
class ICCacheIRStub;
class ICScript;
class ICStubSpace;
class Label;
class MacroAssembler;
struct Register;

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
  _(GetIntrinsic)         \
  _(SetProp)              \
  _(SetElem)              \
  _(BindName)             \
  _(In)                   \
  _(HasOwn)               \
  _(CheckPrivateField)    \
  _(TypeOf)               \
  _(ToPropertyKey)        \
  _(InstanceOf)           \
  _(GetIterator)          \
  _(OptimizeSpreadCall)   \
  _(Compare)              \
  _(ToBool)               \
  _(Call)                 \
  _(UnaryArith)           \
  _(BinaryArith)          \
  _(NewObject)            \
  _(NewArray)

enum class CacheKind : uint8_t {
#define DEFINE_KIND(kind) kind,
  CACHE_IR_KINDS(DEFINE_KIND)
#undef DEFINE_KIND
};

extern const char* const CacheKindNames[];

#ifdef DEBUG
extern size_t NumInputsForCacheKind(CacheKind kind);
#endif

enum class CacheOp {
#define DEFINE_OP(op, ...) op,
  CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP
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
extern const uint32_t CacheIROpHealth[];

class StubField {
 public:
  enum class Type : uint8_t {
    // These fields take up a single word.
    RawInt32,
    RawPointer,
    Shape,
    GetterSetter,
    JSObject,
    Symbol,
    String,
    BaseScript,
    Id,
    AllocSite,

    // These fields take up 64 bits on all platforms.
    RawInt64,
    First64BitType = RawInt64,
    Value,

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
} JS_HAZ_GC_POINTER;

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
    LastArgFormat = FunApplyArray
  };

  CallFlags() = default;
  explicit CallFlags(ArgFormat format) : argFormat_(format) {}
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
  ArrayBuffer,
  SharedArrayBuffer,
  DataView,
  MappedArguments,
  UnmappedArguments,
  WindowProxy,
  JSFunction,
};

// Some ops refer to shapes that might be in other zones. Instead of putting
// cross-zone pointers in the caches themselves (which would complicate tracing
// enormously), these ops instead contain wrappers for objects in the target
// zone, which refer to the actual shape via a reserved slot.
JSObject* NewWrapperWithObjectShape(JSContext* cx, HandleNativeObject obj);

void LoadShapeWrapperContents(MacroAssembler& masm, Register obj, Register dst,
                              Label* failure);

#ifdef JS_SIMULATOR
bool CallAnyNative(JSContext* cx, unsigned argc, Value* vp);
#endif

// Class to record CacheIR + some additional metadata for code generation.
class MOZ_RAII CacheIRWriter : public JS::CustomAutoRooter {
#ifdef DEBUG
  JSContext* cx_;
#endif
  CompactBufferWriter buffer_;

  uint32_t nextOperandId_;
  uint32_t nextInstructionId_;
  uint32_t numInputOperands_;

  TypeData typeData_;

  // The data (shapes, slot offsets, etc.) that will be stored in the ICStub.
  Vector<StubField, 8, SystemAllocPolicy> stubFields_;
  size_t stubDataSize_;

  // For each operand id, record which instruction accessed it last. This
  // information greatly improves register allocation.
  Vector<uint32_t, 8, SystemAllocPolicy> operandLastUsed_;

  // OperandId and stub offsets are stored in a single byte, so make sure
  // this doesn't overflow. We use a very conservative limit for now.
  static const size_t MaxOperandIds = 20;
  static const size_t MaxStubDataSizeInBytes = 20 * sizeof(uintptr_t);
  bool tooLarge_;

  // Assume this stub can't be trial inlined until we see a scripted call/inline
  // instruction.
  TrialInliningState trialInliningState_ = TrialInliningState::Failure;

  // Basic caching to avoid quadatic lookup behaviour in readStubFieldForIon.
  mutable uint32_t lastOffset_;
  mutable uint32_t lastIndex_;

#ifdef DEBUG
  // Information for assertLengthMatches.
  mozilla::Maybe<CacheOp> currentOp_;
  size_t currentOpArgsStart_ = 0;
#endif

#ifdef DEBUG
  void assertSameCompartment(JSObject* obj);
  void assertSameZone(Shape* shape);
#else
  void assertSameCompartment(JSObject* obj) {}
  void assertSameZone(Shape* shape) {}
#endif

  void writeOp(CacheOp op) {
    buffer_.writeUnsigned15Bit(uint32_t(op));
    nextInstructionId_++;
#ifdef DEBUG
    MOZ_ASSERT(currentOp_.isNothing(), "Missing call to assertLengthMatches?");
    currentOp_.emplace(op);
    currentOpArgsStart_ = buffer_.length();
#endif
  }

  void assertLengthMatches() {
#ifdef DEBUG
    // After writing arguments, assert the length matches CacheIROpArgLengths.
    size_t expectedLen = CacheIROpInfos[size_t(*currentOp_)].argLength;
    MOZ_ASSERT_IF(!failed(),
                  buffer_.length() - currentOpArgsStart_ == expectedLen);
    currentOp_.reset();
#endif
  }

  void writeOperandId(OperandId opId) {
    if (opId.id() < MaxOperandIds) {
      static_assert(MaxOperandIds <= UINT8_MAX,
                    "operand id must fit in a single byte");
      buffer_.writeByte(opId.id());
    } else {
      tooLarge_ = true;
      return;
    }
    if (opId.id() >= operandLastUsed_.length()) {
      buffer_.propagateOOM(operandLastUsed_.resize(opId.id() + 1));
      if (buffer_.oom()) {
        return;
      }
    }
    MOZ_ASSERT(nextInstructionId_ > 0);
    operandLastUsed_[opId.id()] = nextInstructionId_ - 1;
  }

  void writeCallFlagsImm(CallFlags flags) { buffer_.writeByte(flags.toByte()); }

  void addStubField(uint64_t value, StubField::Type fieldType) {
    size_t fieldOffset = stubDataSize_;
#ifndef JS_64BIT
    // On 32-bit platforms there are two stub field sizes (4 bytes and 8 bytes).
    // Ensure 8-byte fields are properly aligned.
    if (StubField::sizeIsInt64(fieldType)) {
      fieldOffset = AlignBytes(fieldOffset, sizeof(uint64_t));
    }
#endif
    MOZ_ASSERT((fieldOffset % StubField::sizeInBytes(fieldType)) == 0);

    size_t newStubDataSize = fieldOffset + StubField::sizeInBytes(fieldType);
    if (newStubDataSize < MaxStubDataSizeInBytes) {
#ifndef JS_64BIT
      // Add a RawInt32 stub field for padding if necessary, because when we
      // iterate over the stub fields we assume there are no 'holes'.
      if (fieldOffset != stubDataSize_) {
        MOZ_ASSERT((stubDataSize_ + sizeof(uintptr_t)) == fieldOffset);
        buffer_.propagateOOM(
            stubFields_.append(StubField(0, StubField::Type::RawInt32)));
      }
#endif
      buffer_.propagateOOM(stubFields_.append(StubField(value, fieldType)));
      MOZ_ASSERT((fieldOffset % sizeof(uintptr_t)) == 0);
      buffer_.writeByte(fieldOffset / sizeof(uintptr_t));
      stubDataSize_ = newStubDataSize;
    } else {
      tooLarge_ = true;
    }
  }

  void writeShapeField(Shape* shape) {
    MOZ_ASSERT(shape);
    assertSameZone(shape);
    addStubField(uintptr_t(shape), StubField::Type::Shape);
  }
  void writeGetterSetterField(GetterSetter* gs) {
    MOZ_ASSERT(gs);
    addStubField(uintptr_t(gs), StubField::Type::GetterSetter);
  }
  void writeObjectField(JSObject* obj) {
    MOZ_ASSERT(obj);
    assertSameCompartment(obj);
    addStubField(uintptr_t(obj), StubField::Type::JSObject);
  }
  void writeStringField(JSString* str) {
    MOZ_ASSERT(str);
    addStubField(uintptr_t(str), StubField::Type::String);
  }
  void writeSymbolField(JS::Symbol* sym) {
    MOZ_ASSERT(sym);
    addStubField(uintptr_t(sym), StubField::Type::Symbol);
  }
  void writeBaseScriptField(BaseScript* script) {
    MOZ_ASSERT(script);
    addStubField(uintptr_t(script), StubField::Type::BaseScript);
  }
  void writeRawInt32Field(uint32_t val) {
    addStubField(val, StubField::Type::RawInt32);
  }
  void writeRawPointerField(const void* ptr) {
    addStubField(uintptr_t(ptr), StubField::Type::RawPointer);
  }
  void writeIdField(jsid id) {
    addStubField(uintptr_t(JSID_BITS(id)), StubField::Type::Id);
  }
  void writeValueField(const Value& val) {
    addStubField(val.asRawBits(), StubField::Type::Value);
  }
  void writeRawInt64Field(uint64_t val) {
    addStubField(val, StubField::Type::RawInt64);
  }
  void writeAllocSiteField(gc::AllocSite* ptr) {
    addStubField(uintptr_t(ptr), StubField::Type::AllocSite);
  }

  void writeJSOpImm(JSOp op) {
    static_assert(sizeof(JSOp) == sizeof(uint8_t), "JSOp must fit in a byte");
    buffer_.writeByte(uint8_t(op));
  }
  void writeGuardClassKindImm(GuardClassKind kind) {
    static_assert(sizeof(GuardClassKind) == sizeof(uint8_t),
                  "GuardClassKind must fit in a byte");
    buffer_.writeByte(uint8_t(kind));
  }
  void writeValueTypeImm(ValueType type) {
    static_assert(sizeof(ValueType) == sizeof(uint8_t),
                  "ValueType must fit in uint8_t");
    buffer_.writeByte(uint8_t(type));
  }
  void writeJSWhyMagicImm(JSWhyMagic whyMagic) {
    static_assert(JS_WHY_MAGIC_COUNT <= UINT8_MAX,
                  "JSWhyMagic must fit in uint8_t");
    buffer_.writeByte(uint8_t(whyMagic));
  }
  void writeScalarTypeImm(Scalar::Type type) {
    MOZ_ASSERT(size_t(type) <= UINT8_MAX);
    buffer_.writeByte(uint8_t(type));
  }
  void writeUnaryMathFunctionImm(UnaryMathFunction fun) {
    static_assert(sizeof(UnaryMathFunction) == sizeof(uint8_t),
                  "UnaryMathFunction must fit in a byte");
    buffer_.writeByte(uint8_t(fun));
  }
  void writeBoolImm(bool b) { buffer_.writeByte(uint32_t(b)); }

  void writeByteImm(uint32_t b) {
    MOZ_ASSERT(b <= UINT8_MAX);
    buffer_.writeByte(b);
  }

  void writeInt32Imm(int32_t i32) { buffer_.writeFixedUint32_t(i32); }
  void writeUInt32Imm(uint32_t u32) { buffer_.writeFixedUint32_t(u32); }
  void writePointer(const void* ptr) { buffer_.writeRawPointer(ptr); }

  void writeJSNativeImm(JSNative native) {
    writePointer(JS_FUNC_TO_DATA_PTR(void*, native));
  }
  void writeStaticStringImm(const char* str) { writePointer(str); }

  void writeWasmValTypeImm(wasm::ValType::Kind kind) {
    static_assert(unsigned(wasm::TypeCode::Limit) <= UINT8_MAX);
    buffer_.writeByte(uint8_t(kind));
  }

  void writeAllocKindImm(gc::AllocKind kind) {
    static_assert(unsigned(gc::AllocKind::LIMIT) <= UINT8_MAX);
    buffer_.writeByte(uint8_t(kind));
  }

  uint32_t newOperandId() { return nextOperandId_++; }

  CacheIRWriter(const CacheIRWriter&) = delete;
  CacheIRWriter& operator=(const CacheIRWriter&) = delete;

 public:
  explicit CacheIRWriter(JSContext* cx)
      : CustomAutoRooter(cx),
#ifdef DEBUG
        cx_(cx),
#endif
        nextOperandId_(0),
        nextInstructionId_(0),
        numInputOperands_(0),
        stubDataSize_(0),
        tooLarge_(false),
        lastOffset_(0),
        lastIndex_(0) {
  }

  bool failed() const { return buffer_.oom() || tooLarge_; }

  TrialInliningState trialInliningState() const { return trialInliningState_; }

  uint32_t numInputOperands() const { return numInputOperands_; }
  uint32_t numOperandIds() const { return nextOperandId_; }
  uint32_t numInstructions() const { return nextInstructionId_; }

  size_t numStubFields() const { return stubFields_.length(); }
  StubField::Type stubFieldType(uint32_t i) const {
    return stubFields_[i].type();
  }

  uint32_t setInputOperandId(uint32_t op) {
    MOZ_ASSERT(op == nextOperandId_);
    nextOperandId_++;
    numInputOperands_++;
    return op;
  }

  TypeData typeData() const { return typeData_; }
  void setTypeData(TypeData data) { typeData_ = data; }

  void trace(JSTracer* trc) override {
    // For now, assert we only GC before we append stub fields.
    MOZ_RELEASE_ASSERT(stubFields_.empty());
  }

  size_t stubDataSize() const { return stubDataSize_; }
  void copyStubData(uint8_t* dest) const;
  bool stubDataEquals(const uint8_t* stubData) const;

  bool operandIsDead(uint32_t operandId, uint32_t currentInstruction) const {
    if (operandId >= operandLastUsed_.length()) {
      return false;
    }
    return currentInstruction > operandLastUsed_[operandId];
  }

  const uint8_t* codeStart() const {
    MOZ_ASSERT(!failed());
    return buffer_.buffer();
  }

  const uint8_t* codeEnd() const {
    MOZ_ASSERT(!failed());
    return buffer_.buffer() + buffer_.length();
  }

  uint32_t codeLength() const {
    MOZ_ASSERT(!failed());
    return buffer_.length();
  }

  // This should not be used when compiling Baseline code, as Baseline code
  // shouldn't bake in stub values.
  StubField readStubFieldForIon(uint32_t offset, StubField::Type type) const;

  ObjOperandId guardToObject(ValOperandId input) {
    guardToObject_(input);
    return ObjOperandId(input.id());
  }

  StringOperandId guardToString(ValOperandId input) {
    guardToString_(input);
    return StringOperandId(input.id());
  }

  SymbolOperandId guardToSymbol(ValOperandId input) {
    guardToSymbol_(input);
    return SymbolOperandId(input.id());
  }

  BigIntOperandId guardToBigInt(ValOperandId input) {
    guardToBigInt_(input);
    return BigIntOperandId(input.id());
  }

  BooleanOperandId guardToBoolean(ValOperandId input) {
    guardToBoolean_(input);
    return BooleanOperandId(input.id());
  }

  Int32OperandId guardToInt32(ValOperandId input) {
    guardToInt32_(input);
    return Int32OperandId(input.id());
  }

  NumberOperandId guardIsNumber(ValOperandId input) {
    guardIsNumber_(input);
    return NumberOperandId(input.id());
  }

  ValOperandId boxObject(ObjOperandId input) {
    return ValOperandId(input.id());
  }

  void guardShapeForClass(ObjOperandId obj, Shape* shape) {
    // Guard shape to ensure that object class is unchanged. This is true
    // for all shapes.
    guardShape(obj, shape);
  }

  void guardShapeForOwnProperties(ObjOperandId obj, Shape* shape) {
    // Guard shape to detect changes to (non-dense) own properties. This
    // also implies |guardShapeForClass|.
    MOZ_ASSERT(shape->getObjectClass()->isNativeObject());
    guardShape(obj, shape);
  }

 public:
  static uint32_t encodeNargsAndFlags(JSFunction* fun) {
    static_assert(JSFunction::NArgsBits == 16);
    static_assert(sizeof(decltype(fun->flags().toRaw())) == sizeof(uint16_t));
    return (uint32_t(fun->nargs()) << 16) | fun->flags().toRaw();
  }

  void guardSpecificFunction(ObjOperandId obj, JSFunction* expected) {
    // Guard object is a specific function. This implies immutable fields on
    // the JSFunction struct itself are unchanged.
    // Bake in the nargs and FunctionFlags so Warp can use them off-main thread,
    // instead of directly using the JSFunction fields.
    uint32_t nargsAndFlags = encodeNargsAndFlags(expected);
    guardSpecificFunction_(obj, expected, nargsAndFlags);
  }

  void guardFunctionScript(ObjOperandId fun, BaseScript* expected) {
    // Guard function has a specific BaseScript. This implies immutable fields
    // on the JSFunction struct itself are unchanged and are equivalent for
    // lambda clones.
    // Bake in the nargs and FunctionFlags so Warp can use them off-main thread,
    // instead of directly using the JSFunction fields.
    uint32_t nargsAndFlags = encodeNargsAndFlags(expected->function());
    guardFunctionScript_(fun, expected, nargsAndFlags);
  }

  ValOperandId loadArgumentFixedSlot(
      ArgumentKind kind, uint32_t argc,
      CallFlags flags = CallFlags(CallFlags::Standard)) {
    bool addArgc;
    int32_t slotIndex = GetIndexOfArgument(kind, flags, &addArgc);
    if (addArgc) {
      slotIndex += argc;
    }
    MOZ_ASSERT(slotIndex >= 0);
    MOZ_ASSERT(slotIndex <= UINT8_MAX);
    return loadArgumentFixedSlot_(slotIndex);
  }

  ValOperandId loadStandardCallArgument(uint32_t index, uint32_t argc) {
    int32_t slotIndex = -int32_t(index + 1);
    slotIndex += argc;
    MOZ_ASSERT(slotIndex >= 0);
    MOZ_ASSERT(slotIndex <= UINT8_MAX);
    return loadArgumentFixedSlot_(slotIndex);
  }

  ValOperandId loadArgumentDynamicSlot(
      ArgumentKind kind, Int32OperandId argcId,
      CallFlags flags = CallFlags(CallFlags::Standard)) {
    bool addArgc;
    int32_t slotIndex = GetIndexOfArgument(kind, flags, &addArgc);
    if (addArgc) {
      return loadArgumentDynamicSlot_(argcId, slotIndex);
    }
    return loadArgumentFixedSlot_(slotIndex);
  }

  ObjOperandId loadSpreadArgs() {
    ArgumentKind kind = ArgumentKind::Arg0;
    uint32_t argc = 1;
    CallFlags flags(CallFlags::Spread);
    return ObjOperandId(loadArgumentFixedSlot(kind, argc, flags).id());
  }

  void callScriptedFunction(ObjOperandId callee, Int32OperandId argc,
                            CallFlags flags) {
    callScriptedFunction_(callee, argc, flags);
    trialInliningState_ = TrialInliningState::Candidate;
  }

  void callInlinedFunction(ObjOperandId callee, Int32OperandId argc,
                           ICScript* icScript, CallFlags flags) {
    callInlinedFunction_(callee, argc, icScript, flags);
    trialInliningState_ = TrialInliningState::Inlined;
  }

  void callNativeFunction(ObjOperandId calleeId, Int32OperandId argc, JSOp op,
                          JSFunction* calleeFunc, CallFlags flags) {
    // Some native functions can be implemented faster if we know that
    // the return value is ignored.
    bool ignoresReturnValue =
        op == JSOp::CallIgnoresRv && calleeFunc->hasJitInfo() &&
        calleeFunc->jitInfo()->type() == JSJitInfo::IgnoresReturnValueNative;

#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special
    // swi instruction to handle them, so we store the redirected
    // pointer in the stub and use that instead of the original one.
    // If we are calling the ignoresReturnValue version of a native
    // function, we bake it into the redirected pointer.
    // (See BaselineCacheIRCompiler::emitCallNativeFunction.)
    JSNative target = ignoresReturnValue
                          ? calleeFunc->jitInfo()->ignoresReturnValueMethod
                          : calleeFunc->native();
    void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, target);
    void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
    callNativeFunction_(calleeId, argc, flags, redirected);
#else
    // If we are not running in the simulator, we generate different jitcode
    // to find the ignoresReturnValue version of a native function.
    callNativeFunction_(calleeId, argc, flags, ignoresReturnValue);
#endif
  }

  void callDOMFunction(ObjOperandId calleeId, Int32OperandId argc,
                       ObjOperandId thisObjId, JSFunction* calleeFunc,
                       CallFlags flags) {
#ifdef JS_SIMULATOR
    void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, calleeFunc->native());
    void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
    callDOMFunction_(calleeId, argc, thisObjId, flags, redirected);
#else
    callDOMFunction_(calleeId, argc, thisObjId, flags);
#endif
  }

  void callAnyNativeFunction(ObjOperandId calleeId, Int32OperandId argc,
                             CallFlags flags) {
    MOZ_ASSERT(!flags.isSameRealm());
#ifdef JS_SIMULATOR
    // The simulator requires native calls to be redirected to a
    // special swi instruction. If we are calling an arbitrary native
    // function, we can't wrap the real target ahead of time, so we
    // call a wrapper function (CallAnyNative) that calls the target
    // itself, and redirect that wrapper.
    JSNative target = CallAnyNative;
    void* rawPtr = JS_FUNC_TO_DATA_PTR(void*, target);
    void* redirected = Simulator::RedirectNativeFunction(rawPtr, Args_General3);
    callNativeFunction_(calleeId, argc, flags, redirected);
#else
    callNativeFunction_(calleeId, argc, flags,
                        /* ignoresReturnValue = */ false);
#endif
  }

  void callClassHook(ObjOperandId calleeId, Int32OperandId argc, JSNative hook,
                     CallFlags flags) {
    MOZ_ASSERT(!flags.isSameRealm());
    void* target = JS_FUNC_TO_DATA_PTR(void*, hook);
#ifdef JS_SIMULATOR
    // The simulator requires VM calls to be redirected to a special
    // swi instruction to handle them, so we store the redirected
    // pointer in the stub and use that instead of the original one.
    target = Simulator::RedirectNativeFunction(target, Args_General3);
#endif
    callClassHook_(calleeId, argc, flags, target);
  }

  void callScriptedGetterResult(ValOperandId receiver, JSFunction* getter,
                                bool sameRealm) {
    MOZ_ASSERT(getter->hasJitEntry());
    uint32_t nargsAndFlags = encodeNargsAndFlags(getter);
    callScriptedGetterResult_(receiver, getter, sameRealm, nargsAndFlags);
    trialInliningState_ = TrialInliningState::Candidate;
  }

  void callInlinedGetterResult(ValOperandId receiver, JSFunction* getter,
                               ICScript* icScript, bool sameRealm) {
    MOZ_ASSERT(getter->hasJitEntry());
    uint32_t nargsAndFlags = encodeNargsAndFlags(getter);
    callInlinedGetterResult_(receiver, getter, icScript, sameRealm,
                             nargsAndFlags);
    trialInliningState_ = TrialInliningState::Inlined;
  }

  void callNativeGetterResult(ValOperandId receiver, JSFunction* getter,
                              bool sameRealm) {
    MOZ_ASSERT(getter->isNativeWithoutJitEntry());
    uint32_t nargsAndFlags = encodeNargsAndFlags(getter);
    callNativeGetterResult_(receiver, getter, sameRealm, nargsAndFlags);
  }

  void callScriptedSetter(ObjOperandId receiver, JSFunction* setter,
                          ValOperandId rhs, bool sameRealm) {
    MOZ_ASSERT(setter->hasJitEntry());
    uint32_t nargsAndFlags = encodeNargsAndFlags(setter);
    callScriptedSetter_(receiver, setter, rhs, sameRealm, nargsAndFlags);
    trialInliningState_ = TrialInliningState::Candidate;
  }

  void callInlinedSetter(ObjOperandId receiver, JSFunction* setter,
                         ValOperandId rhs, ICScript* icScript, bool sameRealm) {
    MOZ_ASSERT(setter->hasJitEntry());
    uint32_t nargsAndFlags = encodeNargsAndFlags(setter);
    callInlinedSetter_(receiver, setter, rhs, icScript, sameRealm,
                       nargsAndFlags);
    trialInliningState_ = TrialInliningState::Inlined;
  }

  void callNativeSetter(ObjOperandId receiver, JSFunction* setter,
                        ValOperandId rhs, bool sameRealm) {
    MOZ_ASSERT(setter->isNativeWithoutJitEntry());
    uint32_t nargsAndFlags = encodeNargsAndFlags(setter);
    callNativeSetter_(receiver, setter, rhs, sameRealm, nargsAndFlags);
  }

  void metaScriptedTemplateObject(JSFunction* callee,
                                  JSObject* templateObject) {
    metaTwoByte_(callee, templateObject);
  }
  friend class CacheIRCloner;

  CACHE_IR_WRITER_GENERATED
};

class CacheIRStubInfo;

// Helper class for reading CacheIR bytecode.
class MOZ_RAII CacheIRReader {
  CompactBufferReader buffer_;

  CacheIRReader(const CacheIRReader&) = delete;
  CacheIRReader& operator=(const CacheIRReader&) = delete;

 public:
  CacheIRReader(const uint8_t* start, const uint8_t* end)
      : buffer_(start, end) {}
  explicit CacheIRReader(const CacheIRWriter& writer)
      : CacheIRReader(writer.codeStart(), writer.codeEnd()) {}
  explicit CacheIRReader(const CacheIRStubInfo* stubInfo);

  bool more() const { return buffer_.more(); }

  CacheOp readOp() { return CacheOp(buffer_.readUnsigned15Bit()); }

  // Skip data not currently used.
  void skip() { buffer_.readByte(); }
  void skip(uint32_t skipLength) {
    if (skipLength > 0) {
      buffer_.seek(buffer_.currentPosition(), skipLength);
    }
  }

  ValOperandId valOperandId() { return ValOperandId(buffer_.readByte()); }
  ValueTagOperandId valueTagOperandId() {
    return ValueTagOperandId(buffer_.readByte());
  }

  IntPtrOperandId intPtrOperandId() {
    return IntPtrOperandId(buffer_.readByte());
  }

  ObjOperandId objOperandId() { return ObjOperandId(buffer_.readByte()); }
  NumberOperandId numberOperandId() {
    return NumberOperandId(buffer_.readByte());
  }
  StringOperandId stringOperandId() {
    return StringOperandId(buffer_.readByte());
  }

  SymbolOperandId symbolOperandId() {
    return SymbolOperandId(buffer_.readByte());
  }

  BigIntOperandId bigIntOperandId() {
    return BigIntOperandId(buffer_.readByte());
  }

  BooleanOperandId booleanOperandId() {
    return BooleanOperandId(buffer_.readByte());
  }

  Int32OperandId int32OperandId() { return Int32OperandId(buffer_.readByte()); }

  uint32_t rawOperandId() { return buffer_.readByte(); }

  uint32_t stubOffset() { return buffer_.readByte() * sizeof(uintptr_t); }
  GuardClassKind guardClassKind() { return GuardClassKind(buffer_.readByte()); }
  ValueType valueType() { return ValueType(buffer_.readByte()); }
  wasm::ValType::Kind wasmValType() {
    return wasm::ValType::Kind(buffer_.readByte());
  }
  gc::AllocKind allocKind() { return gc::AllocKind(buffer_.readByte()); }

  Scalar::Type scalarType() { return Scalar::Type(buffer_.readByte()); }
  uint32_t rttValueKey() { return buffer_.readByte(); }
  JSWhyMagic whyMagic() { return JSWhyMagic(buffer_.readByte()); }
  JSOp jsop() { return JSOp(buffer_.readByte()); }
  int32_t int32Immediate() { return int32_t(buffer_.readFixedUint32_t()); }
  uint32_t uint32Immediate() { return buffer_.readFixedUint32_t(); }
  void* pointer() { return buffer_.readRawPointer(); }

  UnaryMathFunction unaryMathFunction() {
    return UnaryMathFunction(buffer_.readByte());
  }

  CallFlags callFlags() {
    // See CacheIRWriter::writeCallFlagsImm()
    uint8_t encoded = buffer_.readByte();
    CallFlags::ArgFormat format =
        CallFlags::ArgFormat(encoded & CallFlags::ArgFormatMask);
    bool isConstructing = encoded & CallFlags::IsConstructing;
    bool isSameRealm = encoded & CallFlags::IsSameRealm;
    bool needsUninitializedThis = encoded & CallFlags::NeedsUninitializedThis;
    MOZ_ASSERT_IF(needsUninitializedThis, isConstructing);
    switch (format) {
      case CallFlags::Unknown:
        MOZ_CRASH("Unexpected call flags");
      case CallFlags::Standard:
        return CallFlags(isConstructing, /*isSpread =*/false, isSameRealm,
                         needsUninitializedThis);
      case CallFlags::Spread:
        return CallFlags(isConstructing, /*isSpread =*/true, isSameRealm,
                         needsUninitializedThis);
      default:
        // The existing non-standard argument formats (FunCall and FunApply)
        // can't be constructors.
        MOZ_ASSERT(!isConstructing);
        return CallFlags(format);
    }
  }

  uint8_t readByte() { return buffer_.readByte(); }
  bool readBool() {
    uint8_t b = buffer_.readByte();
    MOZ_ASSERT(b <= 1);
    return bool(b);
  }

  const uint8_t* currentPosition() const { return buffer_.currentPosition(); }
};

class MOZ_RAII CacheIRCloner {
 public:
  explicit CacheIRCloner(ICCacheIRStub* stubInfo);

  void cloneOp(CacheOp op, CacheIRReader& reader, CacheIRWriter& writer);

  CACHE_IR_CLONE_GENERATED

 private:
  const CacheIRStubInfo* stubInfo_;
  const uint8_t* stubData_;

  uintptr_t readStubWord(uint32_t offset);
  int64_t readStubInt64(uint32_t offset);

  Shape* getShapeField(uint32_t stubOffset);
  GetterSetter* getGetterSetterField(uint32_t stubOffset);
  JSObject* getObjectField(uint32_t stubOffset);
  JSString* getStringField(uint32_t stubOffset);
  JSAtom* getAtomField(uint32_t stubOffset);
  PropertyName* getPropertyNameField(uint32_t stubOffset);
  JS::Symbol* getSymbolField(uint32_t stubOffset);
  BaseScript* getBaseScriptField(uint32_t stubOffset);
  uint32_t getRawInt32Field(uint32_t stubOffset);
  const void* getRawPointerField(uint32_t stubOffset);
  jsid getIdField(uint32_t stubOffset);
  const Value getValueField(uint32_t stubOffset);
  uint64_t getRawInt64Field(uint32_t stubOffset);
  gc::AllocSite* getAllocSiteField(uint32_t stubOffset);
};

class MOZ_RAII IRGenerator {
 protected:
  CacheIRWriter writer;
  JSContext* cx_;
  HandleScript script_;
  jsbytecode* pc_;
  CacheKind cacheKind_;
  ICState::Mode mode_;
  bool isFirstStub_;

  IRGenerator(const IRGenerator&) = delete;
  IRGenerator& operator=(const IRGenerator&) = delete;

  bool maybeGuardInt32Index(const Value& index, ValOperandId indexId,
                            uint32_t* int32Index, Int32OperandId* int32IndexId);

  IntPtrOperandId guardToIntPtrIndex(const Value& index, ValOperandId indexId,
                                     bool supportOOB);

  ObjOperandId guardDOMProxyExpandoObjectAndShape(ProxyObject* obj,
                                                  ObjOperandId objId,
                                                  const Value& expandoVal,
                                                  NativeObject* expandoObj);

  void emitIdGuard(ValOperandId valId, const Value& idVal, jsid id);

  OperandId emitNumericGuard(ValOperandId valId, Scalar::Type type);

  friend class CacheIRSpewer;

 public:
  explicit IRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                       CacheKind cacheKind, ICState state);

  const CacheIRWriter& writerRef() const { return writer; }
  CacheKind cacheKind() const { return cacheKind_; }

  static constexpr char* NotAttached = nullptr;
};

// GetPropIRGenerator generates CacheIR for a GetProp IC.
class MOZ_RAII GetPropIRGenerator : public IRGenerator {
  HandleValue val_;
  HandleValue idVal_;

  AttachDecision tryAttachNative(HandleObject obj, ObjOperandId objId,
                                 HandleId id, ValOperandId receiverId);
  AttachDecision tryAttachObjectLength(HandleObject obj, ObjOperandId objId,
                                       HandleId id);
  AttachDecision tryAttachTypedArray(HandleObject obj, ObjOperandId objId,
                                     HandleId id);
  AttachDecision tryAttachDataView(HandleObject obj, ObjOperandId objId,
                                   HandleId id);
  AttachDecision tryAttachArrayBufferMaybeShared(HandleObject obj,
                                                 ObjOperandId objId,
                                                 HandleId id);
  AttachDecision tryAttachRegExp(HandleObject obj, ObjOperandId objId,
                                 HandleId id);
  AttachDecision tryAttachModuleNamespace(HandleObject obj, ObjOperandId objId,
                                          HandleId id);
  AttachDecision tryAttachWindowProxy(HandleObject obj, ObjOperandId objId,
                                      HandleId id);
  AttachDecision tryAttachCrossCompartmentWrapper(HandleObject obj,
                                                  ObjOperandId objId,
                                                  HandleId id);
  AttachDecision tryAttachXrayCrossCompartmentWrapper(HandleObject obj,
                                                      ObjOperandId objId,
                                                      HandleId id,
                                                      ValOperandId receiverId);
  AttachDecision tryAttachFunction(HandleObject obj, ObjOperandId objId,
                                   HandleId id);
  AttachDecision tryAttachArgumentsObjectIterator(HandleObject obj,
                                                  ObjOperandId objId,
                                                  HandleId id);

  AttachDecision tryAttachGenericProxy(Handle<ProxyObject*> obj,
                                       ObjOperandId objId, HandleId id,
                                       bool handleDOMProxies);
  AttachDecision tryAttachDOMProxyExpando(Handle<ProxyObject*> obj,
                                          ObjOperandId objId, HandleId id,
                                          ValOperandId receiverId);
  AttachDecision tryAttachDOMProxyShadowed(Handle<ProxyObject*> obj,
                                           ObjOperandId objId, HandleId id);
  AttachDecision tryAttachDOMProxyUnshadowed(Handle<ProxyObject*> obj,
                                             ObjOperandId objId, HandleId id,
                                             ValOperandId receiverId);
  AttachDecision tryAttachProxy(HandleObject obj, ObjOperandId objId,
                                HandleId id, ValOperandId receiverId);

  AttachDecision tryAttachPrimitive(ValOperandId valId, HandleId id);
  AttachDecision tryAttachStringChar(ValOperandId valId, ValOperandId indexId);
  AttachDecision tryAttachStringLength(ValOperandId valId, HandleId id);

  AttachDecision tryAttachArgumentsObjectArg(HandleObject obj,
                                             ObjOperandId objId, uint32_t index,
                                             Int32OperandId indexId);
  AttachDecision tryAttachArgumentsObjectCallee(HandleObject obj,
                                                ObjOperandId objId,
                                                HandleId id);

  AttachDecision tryAttachDenseElement(HandleObject obj, ObjOperandId objId,
                                       uint32_t index, Int32OperandId indexId);
  AttachDecision tryAttachDenseElementHole(HandleObject obj, ObjOperandId objId,
                                           uint32_t index,
                                           Int32OperandId indexId);
  AttachDecision tryAttachSparseElement(HandleObject obj, ObjOperandId objId,
                                        uint32_t index, Int32OperandId indexId);
  AttachDecision tryAttachTypedArrayElement(HandleObject obj,
                                            ObjOperandId objId);

  AttachDecision tryAttachGenericElement(HandleObject obj, ObjOperandId objId,
                                         uint32_t index,
                                         Int32OperandId indexId);

  AttachDecision tryAttachProxyElement(HandleObject obj, ObjOperandId objId);

  void attachMegamorphicNativeSlot(ObjOperandId objId, jsid id);

  ValOperandId getElemKeyValueId() const {
    MOZ_ASSERT(cacheKind_ == CacheKind::GetElem ||
               cacheKind_ == CacheKind::GetElemSuper);
    return ValOperandId(1);
  }

  ValOperandId getSuperReceiverValueId() const {
    if (cacheKind_ == CacheKind::GetPropSuper) {
      return ValOperandId(1);
    }

    MOZ_ASSERT(cacheKind_ == CacheKind::GetElemSuper);
    return ValOperandId(2);
  }

  bool isSuper() const {
    return (cacheKind_ == CacheKind::GetPropSuper ||
            cacheKind_ == CacheKind::GetElemSuper);
  }

  // If this is a GetElem cache, emit instructions to guard the incoming Value
  // matches |id|.
  void maybeEmitIdGuard(jsid id);

  void trackAttached(const char* name);

 public:
  GetPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                     ICState state, CacheKind cacheKind, HandleValue val,
                     HandleValue idVal);

  AttachDecision tryAttachStub();
};

// GetNameIRGenerator generates CacheIR for a GetName IC.
class MOZ_RAII GetNameIRGenerator : public IRGenerator {
  HandleObject env_;
  HandlePropertyName name_;

  AttachDecision tryAttachGlobalNameValue(ObjOperandId objId, HandleId id);
  AttachDecision tryAttachGlobalNameGetter(ObjOperandId objId, HandleId id);
  AttachDecision tryAttachEnvironmentName(ObjOperandId objId, HandleId id);

  void trackAttached(const char* name);

 public:
  GetNameIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                     ICState state, HandleObject env, HandlePropertyName name);

  AttachDecision tryAttachStub();
};

// BindNameIRGenerator generates CacheIR for a BindName IC.
class MOZ_RAII BindNameIRGenerator : public IRGenerator {
  HandleObject env_;
  HandlePropertyName name_;

  AttachDecision tryAttachGlobalName(ObjOperandId objId, HandleId id);
  AttachDecision tryAttachEnvironmentName(ObjOperandId objId, HandleId id);

  void trackAttached(const char* name);

 public:
  BindNameIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                      ICState state, HandleObject env, HandlePropertyName name);

  AttachDecision tryAttachStub();
};

// SetPropIRGenerator generates CacheIR for a SetProp IC.
class MOZ_RAII SetPropIRGenerator : public IRGenerator {
  HandleValue lhsVal_;
  HandleValue idVal_;
  HandleValue rhsVal_;

 public:
  enum class DeferType { None, AddSlot };

 private:
  DeferType deferType_ = DeferType::None;

  ValOperandId setElemKeyValueId() const {
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    return ValOperandId(1);
  }

  ValOperandId rhsValueId() const {
    if (cacheKind_ == CacheKind::SetProp) {
      return ValOperandId(1);
    }
    MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
    return ValOperandId(2);
  }

  // If this is a SetElem cache, emit instructions to guard the incoming Value
  // matches |id|.
  void maybeEmitIdGuard(jsid id);

  AttachDecision tryAttachNativeSetSlot(HandleObject obj, ObjOperandId objId,
                                        HandleId id, ValOperandId rhsId);
  AttachDecision tryAttachSetter(HandleObject obj, ObjOperandId objId,
                                 HandleId id, ValOperandId rhsId);
  AttachDecision tryAttachSetArrayLength(HandleObject obj, ObjOperandId objId,
                                         HandleId id, ValOperandId rhsId);
  AttachDecision tryAttachWindowProxy(HandleObject obj, ObjOperandId objId,
                                      HandleId id, ValOperandId rhsId);

  AttachDecision tryAttachSetDenseElement(HandleObject obj, ObjOperandId objId,
                                          uint32_t index,
                                          Int32OperandId indexId,
                                          ValOperandId rhsId);
  AttachDecision tryAttachSetTypedArrayElement(HandleObject obj,
                                               ObjOperandId objId,
                                               ValOperandId rhsId);

  AttachDecision tryAttachSetDenseElementHole(HandleObject obj,
                                              ObjOperandId objId,
                                              uint32_t index,
                                              Int32OperandId indexId,
                                              ValOperandId rhsId);

  AttachDecision tryAttachAddOrUpdateSparseElement(HandleObject obj,
                                                   ObjOperandId objId,
                                                   uint32_t index,
                                                   Int32OperandId indexId,
                                                   ValOperandId rhsId);

  AttachDecision tryAttachGenericProxy(Handle<ProxyObject*> obj,
                                       ObjOperandId objId, HandleId id,
                                       ValOperandId rhsId,
                                       bool handleDOMProxies);
  AttachDecision tryAttachDOMProxyShadowed(Handle<ProxyObject*> obj,
                                           ObjOperandId objId, HandleId id,
                                           ValOperandId rhsId);
  AttachDecision tryAttachDOMProxyUnshadowed(Handle<ProxyObject*> obj,
                                             ObjOperandId objId, HandleId id,
                                             ValOperandId rhsId);
  AttachDecision tryAttachDOMProxyExpando(Handle<ProxyObject*> obj,
                                          ObjOperandId objId, HandleId id,
                                          ValOperandId rhsId);
  AttachDecision tryAttachProxy(HandleObject obj, ObjOperandId objId,
                                HandleId id, ValOperandId rhsId);
  AttachDecision tryAttachProxyElement(HandleObject obj, ObjOperandId objId,
                                       ValOperandId rhsId);
  AttachDecision tryAttachMegamorphicSetElement(HandleObject obj,
                                                ObjOperandId objId,
                                                ValOperandId rhsId);

  bool canAttachAddSlotStub(HandleObject obj, HandleId id);

 public:
  SetPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                     CacheKind cacheKind, ICState state, HandleValue lhsVal,
                     HandleValue idVal, HandleValue rhsVal);

  AttachDecision tryAttachStub();
  AttachDecision tryAttachAddSlotStub(HandleShape oldShape);
  void trackAttached(const char* name);

  DeferType deferType() const { return deferType_; }
};

// HasPropIRGenerator generates CacheIR for a HasProp IC. Used for
// CacheKind::In / CacheKind::HasOwn.
class MOZ_RAII HasPropIRGenerator : public IRGenerator {
  HandleValue val_;
  HandleValue idVal_;

  AttachDecision tryAttachDense(HandleObject obj, ObjOperandId objId,
                                uint32_t index, Int32OperandId indexId);
  AttachDecision tryAttachDenseHole(HandleObject obj, ObjOperandId objId,
                                    uint32_t index, Int32OperandId indexId);
  AttachDecision tryAttachTypedArray(HandleObject obj, ObjOperandId objId,
                                     ValOperandId keyId);
  AttachDecision tryAttachSparse(HandleObject obj, ObjOperandId objId,
                                 Int32OperandId indexId);
  AttachDecision tryAttachNamedProp(HandleObject obj, ObjOperandId objId,
                                    HandleId key, ValOperandId keyId);
  AttachDecision tryAttachMegamorphic(ObjOperandId objId, ValOperandId keyId);
  AttachDecision tryAttachNative(NativeObject* obj, ObjOperandId objId,
                                 jsid key, ValOperandId keyId,
                                 PropertyResult prop, NativeObject* holder);
  AttachDecision tryAttachSlotDoesNotExist(NativeObject* obj,
                                           ObjOperandId objId, jsid key,
                                           ValOperandId keyId);
  AttachDecision tryAttachDoesNotExist(HandleObject obj, ObjOperandId objId,
                                       HandleId key, ValOperandId keyId);
  AttachDecision tryAttachProxyElement(HandleObject obj, ObjOperandId objId,
                                       ValOperandId keyId);

  void trackAttached(const char* name);

 public:
  // NOTE: Argument order is PROPERTY, OBJECT
  HasPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                     ICState state, CacheKind cacheKind, HandleValue idVal,
                     HandleValue val);

  AttachDecision tryAttachStub();
};

class MOZ_RAII CheckPrivateFieldIRGenerator : public IRGenerator {
  HandleValue val_;
  HandleValue idVal_;

  AttachDecision tryAttachNative(JSObject* obj, ObjOperandId objId, jsid key,
                                 ValOperandId keyId, bool hasOwn);

  void trackAttached(const char* name);

 public:
  CheckPrivateFieldIRGenerator(JSContext* cx, HandleScript script,
                               jsbytecode* pc, ICState state,
                               CacheKind cacheKind, HandleValue idVal,
                               HandleValue val);
  AttachDecision tryAttachStub();
};

class MOZ_RAII InstanceOfIRGenerator : public IRGenerator {
  HandleValue lhsVal_;
  HandleObject rhsObj_;

  void trackAttached(const char* name);

 public:
  InstanceOfIRGenerator(JSContext*, HandleScript, jsbytecode*, ICState,
                        HandleValue, HandleObject);

  AttachDecision tryAttachStub();
};

class MOZ_RAII TypeOfIRGenerator : public IRGenerator {
  HandleValue val_;

  AttachDecision tryAttachPrimitive(ValOperandId valId);
  AttachDecision tryAttachObject(ValOperandId valId);
  void trackAttached(const char* name);

 public:
  TypeOfIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState state,
                    HandleValue value);

  AttachDecision tryAttachStub();
};

class MOZ_RAII GetIteratorIRGenerator : public IRGenerator {
  HandleValue val_;

  AttachDecision tryAttachNativeIterator(ObjOperandId objId, HandleObject obj);

 public:
  GetIteratorIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                         ICState state, HandleValue value);

  AttachDecision tryAttachStub();

  void trackAttached(const char* name);
};

class MOZ_RAII OptimizeSpreadCallIRGenerator : public IRGenerator {
  HandleValue val_;

  AttachDecision tryAttachArray();
  AttachDecision tryAttachNotOptimizable();

 public:
  OptimizeSpreadCallIRGenerator(JSContext* cx, HandleScript script,
                                jsbytecode* pc, ICState state,
                                HandleValue value);

  AttachDecision tryAttachStub();

  void trackAttached(const char* name);
};

enum class StringChar { CodeAt, At };
enum class ScriptedThisResult { NoAction, UninitializedThis, TemplateObject };

class MOZ_RAII CallIRGenerator : public IRGenerator {
 private:
  JSOp op_;
  uint32_t argc_;
  HandleValue callee_;
  HandleValue thisval_;
  HandleValue newTarget_;
  HandleValueArray args_;

  ScriptedThisResult getThisForScripted(HandleFunction calleeFunc,
                                        MutableHandleObject result);

  void emitNativeCalleeGuard(JSFunction* callee);
  void emitCalleeGuard(ObjOperandId calleeId, JSFunction* callee);

  bool canAttachAtomicsReadWriteModify();

  struct AtomicsReadWriteModifyOperands {
    ObjOperandId objId;
    IntPtrOperandId intPtrIndexId;
    OperandId numericValueId;
  };

  AtomicsReadWriteModifyOperands emitAtomicsReadWriteModifyOperands(
      HandleFunction callee);

  AttachDecision tryAttachArrayPush(HandleFunction callee);
  AttachDecision tryAttachArrayPopShift(HandleFunction callee,
                                        InlinableNative native);
  AttachDecision tryAttachArrayJoin(HandleFunction callee);
  AttachDecision tryAttachArraySlice(HandleFunction callee);
  AttachDecision tryAttachArrayIsArray(HandleFunction callee);
  AttachDecision tryAttachDataViewGet(HandleFunction callee, Scalar::Type type);
  AttachDecision tryAttachDataViewSet(HandleFunction callee, Scalar::Type type);
  AttachDecision tryAttachUnsafeGetReservedSlot(HandleFunction callee,
                                                InlinableNative native);
  AttachDecision tryAttachUnsafeSetReservedSlot(HandleFunction callee);
  AttachDecision tryAttachIsSuspendedGenerator(HandleFunction callee);
  AttachDecision tryAttachToObject(HandleFunction callee,
                                   InlinableNative native);
  AttachDecision tryAttachToInteger(HandleFunction callee);
  AttachDecision tryAttachToLength(HandleFunction callee);
  AttachDecision tryAttachIsObject(HandleFunction callee);
  AttachDecision tryAttachIsPackedArray(HandleFunction callee);
  AttachDecision tryAttachIsCallable(HandleFunction callee);
  AttachDecision tryAttachIsConstructor(HandleFunction callee);
  AttachDecision tryAttachIsCrossRealmArrayConstructor(HandleFunction callee);
  AttachDecision tryAttachGuardToClass(HandleFunction callee,
                                       InlinableNative native);
  AttachDecision tryAttachHasClass(HandleFunction callee, const JSClass* clasp,
                                   bool isPossiblyWrapped);
  AttachDecision tryAttachRegExpMatcherSearcherTester(HandleFunction callee,
                                                      InlinableNative native);
  AttachDecision tryAttachRegExpPrototypeOptimizable(HandleFunction callee);
  AttachDecision tryAttachRegExpInstanceOptimizable(HandleFunction callee);
  AttachDecision tryAttachGetFirstDollarIndex(HandleFunction callee);
  AttachDecision tryAttachSubstringKernel(HandleFunction callee);
  AttachDecision tryAttachObjectHasPrototype(HandleFunction callee);
  AttachDecision tryAttachString(HandleFunction callee);
  AttachDecision tryAttachStringConstructor(HandleFunction callee);
  AttachDecision tryAttachStringToStringValueOf(HandleFunction callee);
  AttachDecision tryAttachStringChar(HandleFunction callee, StringChar kind);
  AttachDecision tryAttachStringCharCodeAt(HandleFunction callee);
  AttachDecision tryAttachStringCharAt(HandleFunction callee);
  AttachDecision tryAttachStringFromCharCode(HandleFunction callee);
  AttachDecision tryAttachStringFromCodePoint(HandleFunction callee);
  AttachDecision tryAttachStringToLowerCase(HandleFunction callee);
  AttachDecision tryAttachStringToUpperCase(HandleFunction callee);
  AttachDecision tryAttachStringReplaceString(HandleFunction callee);
  AttachDecision tryAttachStringSplitString(HandleFunction callee);
  AttachDecision tryAttachMathRandom(HandleFunction callee);
  AttachDecision tryAttachMathAbs(HandleFunction callee);
  AttachDecision tryAttachMathClz32(HandleFunction callee);
  AttachDecision tryAttachMathSign(HandleFunction callee);
  AttachDecision tryAttachMathImul(HandleFunction callee);
  AttachDecision tryAttachMathFloor(HandleFunction callee);
  AttachDecision tryAttachMathCeil(HandleFunction callee);
  AttachDecision tryAttachMathTrunc(HandleFunction callee);
  AttachDecision tryAttachMathRound(HandleFunction callee);
  AttachDecision tryAttachMathSqrt(HandleFunction callee);
  AttachDecision tryAttachMathFRound(HandleFunction callee);
  AttachDecision tryAttachMathHypot(HandleFunction callee);
  AttachDecision tryAttachMathATan2(HandleFunction callee);
  AttachDecision tryAttachMathFunction(HandleFunction callee,
                                       UnaryMathFunction fun);
  AttachDecision tryAttachMathPow(HandleFunction callee);
  AttachDecision tryAttachMathMinMax(HandleFunction callee, bool isMax);
  AttachDecision tryAttachSpreadMathMinMax(HandleFunction callee, bool isMax);
  AttachDecision tryAttachIsTypedArray(HandleFunction callee,
                                       bool isPossiblyWrapped);
  AttachDecision tryAttachIsTypedArrayConstructor(HandleFunction callee);
  AttachDecision tryAttachTypedArrayByteOffset(HandleFunction callee);
  AttachDecision tryAttachTypedArrayElementSize(HandleFunction callee);
  AttachDecision tryAttachTypedArrayLength(HandleFunction callee,
                                           bool isPossiblyWrapped);
  AttachDecision tryAttachArrayBufferByteLength(HandleFunction callee,
                                                bool isPossiblyWrapped);
  AttachDecision tryAttachIsConstructing(HandleFunction callee);
  AttachDecision tryAttachGetNextMapSetEntryForIterator(HandleFunction callee,
                                                        bool isMap);
  AttachDecision tryAttachFinishBoundFunctionInit(HandleFunction callee);
  AttachDecision tryAttachNewArrayIterator(HandleFunction callee);
  AttachDecision tryAttachNewStringIterator(HandleFunction callee);
  AttachDecision tryAttachNewRegExpStringIterator(HandleFunction callee);
  AttachDecision tryAttachArrayIteratorPrototypeOptimizable(
      HandleFunction callee);
  AttachDecision tryAttachObjectCreate(HandleFunction callee);
  AttachDecision tryAttachArrayConstructor(HandleFunction callee);
  AttachDecision tryAttachTypedArrayConstructor(HandleFunction callee);
  AttachDecision tryAttachNumberToString(HandleFunction callee);
  AttachDecision tryAttachReflectGetPrototypeOf(HandleFunction callee);
  AttachDecision tryAttachAtomicsCompareExchange(HandleFunction callee);
  AttachDecision tryAttachAtomicsExchange(HandleFunction callee);
  AttachDecision tryAttachAtomicsAdd(HandleFunction callee);
  AttachDecision tryAttachAtomicsSub(HandleFunction callee);
  AttachDecision tryAttachAtomicsAnd(HandleFunction callee);
  AttachDecision tryAttachAtomicsOr(HandleFunction callee);
  AttachDecision tryAttachAtomicsXor(HandleFunction callee);
  AttachDecision tryAttachAtomicsLoad(HandleFunction callee);
  AttachDecision tryAttachAtomicsStore(HandleFunction callee);
  AttachDecision tryAttachAtomicsIsLockFree(HandleFunction callee);
  AttachDecision tryAttachBoolean(HandleFunction callee);
  AttachDecision tryAttachBailout(HandleFunction callee);
  AttachDecision tryAttachAssertFloat32(HandleFunction callee);
  AttachDecision tryAttachAssertRecoveredOnBailout(HandleFunction callee);
  AttachDecision tryAttachObjectIs(HandleFunction callee);
  AttachDecision tryAttachObjectIsPrototypeOf(HandleFunction callee);
  AttachDecision tryAttachObjectToString(HandleFunction callee);
  AttachDecision tryAttachBigIntAsIntN(HandleFunction callee);
  AttachDecision tryAttachBigIntAsUintN(HandleFunction callee);

  AttachDecision tryAttachFunCall(HandleFunction calleeFunc);
  AttachDecision tryAttachFunApply(HandleFunction calleeFunc);
  AttachDecision tryAttachCallScripted(HandleFunction calleeFunc);
  AttachDecision tryAttachInlinableNative(HandleFunction calleeFunc);
  AttachDecision tryAttachWasmCall(HandleFunction calleeFunc);
  AttachDecision tryAttachCallNative(HandleFunction calleeFunc);
  AttachDecision tryAttachCallHook(HandleObject calleeObj);

  void trackAttached(const char* name);

 public:
  CallIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, JSOp op,
                  ICState state, uint32_t argc, HandleValue callee,
                  HandleValue thisval, HandleValue newTarget,
                  HandleValueArray args);

  AttachDecision tryAttachStub();
};

class MOZ_RAII CompareIRGenerator : public IRGenerator {
  JSOp op_;
  HandleValue lhsVal_;
  HandleValue rhsVal_;

  AttachDecision tryAttachString(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachObject(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachSymbol(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachStrictDifferentTypes(ValOperandId lhsId,
                                               ValOperandId rhsId);
  AttachDecision tryAttachInt32(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachNumber(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachBigInt(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachNumberUndefined(ValOperandId lhsId,
                                          ValOperandId rhsId);
  AttachDecision tryAttachAnyNullUndefined(ValOperandId lhsId,
                                           ValOperandId rhsId);
  AttachDecision tryAttachNullUndefined(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachStringNumber(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachPrimitiveSymbol(ValOperandId lhsId,
                                          ValOperandId rhsId);
  AttachDecision tryAttachBoolStringOrNumber(ValOperandId lhsId,
                                             ValOperandId rhsId);
  AttachDecision tryAttachBigIntInt32(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachBigIntNumber(ValOperandId lhsId, ValOperandId rhsId);
  AttachDecision tryAttachBigIntString(ValOperandId lhsId, ValOperandId rhsId);

  void trackAttached(const char* name);

 public:
  CompareIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState state,
                     JSOp op, HandleValue lhsVal, HandleValue rhsVal);

  AttachDecision tryAttachStub();
};

class MOZ_RAII ToBoolIRGenerator : public IRGenerator {
  HandleValue val_;

  AttachDecision tryAttachBool();
  AttachDecision tryAttachInt32();
  AttachDecision tryAttachNumber();
  AttachDecision tryAttachString();
  AttachDecision tryAttachSymbol();
  AttachDecision tryAttachNullOrUndefined();
  AttachDecision tryAttachObject();
  AttachDecision tryAttachBigInt();

  void trackAttached(const char* name);

 public:
  ToBoolIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState state,
                    HandleValue val);

  AttachDecision tryAttachStub();
};

class MOZ_RAII GetIntrinsicIRGenerator : public IRGenerator {
  HandleValue val_;

  void trackAttached(const char* name);

 public:
  GetIntrinsicIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                          ICState state, HandleValue val);

  AttachDecision tryAttachStub();
};

class MOZ_RAII UnaryArithIRGenerator : public IRGenerator {
  JSOp op_;
  HandleValue val_;
  HandleValue res_;

  AttachDecision tryAttachInt32();
  AttachDecision tryAttachNumber();
  AttachDecision tryAttachBitwise();
  AttachDecision tryAttachBigInt();
  AttachDecision tryAttachStringInt32();
  AttachDecision tryAttachStringNumber();

  void trackAttached(const char* name);

 public:
  UnaryArithIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                        ICState state, JSOp op, HandleValue val,
                        HandleValue res);

  AttachDecision tryAttachStub();
};

class MOZ_RAII ToPropertyKeyIRGenerator : public IRGenerator {
  HandleValue val_;

  AttachDecision tryAttachInt32();
  AttachDecision tryAttachNumber();
  AttachDecision tryAttachString();
  AttachDecision tryAttachSymbol();

  void trackAttached(const char* name);

 public:
  ToPropertyKeyIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                           ICState state, HandleValue val);

  AttachDecision tryAttachStub();
};

class MOZ_RAII BinaryArithIRGenerator : public IRGenerator {
  JSOp op_;
  HandleValue lhs_;
  HandleValue rhs_;
  HandleValue res_;

  void trackAttached(const char* name);

  AttachDecision tryAttachInt32();
  AttachDecision tryAttachDouble();
  AttachDecision tryAttachBitwise();
  AttachDecision tryAttachStringConcat();
  AttachDecision tryAttachStringObjectConcat();
  AttachDecision tryAttachStringNumberConcat();
  AttachDecision tryAttachStringBooleanConcat();
  AttachDecision tryAttachBigInt();
  AttachDecision tryAttachStringInt32Arith();

 public:
  BinaryArithIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                         ICState state, JSOp op, HandleValue lhs,
                         HandleValue rhs, HandleValue res);

  AttachDecision tryAttachStub();
};

class MOZ_RAII NewArrayIRGenerator : public IRGenerator {
#ifdef JS_CACHEIR_SPEW
  JSOp op_;
#endif
  HandleObject templateObject_;
  BaselineFrame* frame_;

  void trackAttached(const char* name);

 public:
  NewArrayIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                      ICState state, JSOp op, HandleObject templateObj,
                      BaselineFrame* frame);

  AttachDecision tryAttachStub();
  AttachDecision tryAttachArrayObject();
};

class MOZ_RAII NewObjectIRGenerator : public IRGenerator {
#ifdef JS_CACHEIR_SPEW
  JSOp op_;
#endif
  HandleObject templateObject_;
  BaselineFrame* frame_;

  void trackAttached(const char* name);

 public:
  NewObjectIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc,
                       ICState state, JSOp op, HandleObject templateObj,
                       BaselineFrame* frame);

  AttachDecision tryAttachStub();
  AttachDecision tryAttachPlainObject();
};

inline bool BytecodeOpCanHaveAllocSite(JSOp op) {
  return op == JSOp::NewArray || op == JSOp::NewObject || op == JSOp::NewInit;
}

// Retrieve Xray JIT info set by the embedder.
extern JS::XrayJitInfo* GetXrayJitInfo();

}  // namespace jit
}  // namespace js

#endif /* jit_CacheIR_h */
