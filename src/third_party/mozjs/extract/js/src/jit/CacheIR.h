/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CacheIR_h
#define jit_CacheIR_h

#include "mozilla/Maybe.h"

#include "NamespaceImports.h"

#include "gc/Rooting.h"
#include "jit/CompactBuffer.h"
#include "jit/ICState.h"
#include "jit/SharedIC.h"

namespace js {
namespace jit {


enum class BaselineCacheIRStubKind;

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
// JitCompartment has a CacheIRStubInfo* -> JitCode* weak map that's used to
// share both the IR and JitCode between CacheIR stubs. This HashMap owns the
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
// be an object.
class OperandId
{
  protected:
    static const uint16_t InvalidId = UINT16_MAX;
    uint16_t id_;

    OperandId() : id_(InvalidId) {}
    explicit OperandId(uint16_t id) : id_(id) {}

  public:
    uint16_t id() const { return id_; }
    bool valid() const { return id_ != InvalidId; }
};

class ValOperandId : public OperandId
{
  public:
    ValOperandId() = default;
    explicit ValOperandId(uint16_t id) : OperandId(id) {}
};

class ValueTagOperandId : public OperandId
{
  public:
    ValueTagOperandId() = default;
    explicit ValueTagOperandId(uint16_t id) : OperandId(id) {}
};

class ObjOperandId : public OperandId
{
  public:
    ObjOperandId() = default;
    explicit ObjOperandId(uint16_t id) : OperandId(id) {}

    bool operator==(const ObjOperandId& other) const { return id_ == other.id_; }
    bool operator!=(const ObjOperandId& other) const { return id_ != other.id_; }
};

class StringOperandId : public OperandId
{
  public:
    StringOperandId() = default;
    explicit StringOperandId(uint16_t id) : OperandId(id) {}
};

class SymbolOperandId : public OperandId
{
  public:
    SymbolOperandId() = default;
    explicit SymbolOperandId(uint16_t id) : OperandId(id) {}
};

class Int32OperandId : public OperandId
{
  public:
    Int32OperandId() = default;
    explicit Int32OperandId(uint16_t id) : OperandId(id) {}
};

class TypedOperandId : public OperandId
{
    JSValueType type_;

  public:
    MOZ_IMPLICIT TypedOperandId(ObjOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_OBJECT)
    {}
    MOZ_IMPLICIT TypedOperandId(StringOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_STRING)
    {}
    MOZ_IMPLICIT TypedOperandId(SymbolOperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_SYMBOL)
    {}
    MOZ_IMPLICIT TypedOperandId(Int32OperandId id)
      : OperandId(id.id()), type_(JSVAL_TYPE_INT32)
    {}
    MOZ_IMPLICIT TypedOperandId(ValueTagOperandId val)
      : OperandId(val.id()), type_(JSVAL_TYPE_UNKNOWN)
    {}
    TypedOperandId(ValOperandId val, JSValueType type)
      : OperandId(val.id()), type_(type)
    {}

    JSValueType type() const { return type_; }
};

#define CACHE_IR_KINDS(_)   \
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
    _(TypeOf)               \
    _(InstanceOf)           \
    _(GetIterator)          \
    _(Compare)              \
    _(ToBool)               \
    _(Call)

enum class CacheKind : uint8_t
{
#define DEFINE_KIND(kind) kind,
    CACHE_IR_KINDS(DEFINE_KIND)
#undef DEFINE_KIND
};

extern const char* CacheKindNames[];

#define CACHE_IR_OPS(_)                   \
    _(GuardIsObject)                      \
    _(GuardIsObjectOrNull)                \
    _(GuardIsNullOrUndefined)             \
    _(GuardIsString)                      \
    _(GuardIsSymbol)                      \
    _(GuardIsNumber)                      \
    _(GuardIsInt32Index)                  \
    _(GuardType)                          \
    _(GuardShape)                         \
    _(GuardGroup)                         \
    _(GuardProto)                         \
    _(GuardClass)                         /* Guard an object class, per GuardClassKind */ \
    _(GuardAnyClass)                      /* Guard an arbitrary class for an object */ \
    _(GuardCompartment)                   \
    _(GuardIsNativeFunction)              \
    _(GuardIsNativeObject)                \
    _(GuardIsProxy)                       \
    _(GuardHasProxyHandler)               \
    _(GuardNotDOMProxy)                   \
    _(GuardSpecificObject)                \
    _(GuardSpecificAtom)                  \
    _(GuardSpecificSymbol)                \
    _(GuardSpecificInt32Immediate)        \
    _(GuardNoDetachedTypedObjects)        \
    _(GuardMagicValue)                    \
    _(GuardFrameHasNoArgumentsObject)     \
    _(GuardNoDenseElements)               \
    _(GuardNoUnboxedExpando)              \
    _(GuardAndLoadUnboxedExpando)         \
    _(GuardAndGetIndexFromString)         \
    _(GuardAndGetIterator)                \
    _(GuardHasGetterSetter)               \
    _(GuardGroupHasUnanalyzedNewScript)   \
    _(GuardIndexIsNonNegative)            \
    _(GuardTagNotEqual)                   \
    _(GuardXrayExpandoShapeAndDefaultProto) \
    _(GuardFunctionPrototype)             \
    _(LoadStackValue)                     \
    _(LoadObject)                         \
    _(LoadProto)                          \
    _(LoadEnclosingEnvironment)           \
    _(LoadWrapperTarget)                  \
    _(LoadValueTag)                       \
                                          \
    _(MegamorphicLoadSlotResult)          \
    _(MegamorphicLoadSlotByValueResult)   \
    _(MegamorphicStoreSlot)               \
    _(MegamorphicSetElement)              \
    _(MegamorphicHasPropResult)           \
                                          \
    /* See CacheIR.cpp 'DOM proxies' comment. */ \
    _(LoadDOMExpandoValue)                \
    _(LoadDOMExpandoValueGuardGeneration) \
    _(LoadDOMExpandoValueIgnoreGeneration)\
    _(GuardDOMExpandoMissingOrGuardShape) \
                                          \
    _(StoreFixedSlot)                     \
    _(StoreDynamicSlot)                   \
    _(AddAndStoreFixedSlot)               \
    _(AddAndStoreDynamicSlot)             \
    _(AllocateAndStoreDynamicSlot)        \
    _(StoreTypedObjectReferenceProperty)  \
    _(StoreTypedObjectScalarProperty)     \
    _(StoreUnboxedProperty)               \
    _(StoreDenseElement)                  \
    _(StoreDenseElementHole)              \
    _(ArrayPush)                          \
    _(ArrayJoinResult)                    \
    _(StoreTypedElement)                  \
    _(CallNativeSetter)                   \
    _(CallScriptedSetter)                 \
    _(CallSetArrayLength)                 \
    _(CallProxySet)                       \
    _(CallProxySetByValue)                \
                                          \
    /* The *Result ops load a value into the cache's result register. */ \
    _(LoadFixedSlotResult)                \
    _(LoadDynamicSlotResult)              \
    _(LoadUnboxedPropertyResult)          \
    _(LoadTypedObjectResult)              \
    _(LoadDenseElementResult)             \
    _(LoadDenseElementHoleResult)         \
    _(LoadDenseElementExistsResult)       \
    _(LoadTypedElementExistsResult)       \
    _(LoadDenseElementHoleExistsResult)   \
    _(LoadTypedElementResult)             \
    _(LoadInt32ArrayLengthResult)         \
    _(LoadArgumentsObjectArgResult)       \
    _(LoadArgumentsObjectLengthResult)    \
    _(LoadFunctionLengthResult)           \
    _(LoadStringCharResult)               \
    _(LoadStringLengthResult)             \
    _(LoadFrameCalleeResult)              \
    _(LoadFrameNumActualArgsResult)       \
    _(LoadFrameArgumentResult)            \
    _(LoadEnvironmentFixedSlotResult)     \
    _(LoadEnvironmentDynamicSlotResult)   \
    _(LoadObjectResult)                   \
    _(CallScriptedGetterResult)           \
    _(CallNativeGetterResult)             \
    _(CallProxyGetResult)                 \
    _(CallProxyGetByValueResult)          \
    _(CallProxyHasPropResult)             \
    _(CallObjectHasSparseElementResult)   \
    _(LoadUndefinedResult)                \
    _(LoadBooleanResult)                  \
    _(LoadStringResult)                   \
    _(LoadInstanceOfObjectResult)         \
    _(LoadTypeOfObjectResult)             \
    _(LoadInt32TruthyResult)              \
    _(LoadDoubleTruthyResult)             \
    _(LoadStringTruthyResult)             \
    _(LoadObjectTruthyResult)             \
    _(LoadValueResult)                    \
                                          \
    _(CallStringSplitResult)              \
                                          \
    _(CompareStringResult)                \
    _(CompareObjectResult)                \
    _(CompareSymbolResult)                \
                                          \
    _(CallPrintString)                    \
    _(Breakpoint)                         \
                                          \
    _(TypeMonitorResult)                  \
    _(ReturnFromIC)                       \
    _(WrapResult)

enum class CacheOp {
#define DEFINE_OP(op) op,
    CACHE_IR_OPS(DEFINE_OP)
#undef DEFINE_OP
};

class StubField
{
  public:
    enum class Type : uint8_t {
        // These fields take up a single word.
        RawWord,
        Shape,
        ObjectGroup,
        JSObject,
        Symbol,
        String,
        Id,

        // These fields take up 64 bits on all platforms.
        RawInt64,
        First64BitType = RawInt64,
        DOMExpandoGeneration,
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
        if (sizeIsWord(type))
            return sizeof(uintptr_t);
        MOZ_ASSERT(sizeIsInt64(type));
        return sizeof(int64_t);
    }

  private:
    uint64_t data_;
    Type type_;

  public:
    StubField(uint64_t data, Type type)
      : data_(data), type_(type)
    {
        MOZ_ASSERT_IF(sizeIsWord(), data <= UINTPTR_MAX);
    }

    Type type() const { return type_; }

    bool sizeIsWord() const { return sizeIsWord(type_); }
    bool sizeIsInt64() const { return sizeIsInt64(type_); }

    uintptr_t asWord() const { MOZ_ASSERT(sizeIsWord()); return uintptr_t(data_); }
    uint64_t asInt64() const { MOZ_ASSERT(sizeIsInt64()); return data_; }
} JS_HAZ_GC_POINTER;

// We use this enum as GuardClass operand, instead of storing Class* pointers
// in the IR, to keep the IR compact and the same size on all platforms.
enum class GuardClassKind : uint8_t
{
    Array,
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

void LoadShapeWrapperContents(MacroAssembler& masm, Register obj, Register dst, Label* failure);

// Class to record CacheIR + some additional metadata for code generation.
class MOZ_RAII CacheIRWriter : public JS::CustomAutoRooter
{
    JSContext* cx_;
    CompactBufferWriter buffer_;

    uint32_t nextOperandId_;
    uint32_t nextInstructionId_;
    uint32_t numInputOperands_;

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

    void assertSameCompartment(JSObject*);

    void writeOp(CacheOp op) {
        MOZ_ASSERT(uint32_t(op) <= UINT8_MAX);
        buffer_.writeByte(uint32_t(op));
        nextInstructionId_++;
    }

    void writeOperandId(OperandId opId) {
        if (opId.id() < MaxOperandIds) {
            static_assert(MaxOperandIds <= UINT8_MAX, "operand id must fit in a single byte");
            buffer_.writeByte(opId.id());
        } else {
            tooLarge_ = true;
            return;
        }
        if (opId.id() >= operandLastUsed_.length()) {
            buffer_.propagateOOM(operandLastUsed_.resize(opId.id() + 1));
            if (buffer_.oom())
                return;
        }
        MOZ_ASSERT(nextInstructionId_ > 0);
        operandLastUsed_[opId.id()] = nextInstructionId_ - 1;
    }

    void writeInt32Immediate(int32_t i32) {
        buffer_.writeSigned(i32);
    }
    void writeUint32Immediate(uint32_t u32) {
        buffer_.writeUnsigned(u32);
    }
    void writePointer(void* ptr) {
        buffer_.writeRawPointer(ptr);
    }

    void writeOpWithOperandId(CacheOp op, OperandId opId) {
        writeOp(op);
        writeOperandId(opId);
    }

    void addStubField(uint64_t value, StubField::Type fieldType) {
        size_t newStubDataSize = stubDataSize_ + StubField::sizeInBytes(fieldType);
        if (newStubDataSize < MaxStubDataSizeInBytes) {
            buffer_.propagateOOM(stubFields_.append(StubField(value, fieldType)));
            MOZ_ASSERT((stubDataSize_ % sizeof(uintptr_t)) == 0);
            buffer_.writeByte(stubDataSize_ / sizeof(uintptr_t));
            stubDataSize_ = newStubDataSize;
        } else {
            tooLarge_ = true;
        }
    }

    CacheIRWriter(const CacheIRWriter&) = delete;
    CacheIRWriter& operator=(const CacheIRWriter&) = delete;

  public:
    explicit CacheIRWriter(JSContext* cx)
      : CustomAutoRooter(cx),
        cx_(cx),
        nextOperandId_(0),
        nextInstructionId_(0),
        numInputOperands_(0),
        stubDataSize_(0),
        tooLarge_(false)
    {}

    bool failed() const { return buffer_.oom() || tooLarge_; }

    uint32_t numInputOperands() const { return numInputOperands_; }
    uint32_t numOperandIds() const { return nextOperandId_; }
    uint32_t numInstructions() const { return nextInstructionId_; }

    size_t numStubFields() const { return stubFields_.length(); }
    StubField::Type stubFieldType(uint32_t i) const { return stubFields_[i].type(); }

    uint32_t setInputOperandId(uint32_t op) {
        MOZ_ASSERT(op == nextOperandId_);
        nextOperandId_++;
        numInputOperands_++;
        return op;
    }

    void trace(JSTracer* trc) override {
        // For now, assert we only GC before we append stub fields.
        MOZ_RELEASE_ASSERT(stubFields_.empty());
    }

    size_t stubDataSize() const {
        return stubDataSize_;
    }
    void copyStubData(uint8_t* dest) const;
    bool stubDataEqualsMaybeUpdate(uint8_t* stubData, bool* updated) const;

    bool operandIsDead(uint32_t operandId, uint32_t currentInstruction) const {
        if (operandId >= operandLastUsed_.length())
            return false;
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
    StubField readStubFieldForIon(size_t i, StubField::Type type) const {
        MOZ_ASSERT(stubFields_[i].type() == type);
        return stubFields_[i];
    }

    ObjOperandId guardIsObject(ValOperandId val) {
        writeOpWithOperandId(CacheOp::GuardIsObject, val);
        return ObjOperandId(val.id());
    }
    StringOperandId guardIsString(ValOperandId val) {
        writeOpWithOperandId(CacheOp::GuardIsString, val);
        return StringOperandId(val.id());
    }
    SymbolOperandId guardIsSymbol(ValOperandId val) {
        writeOpWithOperandId(CacheOp::GuardIsSymbol, val);
        return SymbolOperandId(val.id());
    }
    Int32OperandId guardIsInt32Index(ValOperandId val) {
        Int32OperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::GuardIsInt32Index, val);
        writeOperandId(res);
        return res;
    }
    void guardIsNumber(ValOperandId val) {
        writeOpWithOperandId(CacheOp::GuardIsNumber, val);
    }
    void guardType(ValOperandId val, JSValueType type) {
        writeOpWithOperandId(CacheOp::GuardType, val);
        static_assert(sizeof(type) == sizeof(uint8_t), "JSValueType should fit in a byte");
        buffer_.writeByte(uint32_t(type));
    }
    void guardIsObjectOrNull(ValOperandId val) {
        writeOpWithOperandId(CacheOp::GuardIsObjectOrNull, val);
    }
    void guardIsNullOrUndefined(ValOperandId val) {
        writeOpWithOperandId(CacheOp::GuardIsNullOrUndefined, val);
    }
    void guardShape(ObjOperandId obj, Shape* shape) {
        MOZ_ASSERT(shape);
        writeOpWithOperandId(CacheOp::GuardShape, obj);
        addStubField(uintptr_t(shape), StubField::Type::Shape);
    }
    void guardShapeForClass(ObjOperandId obj, Shape* shape) {
        // Guard shape to ensure that object class is unchanged. This is true
        // for all shapes.
        guardShape(obj, shape);
    }
    void guardShapeForOwnProperties(ObjOperandId obj, Shape* shape) {
        // Guard shape to detect changes to (non-dense) own properties. This
        // also implies |guardShapeForClass|.
        MOZ_ASSERT(shape->getObjectClass()->isNative());
        guardShape(obj, shape);
    }
    void guardXrayExpandoShapeAndDefaultProto(ObjOperandId obj, JSObject* shapeWrapper) {
        assertSameCompartment(shapeWrapper);
        writeOpWithOperandId(CacheOp::GuardXrayExpandoShapeAndDefaultProto, obj);
        buffer_.writeByte(uint32_t(!!shapeWrapper));
        addStubField(uintptr_t(shapeWrapper), StubField::Type::JSObject);
    }
    // Guard rhs[slot] == prototypeObject
    void guardFunctionPrototype(ObjOperandId rhs, uint32_t slot, ObjOperandId protoId) {
        writeOpWithOperandId(CacheOp::GuardFunctionPrototype, rhs);
        writeOperandId(protoId);
        addStubField(slot, StubField::Type::RawWord);
    }
  private:
    // Use (or create) a specialization below to clarify what constaint the
    // group guard is implying.
    void guardGroup(ObjOperandId obj, ObjectGroup* group) {
        writeOpWithOperandId(CacheOp::GuardGroup, obj);
        addStubField(uintptr_t(group), StubField::Type::ObjectGroup);
    }
  public:
    void guardGroupForProto(ObjOperandId obj, ObjectGroup* group) {
        MOZ_ASSERT(!group->hasUncacheableProto());
        guardGroup(obj, group);
    }
    void guardGroupForTypeBarrier(ObjOperandId obj, ObjectGroup* group) {
        // Typesets will always be a super-set of any typesets previously seen
        // for this group. If the type/group of a value being stored to a
        // property in this group is not known, a TypeUpdate IC chain should be
        // used as well.
        guardGroup(obj, group);
    }
    void guardGroupForLayout(ObjOperandId obj, ObjectGroup* group) {
        // NOTE: Comment in guardGroupForTypeBarrier also applies.
        MOZ_ASSERT(!group->hasUncacheableClass());
        MOZ_ASSERT(IsUnboxedObjectClass(group->clasp()) ||
                   IsTypedObjectClass(group->clasp()));
        guardGroup(obj, group);
    }
    void guardProto(ObjOperandId obj, JSObject* proto) {
        assertSameCompartment(proto);
        writeOpWithOperandId(CacheOp::GuardProto, obj);
        addStubField(uintptr_t(proto), StubField::Type::JSObject);
    }
    void guardClass(ObjOperandId obj, GuardClassKind kind) {
        static_assert(sizeof(GuardClassKind) == sizeof(uint8_t),
                      "GuardClassKind must fit in a byte");
        writeOpWithOperandId(CacheOp::GuardClass, obj);
        buffer_.writeByte(uint32_t(kind));
    }
    void guardAnyClass(ObjOperandId obj, const Class* clasp) {
        writeOpWithOperandId(CacheOp::GuardAnyClass, obj);
        addStubField(uintptr_t(clasp), StubField::Type::RawWord);
    }
    void guardIsNativeFunction(ObjOperandId obj, JSNative nativeFunc) {
        writeOpWithOperandId(CacheOp::GuardIsNativeFunction, obj);
        writePointer(JS_FUNC_TO_DATA_PTR(void*, nativeFunc));
    }
    void guardIsNativeObject(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::GuardIsNativeObject, obj);
    }
    void guardIsProxy(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::GuardIsProxy, obj);
    }
    void guardHasProxyHandler(ObjOperandId obj, const void* handler) {
        writeOpWithOperandId(CacheOp::GuardHasProxyHandler, obj);
        addStubField(uintptr_t(handler), StubField::Type::RawWord);
    }
    void guardNotDOMProxy(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::GuardNotDOMProxy, obj);
    }
    void guardSpecificObject(ObjOperandId obj, JSObject* expected) {
        assertSameCompartment(expected);
        writeOpWithOperandId(CacheOp::GuardSpecificObject, obj);
        addStubField(uintptr_t(expected), StubField::Type::JSObject);
    }
    void guardSpecificAtom(StringOperandId str, JSAtom* expected) {
        writeOpWithOperandId(CacheOp::GuardSpecificAtom, str);
        addStubField(uintptr_t(expected), StubField::Type::String);
    }
    void guardSpecificSymbol(SymbolOperandId sym, JS::Symbol* expected) {
        writeOpWithOperandId(CacheOp::GuardSpecificSymbol, sym);
        addStubField(uintptr_t(expected), StubField::Type::Symbol);
    }
    void guardSpecificInt32Immediate(Int32OperandId operand, int32_t expected,
                                     Assembler::Condition cond = Assembler::Equal)
    {
        writeOp(CacheOp::GuardSpecificInt32Immediate);
        writeOperandId(operand);
        writeInt32Immediate(expected);
        buffer_.writeByte(uint32_t(cond));
    }
    void guardMagicValue(ValOperandId val, JSWhyMagic magic) {
        writeOpWithOperandId(CacheOp::GuardMagicValue, val);
        buffer_.writeByte(uint32_t(magic));
    }
    void guardCompartment(ObjOperandId obj, JSObject* global, JSCompartment* compartment) {
        assertSameCompartment(global);
        writeOpWithOperandId(CacheOp::GuardCompartment, obj);
        // Add a reference to the compartment's global to keep it alive.
        addStubField(uintptr_t(global), StubField::Type::JSObject);
        // Use RawWord, because compartments never move and it can't be GCed.
        addStubField(uintptr_t(compartment), StubField::Type::RawWord);
    }
    void guardNoDetachedTypedObjects() {
        writeOp(CacheOp::GuardNoDetachedTypedObjects);
    }
    void guardFrameHasNoArgumentsObject() {
        writeOp(CacheOp::GuardFrameHasNoArgumentsObject);
    }

    Int32OperandId guardAndGetIndexFromString(StringOperandId str) {
        Int32OperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::GuardAndGetIndexFromString, str);
        writeOperandId(res);
        return res;
    }
    ObjOperandId guardAndGetIterator(ObjOperandId obj, PropertyIteratorObject* iter,
                                     NativeIterator** enumeratorsAddr)
    {
        ObjOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::GuardAndGetIterator, obj);
        addStubField(uintptr_t(iter), StubField::Type::JSObject);
        addStubField(uintptr_t(enumeratorsAddr), StubField::Type::RawWord);
        writeOperandId(res);
        return res;
    }

    void guardHasGetterSetter(ObjOperandId obj, Shape* shape) {
        writeOpWithOperandId(CacheOp::GuardHasGetterSetter, obj);
        addStubField(uintptr_t(shape), StubField::Type::Shape);
    }
    void guardGroupHasUnanalyzedNewScript(ObjectGroup* group) {
        writeOp(CacheOp::GuardGroupHasUnanalyzedNewScript);
        addStubField(uintptr_t(group), StubField::Type::ObjectGroup);
    }

    void guardIndexIsNonNegative(Int32OperandId index) {
        writeOpWithOperandId(CacheOp::GuardIndexIsNonNegative, index);
    }
    void guardTagNotEqual(ValueTagOperandId lhs, ValueTagOperandId rhs) {
        writeOpWithOperandId(CacheOp::GuardTagNotEqual, lhs);
        writeOperandId(rhs);
    }

    void loadFrameCalleeResult() {
        writeOp(CacheOp::LoadFrameCalleeResult);
    }
    void loadFrameNumActualArgsResult() {
        writeOp(CacheOp::LoadFrameNumActualArgsResult);
    }
    void loadFrameArgumentResult(Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadFrameArgumentResult, index);
    }
    void guardNoDenseElements(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::GuardNoDenseElements, obj);
    }
    void guardNoUnboxedExpando(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::GuardNoUnboxedExpando, obj);
    }
    ObjOperandId guardAndLoadUnboxedExpando(ObjOperandId obj) {
        ObjOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::GuardAndLoadUnboxedExpando, obj);
        writeOperandId(res);
        return res;
    }

    ValOperandId loadStackValue(uint32_t idx) {
        ValOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadStackValue, res);
        writeUint32Immediate(idx);
        return res;
    }
    ObjOperandId loadObject(JSObject* obj) {
        assertSameCompartment(obj);
        ObjOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadObject, res);
        addStubField(uintptr_t(obj), StubField::Type::JSObject);
        return res;
    }
    ObjOperandId loadProto(ObjOperandId obj) {
        ObjOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadProto, obj);
        writeOperandId(res);
        return res;
    }

    ObjOperandId loadEnclosingEnvironment(ObjOperandId obj) {
        ObjOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadEnclosingEnvironment, obj);
        writeOperandId(res);
        return res;
    }

    ObjOperandId loadWrapperTarget(ObjOperandId obj) {
        ObjOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadWrapperTarget, obj);
        writeOperandId(res);
        return res;
    }

    ValueTagOperandId loadValueTag(ValOperandId val) {
        ValueTagOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadValueTag, val);
        writeOperandId(res);
        return res;
    }

    ValOperandId loadDOMExpandoValue(ObjOperandId obj) {
        ValOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadDOMExpandoValue, obj);
        writeOperandId(res);
        return res;
    }
    void guardDOMExpandoMissingOrGuardShape(ValOperandId expando, Shape* shape) {
        writeOpWithOperandId(CacheOp::GuardDOMExpandoMissingOrGuardShape, expando);
        addStubField(uintptr_t(shape), StubField::Type::Shape);
    }
    ValOperandId loadDOMExpandoValueGuardGeneration(ObjOperandId obj,
                                                    ExpandoAndGeneration* expandoAndGeneration)
    {
        ValOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadDOMExpandoValueGuardGeneration, obj);
        addStubField(uintptr_t(expandoAndGeneration), StubField::Type::RawWord);
        addStubField(expandoAndGeneration->generation, StubField::Type::DOMExpandoGeneration);
        writeOperandId(res);
        return res;
    }
    ValOperandId loadDOMExpandoValueIgnoreGeneration(ObjOperandId obj) {
        ValOperandId res(nextOperandId_++);
        writeOpWithOperandId(CacheOp::LoadDOMExpandoValueIgnoreGeneration, obj);
        writeOperandId(res);
        return res;
    }

    void storeFixedSlot(ObjOperandId obj, size_t offset, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::StoreFixedSlot, obj);
        addStubField(offset, StubField::Type::RawWord);
        writeOperandId(rhs);
    }
    void storeDynamicSlot(ObjOperandId obj, size_t offset, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::StoreDynamicSlot, obj);
        addStubField(offset, StubField::Type::RawWord);
        writeOperandId(rhs);
    }
    void addAndStoreFixedSlot(ObjOperandId obj, size_t offset, ValOperandId rhs,
                              Shape* newShape, bool changeGroup, ObjectGroup* newGroup)
    {
        writeOpWithOperandId(CacheOp::AddAndStoreFixedSlot, obj);
        addStubField(offset, StubField::Type::RawWord);
        writeOperandId(rhs);
        buffer_.writeByte(changeGroup);
        addStubField(uintptr_t(newGroup), StubField::Type::ObjectGroup);
        addStubField(uintptr_t(newShape), StubField::Type::Shape);
    }
    void addAndStoreDynamicSlot(ObjOperandId obj, size_t offset, ValOperandId rhs,
                                Shape* newShape, bool changeGroup, ObjectGroup* newGroup)
    {
        writeOpWithOperandId(CacheOp::AddAndStoreDynamicSlot, obj);
        addStubField(offset, StubField::Type::RawWord);
        writeOperandId(rhs);
        buffer_.writeByte(changeGroup);
        addStubField(uintptr_t(newGroup), StubField::Type::ObjectGroup);
        addStubField(uintptr_t(newShape), StubField::Type::Shape);
    }
    void allocateAndStoreDynamicSlot(ObjOperandId obj, size_t offset, ValOperandId rhs,
                                     Shape* newShape, bool changeGroup, ObjectGroup* newGroup,
                                     uint32_t numNewSlots)
    {
        writeOpWithOperandId(CacheOp::AllocateAndStoreDynamicSlot, obj);
        addStubField(offset, StubField::Type::RawWord);
        writeOperandId(rhs);
        buffer_.writeByte(changeGroup);
        addStubField(uintptr_t(newGroup), StubField::Type::ObjectGroup);
        addStubField(uintptr_t(newShape), StubField::Type::Shape);
        addStubField(numNewSlots, StubField::Type::RawWord);
    }

    void storeTypedObjectReferenceProperty(ObjOperandId obj, uint32_t offset,
                                           TypedThingLayout layout, ReferenceTypeDescr::Type type,
                                           ValOperandId rhs)
    {
        writeOpWithOperandId(CacheOp::StoreTypedObjectReferenceProperty, obj);
        addStubField(offset, StubField::Type::RawWord);
        buffer_.writeByte(uint32_t(layout));
        buffer_.writeByte(uint32_t(type));
        writeOperandId(rhs);
    }
    void storeTypedObjectScalarProperty(ObjOperandId obj, uint32_t offset, TypedThingLayout layout,
                                        Scalar::Type type, ValOperandId rhs)
    {
        writeOpWithOperandId(CacheOp::StoreTypedObjectScalarProperty, obj);
        addStubField(offset, StubField::Type::RawWord);
        buffer_.writeByte(uint32_t(layout));
        buffer_.writeByte(uint32_t(type));
        writeOperandId(rhs);
    }
    void storeUnboxedProperty(ObjOperandId obj, JSValueType type, size_t offset,
                              ValOperandId rhs)
    {
        writeOpWithOperandId(CacheOp::StoreUnboxedProperty, obj);
        buffer_.writeByte(uint32_t(type));
        addStubField(offset, StubField::Type::RawWord);
        writeOperandId(rhs);
    }
    void storeDenseElement(ObjOperandId obj, Int32OperandId index, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::StoreDenseElement, obj);
        writeOperandId(index);
        writeOperandId(rhs);
    }
    void storeTypedElement(ObjOperandId obj, Int32OperandId index, ValOperandId rhs,
                           TypedThingLayout layout, Scalar::Type elementType, bool handleOOB)
    {
        writeOpWithOperandId(CacheOp::StoreTypedElement, obj);
        writeOperandId(index);
        writeOperandId(rhs);
        buffer_.writeByte(uint32_t(layout));
        buffer_.writeByte(uint32_t(elementType));
        buffer_.writeByte(uint32_t(handleOOB));
    }
    void storeDenseElementHole(ObjOperandId obj, Int32OperandId index, ValOperandId rhs,
                               bool handleAdd)
    {
        writeOpWithOperandId(CacheOp::StoreDenseElementHole, obj);
        writeOperandId(index);
        writeOperandId(rhs);
        buffer_.writeByte(handleAdd);
    }
    void arrayPush(ObjOperandId obj, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::ArrayPush, obj);
        writeOperandId(rhs);
    }
    void arrayJoinResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::ArrayJoinResult, obj);
    }
    void callScriptedSetter(ObjOperandId obj, JSFunction* setter, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::CallScriptedSetter, obj);
        addStubField(uintptr_t(setter), StubField::Type::JSObject);
        writeOperandId(rhs);
    }
    void callNativeSetter(ObjOperandId obj, JSFunction* setter, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::CallNativeSetter, obj);
        addStubField(uintptr_t(setter), StubField::Type::JSObject);
        writeOperandId(rhs);
    }
    void callSetArrayLength(ObjOperandId obj, bool strict, ValOperandId rhs) {
        writeOpWithOperandId(CacheOp::CallSetArrayLength, obj);
        buffer_.writeByte(uint32_t(strict));
        writeOperandId(rhs);
    }
    void callProxySet(ObjOperandId obj, jsid id, ValOperandId rhs, bool strict) {
        writeOpWithOperandId(CacheOp::CallProxySet, obj);
        writeOperandId(rhs);
        addStubField(uintptr_t(JSID_BITS(id)), StubField::Type::Id);
        buffer_.writeByte(uint32_t(strict));
    }
    void callProxySetByValue(ObjOperandId obj, ValOperandId id, ValOperandId rhs, bool strict) {
        writeOpWithOperandId(CacheOp::CallProxySetByValue, obj);
        writeOperandId(id);
        writeOperandId(rhs);
        buffer_.writeByte(uint32_t(strict));
    }

    void megamorphicLoadSlotResult(ObjOperandId obj, PropertyName* name, bool handleMissing) {
        writeOpWithOperandId(CacheOp::MegamorphicLoadSlotResult, obj);
        addStubField(uintptr_t(name), StubField::Type::String);
        buffer_.writeByte(uint32_t(handleMissing));
    }
    void megamorphicLoadSlotByValueResult(ObjOperandId obj, ValOperandId id, bool handleMissing) {
        writeOpWithOperandId(CacheOp::MegamorphicLoadSlotByValueResult, obj);
        writeOperandId(id);
        buffer_.writeByte(uint32_t(handleMissing));
    }
    void megamorphicStoreSlot(ObjOperandId obj, PropertyName* name, ValOperandId rhs,
                              bool needsTypeBarrier) {
        writeOpWithOperandId(CacheOp::MegamorphicStoreSlot, obj);
        addStubField(uintptr_t(name), StubField::Type::String);
        writeOperandId(rhs);
        buffer_.writeByte(needsTypeBarrier);
    }
    void megamorphicSetElement(ObjOperandId obj, ValOperandId id, ValOperandId rhs, bool strict) {
        writeOpWithOperandId(CacheOp::MegamorphicSetElement, obj);
        writeOperandId(id);
        writeOperandId(rhs);
        buffer_.writeByte(uint32_t(strict));
    }
    void megamorphicHasPropResult(ObjOperandId obj, ValOperandId id, bool hasOwn) {
        writeOpWithOperandId(CacheOp::MegamorphicHasPropResult, obj);
        writeOperandId(id);
        buffer_.writeByte(uint32_t(hasOwn));
    }

    void loadBooleanResult(bool val) {
        writeOp(CacheOp::LoadBooleanResult);
        buffer_.writeByte(uint32_t(val));
    }
    void loadUndefinedResult() {
        writeOp(CacheOp::LoadUndefinedResult);
    }
    void loadStringResult(JSString* str) {
        writeOp(CacheOp::LoadStringResult);
        addStubField(uintptr_t(str), StubField::Type::String);
    }
    void loadFixedSlotResult(ObjOperandId obj, size_t offset) {
        writeOpWithOperandId(CacheOp::LoadFixedSlotResult, obj);
        addStubField(offset, StubField::Type::RawWord);
    }
    void loadDynamicSlotResult(ObjOperandId obj, size_t offset) {
        writeOpWithOperandId(CacheOp::LoadDynamicSlotResult, obj);
        addStubField(offset, StubField::Type::RawWord);
    }
    void loadUnboxedPropertyResult(ObjOperandId obj, JSValueType type, size_t offset) {
        writeOpWithOperandId(CacheOp::LoadUnboxedPropertyResult, obj);
        buffer_.writeByte(uint32_t(type));
        addStubField(offset, StubField::Type::RawWord);
    }
    void loadTypedObjectResult(ObjOperandId obj, uint32_t offset, TypedThingLayout layout,
                               uint32_t typeDescr) {
        MOZ_ASSERT(uint32_t(layout) <= UINT8_MAX);
        MOZ_ASSERT(typeDescr <= UINT8_MAX);
        writeOpWithOperandId(CacheOp::LoadTypedObjectResult, obj);
        buffer_.writeByte(uint32_t(layout));
        buffer_.writeByte(typeDescr);
        addStubField(offset, StubField::Type::RawWord);
    }
    void loadInt32ArrayLengthResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::LoadInt32ArrayLengthResult, obj);
    }
    void loadArgumentsObjectLengthResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::LoadArgumentsObjectLengthResult, obj);
    }
    void loadFunctionLengthResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::LoadFunctionLengthResult, obj);
    }
    void loadArgumentsObjectArgResult(ObjOperandId obj, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadArgumentsObjectArgResult, obj);
        writeOperandId(index);
    }
    void loadDenseElementResult(ObjOperandId obj, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadDenseElementResult, obj);
        writeOperandId(index);
    }
    void loadDenseElementHoleResult(ObjOperandId obj, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadDenseElementHoleResult, obj);
        writeOperandId(index);
    }
    void loadDenseElementExistsResult(ObjOperandId obj, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadDenseElementExistsResult, obj);
        writeOperandId(index);
    }
    void loadTypedElementExistsResult(ObjOperandId obj, Int32OperandId index, TypedThingLayout layout) {
        writeOpWithOperandId(CacheOp::LoadTypedElementExistsResult, obj);
        writeOperandId(index);
        buffer_.writeByte(uint32_t(layout));
    }
    void loadDenseElementHoleExistsResult(ObjOperandId obj, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadDenseElementHoleExistsResult, obj);
        writeOperandId(index);
    }
    void loadTypedElementResult(ObjOperandId obj, Int32OperandId index, TypedThingLayout layout,
                                Scalar::Type elementType) {
        writeOpWithOperandId(CacheOp::LoadTypedElementResult, obj);
        writeOperandId(index);
        buffer_.writeByte(uint32_t(layout));
        buffer_.writeByte(uint32_t(elementType));
    }
    void loadStringLengthResult(StringOperandId str) {
        writeOpWithOperandId(CacheOp::LoadStringLengthResult, str);
    }
    void loadStringCharResult(StringOperandId str, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::LoadStringCharResult, str);
        writeOperandId(index);
    }
    void callScriptedGetterResult(ObjOperandId obj, JSFunction* getter) {
        writeOpWithOperandId(CacheOp::CallScriptedGetterResult, obj);
        addStubField(uintptr_t(getter), StubField::Type::JSObject);
    }
    void callNativeGetterResult(ObjOperandId obj, JSFunction* getter) {
        writeOpWithOperandId(CacheOp::CallNativeGetterResult, obj);
        addStubField(uintptr_t(getter), StubField::Type::JSObject);
    }
    void callProxyGetResult(ObjOperandId obj, jsid id) {
        writeOpWithOperandId(CacheOp::CallProxyGetResult, obj);
        addStubField(uintptr_t(JSID_BITS(id)), StubField::Type::Id);
    }
    void callProxyGetByValueResult(ObjOperandId obj, ValOperandId idVal) {
        writeOpWithOperandId(CacheOp::CallProxyGetByValueResult, obj);
        writeOperandId(idVal);
    }
    void callProxyHasPropResult(ObjOperandId obj, ValOperandId idVal, bool hasOwn) {
        writeOpWithOperandId(CacheOp::CallProxyHasPropResult, obj);
        writeOperandId(idVal);
        buffer_.writeByte(uint32_t(hasOwn));
    }
    void callObjectHasSparseElementResult(ObjOperandId obj, Int32OperandId index) {
        writeOpWithOperandId(CacheOp::CallObjectHasSparseElementResult, obj);
        writeOperandId(index);
    }
    void loadEnvironmentFixedSlotResult(ObjOperandId obj, size_t offset) {
        writeOpWithOperandId(CacheOp::LoadEnvironmentFixedSlotResult, obj);
        addStubField(offset, StubField::Type::RawWord);
    }
    void loadEnvironmentDynamicSlotResult(ObjOperandId obj, size_t offset) {
        writeOpWithOperandId(CacheOp::LoadEnvironmentDynamicSlotResult, obj);
        addStubField(offset, StubField::Type::RawWord);
    }
    void loadObjectResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::LoadObjectResult, obj);
    }
    void loadInstanceOfObjectResult(ValOperandId lhs, ObjOperandId protoId, uint32_t slot) {
        writeOp(CacheOp::LoadInstanceOfObjectResult);
        writeOperandId(lhs);
        writeOperandId(protoId);
    }
    void loadTypeOfObjectResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::LoadTypeOfObjectResult, obj);
    }
    void loadInt32TruthyResult(ValOperandId integer) {
        writeOpWithOperandId(CacheOp::LoadInt32TruthyResult, integer);
    }
    void loadDoubleTruthyResult(ValOperandId dbl) {
        writeOpWithOperandId(CacheOp::LoadDoubleTruthyResult, dbl);
    }
    void loadStringTruthyResult(StringOperandId str) {
        writeOpWithOperandId(CacheOp::LoadStringTruthyResult, str);
    }
    void loadObjectTruthyResult(ObjOperandId obj) {
        writeOpWithOperandId(CacheOp::LoadObjectTruthyResult, obj);
    }
    void loadValueResult(const Value& val) {
        writeOp(CacheOp::LoadValueResult);
        addStubField(val.asRawBits(), StubField::Type::Value);
    }
    void callStringSplitResult(StringOperandId str, StringOperandId sep, ObjectGroup* group) {
        writeOp(CacheOp::CallStringSplitResult);
        writeOperandId(str);
        writeOperandId(sep);
        addStubField(uintptr_t(group), StubField::Type::ObjectGroup);
    }

    void compareStringResult(uint32_t op, StringOperandId lhs, StringOperandId rhs) {
        writeOpWithOperandId(CacheOp::CompareStringResult, lhs);
        writeOperandId(rhs);
        buffer_.writeByte(uint32_t(op));
    }
    void compareObjectResult(uint32_t op, ObjOperandId lhs, ObjOperandId rhs) {
        writeOpWithOperandId(CacheOp::CompareObjectResult, lhs);
        writeOperandId(rhs);
        buffer_.writeByte(uint32_t(op));
    }
    void compareSymbolResult(uint32_t op, SymbolOperandId lhs, SymbolOperandId rhs) {
        writeOpWithOperandId(CacheOp::CompareSymbolResult, lhs);
        writeOperandId(rhs);
        buffer_.writeByte(uint32_t(op));
    }

    void callPrintString(const char* str) {
        writeOp(CacheOp::CallPrintString);
        writePointer(const_cast<char*>(str));
    }
    void breakpoint() {
        writeOp(CacheOp::Breakpoint);
    }

    void typeMonitorResult() {
        writeOp(CacheOp::TypeMonitorResult);
    }
    void returnFromIC() {
        writeOp(CacheOp::ReturnFromIC);
    }
    void wrapResult() {
        writeOp(CacheOp::WrapResult);
    }
};

class CacheIRStubInfo;

// Helper class for reading CacheIR bytecode.
class MOZ_RAII CacheIRReader
{
    CompactBufferReader buffer_;

    CacheIRReader(const CacheIRReader&) = delete;
    CacheIRReader& operator=(const CacheIRReader&) = delete;

  public:
    CacheIRReader(const uint8_t* start, const uint8_t* end)
      : buffer_(start, end)
    {}
    explicit CacheIRReader(const CacheIRWriter& writer)
      : CacheIRReader(writer.codeStart(), writer.codeEnd())
    {}
    explicit CacheIRReader(const CacheIRStubInfo* stubInfo);

    bool more() const { return buffer_.more(); }

    CacheOp readOp() {
        return CacheOp(buffer_.readByte());
    }

    // Skip data not currently used.
    void skip() { buffer_.readByte(); }

    ValOperandId valOperandId() { return ValOperandId(buffer_.readByte()); }
    ValueTagOperandId valueTagOperandId() { return ValueTagOperandId(buffer_.readByte()); }
    ObjOperandId objOperandId() { return ObjOperandId(buffer_.readByte()); }
    StringOperandId stringOperandId() { return StringOperandId(buffer_.readByte()); }
    SymbolOperandId symbolOperandId() { return SymbolOperandId(buffer_.readByte()); }
    Int32OperandId int32OperandId() { return Int32OperandId(buffer_.readByte()); }

    uint32_t stubOffset() { return buffer_.readByte() * sizeof(uintptr_t); }
    GuardClassKind guardClassKind() { return GuardClassKind(buffer_.readByte()); }
    JSValueType valueType() { return JSValueType(buffer_.readByte()); }
    TypedThingLayout typedThingLayout() { return TypedThingLayout(buffer_.readByte()); }
    Scalar::Type scalarType() { return Scalar::Type(buffer_.readByte()); }
    uint32_t typeDescrKey() { return buffer_.readByte(); }
    JSWhyMagic whyMagic() { return JSWhyMagic(buffer_.readByte()); }
    JSOp jsop() { return JSOp(buffer_.readByte()); }
    int32_t int32Immediate() { return buffer_.readSigned(); }
    uint32_t uint32Immediate() { return buffer_.readUnsigned(); }
    void* pointer() { return buffer_.readRawPointer(); }

    ReferenceTypeDescr::Type referenceTypeDescrType() {
        return ReferenceTypeDescr::Type(buffer_.readByte());
    }

    uint8_t readByte() {
        return buffer_.readByte();
    }
    bool readBool() {
        uint8_t b = buffer_.readByte();
        MOZ_ASSERT(b <= 1);
        return bool(b);
    }

    bool matchOp(CacheOp op) {
        const uint8_t* pos = buffer_.currentPosition();
        if (readOp() == op)
            return true;
        buffer_.seek(pos, 0);
        return false;
    }
    bool matchOp(CacheOp op, OperandId id) {
        const uint8_t* pos = buffer_.currentPosition();
        if (readOp() == op && buffer_.readByte() == id.id())
            return true;
        buffer_.seek(pos, 0);
        return false;
    }
    bool matchOpEither(CacheOp op1, CacheOp op2) {
        const uint8_t* pos = buffer_.currentPosition();
        CacheOp op = readOp();
        if (op == op1 || op == op2)
            return true;
        buffer_.seek(pos, 0);
        return false;
    }
};

class MOZ_RAII IRGenerator
{
  protected:
    CacheIRWriter writer;
    JSContext* cx_;
    HandleScript script_;
    jsbytecode* pc_;
    CacheKind cacheKind_;
    ICState::Mode mode_;

    IRGenerator(const IRGenerator&) = delete;
    IRGenerator& operator=(const IRGenerator&) = delete;

    bool maybeGuardInt32Index(const Value& index, ValOperandId indexId,
                              uint32_t* int32Index, Int32OperandId* int32IndexId);

    ObjOperandId guardDOMProxyExpandoObjectAndShape(JSObject* obj, ObjOperandId objId,
                                                    const Value& expandoVal, JSObject* expandoObj);

    void emitIdGuard(ValOperandId valId, jsid id);

    friend class CacheIRSpewer;

  public:
    explicit IRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, CacheKind cacheKind,
                         ICState::Mode mode);

    const CacheIRWriter& writerRef() const { return writer; }
    CacheKind cacheKind() const { return cacheKind_; }

    static constexpr char* NotAttached = nullptr;
};

// Flags used to describe what values a GetProperty cache may produce.
enum class GetPropertyResultFlags {
    None            = 0,

    // Values produced by this cache will go through a type barrier,
    // so the cache may produce any type of value that is compatible with its
    // result operand.
    Monitored       = 1 << 0,

    // Whether particular primitives may be produced by this cache.
    AllowUndefined  = 1 << 1,
    AllowInt32      = 1 << 2,
    AllowDouble     = 1 << 3,

    All             = Monitored | AllowUndefined | AllowInt32 | AllowDouble
};

static inline bool operator&(GetPropertyResultFlags a, GetPropertyResultFlags b)
{
    return static_cast<int>(a) & static_cast<int>(b);
}

static inline GetPropertyResultFlags operator|(GetPropertyResultFlags a, GetPropertyResultFlags b)
{
    return static_cast<GetPropertyResultFlags>(static_cast<int>(a) | static_cast<int>(b));
}

static inline GetPropertyResultFlags& operator|=(GetPropertyResultFlags& lhs, GetPropertyResultFlags b)
{
    lhs = lhs | b;
    return lhs;
}

// GetPropIRGenerator generates CacheIR for a GetProp IC.
class MOZ_RAII GetPropIRGenerator : public IRGenerator
{
    HandleValue val_;
    HandleValue idVal_;
    HandleValue receiver_;
    bool* isTemporarilyUnoptimizable_;
    GetPropertyResultFlags resultFlags_;

    enum class PreliminaryObjectAction { None, Unlink, NotePreliminary };
    PreliminaryObjectAction preliminaryObjectAction_;

    bool tryAttachNative(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachUnboxed(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachUnboxedExpando(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachTypedObject(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachObjectLength(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachModuleNamespace(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachWindowProxy(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachCrossCompartmentWrapper(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachXrayCrossCompartmentWrapper(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachFunction(HandleObject obj, ObjOperandId objId, HandleId id);

    bool tryAttachGenericProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                               bool handleDOMProxies);
    bool tryAttachDOMProxyExpando(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachDOMProxyShadowed(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachDOMProxyUnshadowed(HandleObject obj, ObjOperandId objId, HandleId id);
    bool tryAttachProxy(HandleObject obj, ObjOperandId objId, HandleId id);

    bool tryAttachPrimitive(ValOperandId valId, HandleId id);
    bool tryAttachStringChar(ValOperandId valId, ValOperandId indexId);
    bool tryAttachStringLength(ValOperandId valId, HandleId id);
    bool tryAttachMagicArgumentsName(ValOperandId valId, HandleId id);

    bool tryAttachMagicArgument(ValOperandId valId, ValOperandId indexId);
    bool tryAttachArgumentsObjectArg(HandleObject obj, ObjOperandId objId,
                                     Int32OperandId indexId);

    bool tryAttachDenseElement(HandleObject obj, ObjOperandId objId,
                               uint32_t index, Int32OperandId indexId);
    bool tryAttachDenseElementHole(HandleObject obj, ObjOperandId objId,
                                   uint32_t index, Int32OperandId indexId);
    bool tryAttachTypedElement(HandleObject obj, ObjOperandId objId,
                               uint32_t index, Int32OperandId indexId);

    bool tryAttachProxyElement(HandleObject obj, ObjOperandId objId);

    void attachMegamorphicNativeSlot(ObjOperandId objId, jsid id, bool handleMissing);

    ValOperandId getElemKeyValueId() const {
        MOZ_ASSERT(cacheKind_ == CacheKind::GetElem || cacheKind_ == CacheKind::GetElemSuper);
        return ValOperandId(1);
    }

    ValOperandId getSuperReceiverValueId() const {
        if (cacheKind_ == CacheKind::GetPropSuper)
            return ValOperandId(1);

        MOZ_ASSERT(cacheKind_ == CacheKind::GetElemSuper);
        return ValOperandId(2);
    }

    bool isSuper() const {
        return (cacheKind_ == CacheKind::GetPropSuper ||
                cacheKind_ == CacheKind::GetElemSuper);
    }

    // No pc if idempotent, as there can be multiple bytecode locations
    // due to GVN.
    bool idempotent() const { return pc_ == nullptr; }

    // If this is a GetElem cache, emit instructions to guard the incoming Value
    // matches |id|.
    void maybeEmitIdGuard(jsid id);

    void trackAttached(const char* name);

  public:
    GetPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, CacheKind cacheKind,
                       ICState::Mode mode, bool* isTemporarilyUnoptimizable, HandleValue val,
                       HandleValue idVal, HandleValue receiver,
                       GetPropertyResultFlags resultFlags);

    bool tryAttachStub();
    bool tryAttachIdempotentStub();

    bool shouldUnlinkPreliminaryObjectStubs() const {
        return preliminaryObjectAction_ == PreliminaryObjectAction::Unlink;
    }
    bool shouldNotePreliminaryObjectStub() const {
        return preliminaryObjectAction_ == PreliminaryObjectAction::NotePreliminary;
    }
};

// GetNameIRGenerator generates CacheIR for a GetName IC.
class MOZ_RAII GetNameIRGenerator : public IRGenerator
{
    HandleObject env_;
    HandlePropertyName name_;

    bool tryAttachGlobalNameValue(ObjOperandId objId, HandleId id);
    bool tryAttachGlobalNameGetter(ObjOperandId objId, HandleId id);
    bool tryAttachEnvironmentName(ObjOperandId objId, HandleId id);

    void trackAttached(const char* name);

  public:
    GetNameIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, ICState::Mode mode,
                       HandleObject env, HandlePropertyName name);

    bool tryAttachStub();
};

// BindNameIRGenerator generates CacheIR for a BindName IC.
class MOZ_RAII BindNameIRGenerator : public IRGenerator
{
    HandleObject env_;
    HandlePropertyName name_;

    bool tryAttachGlobalName(ObjOperandId objId, HandleId id);
    bool tryAttachEnvironmentName(ObjOperandId objId, HandleId id);

    void trackAttached(const char* name);

  public:
    BindNameIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, ICState::Mode mode,
                        HandleObject env, HandlePropertyName name);

    bool tryAttachStub();
};

// Information used by SetProp/SetElem stubs to check/update property types.
class MOZ_RAII PropertyTypeCheckInfo
{
    RootedObjectGroup group_;
    RootedId id_;
    bool needsTypeBarrier_;

    PropertyTypeCheckInfo(const PropertyTypeCheckInfo&) = delete;
    void operator=(const PropertyTypeCheckInfo&) = delete;

  public:
    PropertyTypeCheckInfo(JSContext* cx, bool needsTypeBarrier)
      : group_(cx), id_(cx), needsTypeBarrier_(needsTypeBarrier)
    {}

    bool needsTypeBarrier() const { return needsTypeBarrier_; }
    bool isSet() const { return group_ != nullptr; }
    ObjectGroup* group() const { MOZ_ASSERT(isSet()); return group_; }
    jsid id() const { MOZ_ASSERT(isSet()); return id_; }

    void set(ObjectGroup* group, jsid id) {
        MOZ_ASSERT(!group_);
        MOZ_ASSERT(group);
        if (needsTypeBarrier_) {
            group_ = group;
            id_ = id;
        }
    }
};

// SetPropIRGenerator generates CacheIR for a SetProp IC.
class MOZ_RAII SetPropIRGenerator : public IRGenerator
{
    HandleValue lhsVal_;
    HandleValue idVal_;
    HandleValue rhsVal_;
    bool* isTemporarilyUnoptimizable_;
    PropertyTypeCheckInfo typeCheckInfo_;

    enum class PreliminaryObjectAction { None, Unlink, NotePreliminary };
    PreliminaryObjectAction preliminaryObjectAction_;
    bool attachedTypedArrayOOBStub_;

    bool maybeHasExtraIndexedProps_;

    ValOperandId setElemKeyValueId() const {
        MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
        return ValOperandId(1);
    }
    ValOperandId rhsValueId() const {
        if (cacheKind_ == CacheKind::SetProp)
            return ValOperandId(1);
        MOZ_ASSERT(cacheKind_ == CacheKind::SetElem);
        return ValOperandId(2);
    }

    // If this is a SetElem cache, emit instructions to guard the incoming Value
    // matches |id|.
    void maybeEmitIdGuard(jsid id);

    bool tryAttachNativeSetSlot(HandleObject obj, ObjOperandId objId, HandleId id,
                                 ValOperandId rhsId);
    bool tryAttachUnboxedExpandoSetSlot(HandleObject obj, ObjOperandId objId, HandleId id,
                                        ValOperandId rhsId);
    bool tryAttachUnboxedProperty(HandleObject obj, ObjOperandId objId, HandleId id,
                                  ValOperandId rhsId);
    bool tryAttachTypedObjectProperty(HandleObject obj, ObjOperandId objId, HandleId id,
                                      ValOperandId rhsId);
    bool tryAttachSetter(HandleObject obj, ObjOperandId objId, HandleId id,
                         ValOperandId rhsId);
    bool tryAttachSetArrayLength(HandleObject obj, ObjOperandId objId, HandleId id,
                                 ValOperandId rhsId);
    bool tryAttachWindowProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                              ValOperandId rhsId);

    bool tryAttachSetDenseElement(HandleObject obj, ObjOperandId objId, uint32_t index,
                                  Int32OperandId indexId, ValOperandId rhsId);
    bool tryAttachSetTypedElement(HandleObject obj, ObjOperandId objId, uint32_t index,
                                  Int32OperandId indexId, ValOperandId rhsId);

    bool tryAttachSetDenseElementHole(HandleObject obj, ObjOperandId objId, uint32_t index,
                                      Int32OperandId indexId, ValOperandId rhsId);

    bool tryAttachGenericProxy(HandleObject obj, ObjOperandId objId, HandleId id,
                               ValOperandId rhsId, bool handleDOMProxies);
    bool tryAttachDOMProxyShadowed(HandleObject obj, ObjOperandId objId, HandleId id,
                                   ValOperandId rhsId);
    bool tryAttachDOMProxyUnshadowed(HandleObject obj, ObjOperandId objId, HandleId id,
                                     ValOperandId rhsId);
    bool tryAttachDOMProxyExpando(HandleObject obj, ObjOperandId objId, HandleId id,
                                  ValOperandId rhsId);
    bool tryAttachProxy(HandleObject obj, ObjOperandId objId, HandleId id, ValOperandId rhsId);
    bool tryAttachProxyElement(HandleObject obj, ObjOperandId objId, ValOperandId rhsId);
    bool tryAttachMegamorphicSetElement(HandleObject obj, ObjOperandId objId, ValOperandId rhsId);

  public:
    SetPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, CacheKind cacheKind,
                       ICState::Mode mode, bool* isTemporarilyUnoptimizable, HandleValue lhsVal,
                       HandleValue idVal, HandleValue rhsVal, bool needsTypeBarrier = true,
                       bool maybeHasExtraIndexedProps = true);

    bool tryAttachStub();
    bool tryAttachAddSlotStub(HandleObjectGroup oldGroup, HandleShape oldShape);
    void trackAttached(const char* name);

    bool shouldUnlinkPreliminaryObjectStubs() const {
        return preliminaryObjectAction_ == PreliminaryObjectAction::Unlink;
    }
    bool shouldNotePreliminaryObjectStub() const {
        return preliminaryObjectAction_ == PreliminaryObjectAction::NotePreliminary;
    }

    const PropertyTypeCheckInfo* typeCheckInfo() const {
        return &typeCheckInfo_;
    }

    bool attachedTypedArrayOOBStub() const {
        return attachedTypedArrayOOBStub_;
    }
};

// HasPropIRGenerator generates CacheIR for a HasProp IC. Used for
// CacheKind::In / CacheKind::HasOwn.
class MOZ_RAII HasPropIRGenerator : public IRGenerator
{
    HandleValue val_;
    HandleValue idVal_;

    bool tryAttachDense(HandleObject obj, ObjOperandId objId,
                        uint32_t index, Int32OperandId indexId);
    bool tryAttachDenseHole(HandleObject obj, ObjOperandId objId,
                            uint32_t index, Int32OperandId indexId);
    bool tryAttachTypedArray(HandleObject obj, ObjOperandId objId,
                             Int32OperandId indexId);
    bool tryAttachSparse(HandleObject obj, ObjOperandId objId,
                         Int32OperandId indexId);
    bool tryAttachNamedProp(HandleObject obj, ObjOperandId objId,
                            HandleId key, ValOperandId keyId);
    bool tryAttachMegamorphic(ObjOperandId objId, ValOperandId keyId);
    bool tryAttachNative(JSObject* obj, ObjOperandId objId,
                         jsid key, ValOperandId keyId,
                         PropertyResult prop, JSObject* holder);
    bool tryAttachUnboxed(JSObject* obj, ObjOperandId objId,
                          jsid key, ValOperandId keyId);
    bool tryAttachUnboxedExpando(JSObject* obj, ObjOperandId objId,
                                 jsid key, ValOperandId keyId);
    bool tryAttachTypedObject(JSObject* obj, ObjOperandId objId,
                              jsid key, ValOperandId keyId);
    bool tryAttachSlotDoesNotExist(JSObject* obj, ObjOperandId objId,
                                   jsid key, ValOperandId keyId);
    bool tryAttachDoesNotExist(HandleObject obj, ObjOperandId objId,
                               HandleId key, ValOperandId keyId);
    bool tryAttachProxyElement(HandleObject obj, ObjOperandId objId,
                               ValOperandId keyId);

    void trackAttached(const char* name);

  public:
    // NOTE: Argument order is PROPERTY, OBJECT
    HasPropIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc, CacheKind cacheKind,
                       ICState::Mode mode, HandleValue idVal, HandleValue val);

    bool tryAttachStub();
};

class MOZ_RAII InstanceOfIRGenerator : public IRGenerator
{
    HandleValue lhsVal_;
    HandleObject rhsObj_;

    void trackAttached(const char* name);
  public:
    InstanceOfIRGenerator(JSContext*, HandleScript, jsbytecode*, ICState::Mode,
                          HandleValue, HandleObject);

    bool tryAttachStub();
};

class MOZ_RAII TypeOfIRGenerator : public IRGenerator
{
    HandleValue val_;

    bool tryAttachPrimitive(ValOperandId valId);
    bool tryAttachObject(ValOperandId valId);

  public:
    TypeOfIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState::Mode mode, HandleValue value);

    bool tryAttachStub();
};

class MOZ_RAII GetIteratorIRGenerator : public IRGenerator
{
    HandleValue val_;

    bool tryAttachNativeIterator(ObjOperandId objId, HandleObject obj);

  public:
    GetIteratorIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState::Mode mode,
                           HandleValue value);

    bool tryAttachStub();
};

class MOZ_RAII CallIRGenerator : public IRGenerator
{
  private:
    JSOp op_;
    uint32_t argc_;
    HandleValue callee_;
    HandleValue thisval_;
    HandleValueArray args_;
    PropertyTypeCheckInfo typeCheckInfo_;
    BaselineCacheIRStubKind cacheIRStubKind_;

    bool tryAttachStringSplit();
    bool tryAttachArrayPush();
    bool tryAttachArrayJoin();

    void trackAttached(const char* name);

  public:
    CallIRGenerator(JSContext* cx, HandleScript script, jsbytecode* pc,
                    JSOp op, ICState::Mode mode,
                    uint32_t argc, HandleValue callee, HandleValue thisval,
                    HandleValueArray args);

    bool tryAttachStub();

    BaselineCacheIRStubKind cacheIRStubKind() const {
        return cacheIRStubKind_;
    }

    const PropertyTypeCheckInfo* typeCheckInfo() const {
        return &typeCheckInfo_;
    }
};

class MOZ_RAII CompareIRGenerator : public IRGenerator
{
    JSOp op_;
    HandleValue lhsVal_;
    HandleValue rhsVal_;

    bool tryAttachString(ValOperandId lhsId, ValOperandId rhsId);
    bool tryAttachObject(ValOperandId lhsId, ValOperandId rhsId);
    bool tryAttachSymbol(ValOperandId lhsId, ValOperandId rhsId);
    bool tryAttachStrictDifferentTypes(ValOperandId lhsId, ValOperandId rhsId);

    void trackAttached(const char* name);

  public:
    CompareIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState::Mode mode,
                       JSOp op, HandleValue lhsVal, HandleValue rhsVal);

    bool tryAttachStub();
};

class MOZ_RAII ToBoolIRGenerator : public IRGenerator
{
    HandleValue val_;

    bool tryAttachInt32();
    bool tryAttachDouble();
    bool tryAttachString();
    bool tryAttachSymbol();
    bool tryAttachNullOrUndefined();
    bool tryAttachObject();

    void trackAttached(const char* name);

  public:
    ToBoolIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState::Mode mode,
                      HandleValue val);

    bool tryAttachStub();
};

class MOZ_RAII GetIntrinsicIRGenerator : public IRGenerator
{
    HandleValue val_;

    void trackAttached(const char* name);

  public:
    GetIntrinsicIRGenerator(JSContext* cx, HandleScript, jsbytecode* pc, ICState::Mode,
                            HandleValue val);

    bool tryAttachStub();
};

} // namespace jit
} // namespace js

#endif /* jit_CacheIR_h */
