/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_types_h
#define wasm_types_h

#include "mozilla/Alignment.h"
#include "mozilla/Atomics.h"
#include "mozilla/EnumeratedArray.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Maybe.h"
#include "mozilla/Move.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Unused.h"

#include "NamespaceImports.h"

#include "ds/LifoAlloc.h"
#include "jit/IonTypes.h"
#include "js/RefCounted.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"
#include "vm/MallocProvider.h"
#include "wasm/WasmBinaryConstants.h"

namespace js {

namespace jit {
    struct BaselineScript;
    enum class RoundingMode;
}

// This is a widespread header, so lets keep out the core wasm impl types.

class WasmMemoryObject;
typedef GCPtr<WasmMemoryObject*> GCPtrWasmMemoryObject;
typedef Rooted<WasmMemoryObject*> RootedWasmMemoryObject;
typedef Handle<WasmMemoryObject*> HandleWasmMemoryObject;
typedef MutableHandle<WasmMemoryObject*> MutableHandleWasmMemoryObject;

class WasmModuleObject;
typedef Rooted<WasmModuleObject*> RootedWasmModuleObject;
typedef Handle<WasmModuleObject*> HandleWasmModuleObject;
typedef MutableHandle<WasmModuleObject*> MutableHandleWasmModuleObject;

class WasmInstanceObject;
typedef GCVector<WasmInstanceObject*> WasmInstanceObjectVector;
typedef Rooted<WasmInstanceObject*> RootedWasmInstanceObject;
typedef Handle<WasmInstanceObject*> HandleWasmInstanceObject;
typedef MutableHandle<WasmInstanceObject*> MutableHandleWasmInstanceObject;

class WasmTableObject;
typedef Rooted<WasmTableObject*> RootedWasmTableObject;
typedef Handle<WasmTableObject*> HandleWasmTableObject;
typedef MutableHandle<WasmTableObject*> MutableHandleWasmTableObject;

namespace wasm {

using mozilla::Atomic;
using mozilla::DebugOnly;
using mozilla::EnumeratedArray;
using mozilla::Maybe;
using mozilla::Move;
using mozilla::MallocSizeOf;
using mozilla::Nothing;
using mozilla::PodZero;
using mozilla::PodCopy;
using mozilla::PodEqual;
using mozilla::Some;
using mozilla::Unused;

typedef Vector<uint32_t, 0, SystemAllocPolicy> Uint32Vector;
typedef Vector<uint8_t, 0, SystemAllocPolicy> Bytes;
typedef UniquePtr<Bytes> UniqueBytes;
typedef UniquePtr<const Bytes> UniqueConstBytes;
typedef Vector<char, 0, SystemAllocPolicy> UTF8Bytes;

typedef int8_t I8x16[16];
typedef int16_t I16x8[8];
typedef int32_t I32x4[4];
typedef float F32x4[4];

class Code;
class DebugState;
class GeneratedSourceMap;
class Memory;
class Module;
class Instance;
class Table;

// To call Vector::podResizeToFit, a type must specialize mozilla::IsPod
// which is pretty verbose to do within js::wasm, so factor that process out
// into a macro.

#define WASM_DECLARE_POD_VECTOR(Type, VectorName)                               \
} } namespace mozilla {                                                         \
template <> struct IsPod<js::wasm::Type> : TrueType {};                         \
} namespace js { namespace wasm {                                               \
typedef Vector<Type, 0, SystemAllocPolicy> VectorName;

// A wasm Module and everything it contains must support serialization and
// deserialization. Some data can be simply copied as raw bytes and,
// as a convention, is stored in an inline CacheablePod struct. Everything else
// should implement the below methods which are called recusively by the
// containing Module.

#define WASM_DECLARE_SERIALIZABLE(Type)                                         \
    size_t serializedSize() const;                                              \
    uint8_t* serialize(uint8_t* cursor) const;                                  \
    const uint8_t* deserialize(const uint8_t* cursor);                          \
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

#define WASM_DECLARE_SERIALIZABLE_VIRTUAL(Type)                                 \
    virtual size_t serializedSize() const;                                      \
    virtual uint8_t* serialize(uint8_t* cursor) const;                          \
    virtual const uint8_t* deserialize(const uint8_t* cursor);                  \
    virtual size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

#define WASM_DECLARE_SERIALIZABLE_OVERRIDE(Type)                                \
    size_t serializedSize() const override;                                     \
    uint8_t* serialize(uint8_t* cursor) const override;                         \
    const uint8_t* deserialize(const uint8_t* cursor) override;                 \
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const override;

// This reusable base class factors out the logic for a resource that is shared
// by multiple instances/modules but should only be counted once when computing
// about:memory stats.

template <class T>
struct ShareableBase : AtomicRefCounted<T>
{
    using SeenSet = HashSet<const T*, DefaultHasher<const T*>, SystemAllocPolicy>;

    size_t sizeOfIncludingThisIfNotSeen(MallocSizeOf mallocSizeOf, SeenSet* seen) const {
        const T* self = static_cast<const T*>(this);
        typename SeenSet::AddPtr p = seen->lookupForAdd(self);
        if (p)
            return 0;
        bool ok = seen->add(p, self);
        (void)ok;  // oh well
        return mallocSizeOf(self) + self->sizeOfExcludingThis(mallocSizeOf);
    }
};

// ValType utilities

static inline unsigned
SizeOf(ValType vt)
{
    switch (vt) {
      case ValType::I32:
      case ValType::F32:
        return 4;
      case ValType::I64:
      case ValType::F64:
        return 8;
      case ValType::I8x16:
      case ValType::I16x8:
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B8x16:
      case ValType::B16x8:
      case ValType::B32x4:
        return 16;
      default:
        MOZ_CRASH("Invalid ValType");
    }
}

static inline bool
IsSimdType(ValType vt)
{
    switch (vt) {
      case ValType::I8x16:
      case ValType::I16x8:
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B8x16:
      case ValType::B16x8:
      case ValType::B32x4:
        return true;
      default:
        return false;
    }
}

static inline uint32_t
NumSimdElements(ValType vt)
{
    MOZ_ASSERT(IsSimdType(vt));
    switch (vt) {
      case ValType::I8x16:
      case ValType::B8x16:
        return 16;
      case ValType::I16x8:
      case ValType::B16x8:
        return 8;
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B32x4:
        return 4;
     default:
        MOZ_CRASH("Unhandled SIMD type");
    }
}

static inline ValType
SimdElementType(ValType vt)
{
    MOZ_ASSERT(IsSimdType(vt));
    switch (vt) {
      case ValType::I8x16:
      case ValType::I16x8:
      case ValType::I32x4:
        return ValType::I32;
      case ValType::F32x4:
        return ValType::F32;
      case ValType::B8x16:
      case ValType::B16x8:
      case ValType::B32x4:
        return ValType::I32;
     default:
        MOZ_CRASH("Unhandled SIMD type");
    }
}

static inline ValType
SimdBoolType(ValType vt)
{
    MOZ_ASSERT(IsSimdType(vt));
    switch (vt) {
      case ValType::I8x16:
      case ValType::B8x16:
        return ValType::B8x16;
      case ValType::I16x8:
      case ValType::B16x8:
        return ValType::B16x8;
      case ValType::I32x4:
      case ValType::F32x4:
      case ValType::B32x4:
        return ValType::B32x4;
     default:
        MOZ_CRASH("Unhandled SIMD type");
    }
}

static inline bool
IsSimdBoolType(ValType vt)
{
    return vt == ValType::B8x16 || vt == ValType::B16x8 || vt == ValType::B32x4;
}

static inline jit::MIRType
ToMIRType(ValType vt)
{
    switch (vt) {
      case ValType::I32: return jit::MIRType::Int32;
      case ValType::I64: return jit::MIRType::Int64;
      case ValType::F32: return jit::MIRType::Float32;
      case ValType::F64: return jit::MIRType::Double;
      case ValType::I8x16: return jit::MIRType::Int8x16;
      case ValType::I16x8: return jit::MIRType::Int16x8;
      case ValType::I32x4: return jit::MIRType::Int32x4;
      case ValType::F32x4: return jit::MIRType::Float32x4;
      case ValType::B8x16: return jit::MIRType::Bool8x16;
      case ValType::B16x8: return jit::MIRType::Bool16x8;
      case ValType::B32x4: return jit::MIRType::Bool32x4;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("bad type");
}

// The ExprType enum represents the type of a WebAssembly expression or return
// value and may either be a value type or void. Soon, expression types will be
// generalized to a list of ValType and this enum will go away, replaced,
// wherever it is used, by a varU32 + list of ValType.

enum class ExprType
{
    Void  = uint8_t(TypeCode::BlockVoid),

    I32   = uint8_t(TypeCode::I32),
    I64   = uint8_t(TypeCode::I64),
    F32   = uint8_t(TypeCode::F32),
    F64   = uint8_t(TypeCode::F64),

    I8x16 = uint8_t(TypeCode::I8x16),
    I16x8 = uint8_t(TypeCode::I16x8),
    I32x4 = uint8_t(TypeCode::I32x4),
    F32x4 = uint8_t(TypeCode::F32x4),
    B8x16 = uint8_t(TypeCode::B8x16),
    B16x8 = uint8_t(TypeCode::B16x8),
    B32x4 = uint8_t(TypeCode::B32x4),

    Limit = uint8_t(TypeCode::Limit)
};

static inline bool
IsVoid(ExprType et)
{
    return et == ExprType::Void;
}

static inline ValType
NonVoidToValType(ExprType et)
{
    MOZ_ASSERT(!IsVoid(et));
    return ValType(et);
}

static inline ExprType
ToExprType(ValType vt)
{
    return ExprType(vt);
}

static inline bool
IsSimdType(ExprType et)
{
    return IsVoid(et) ? false : IsSimdType(ValType(et));
}

static inline jit::MIRType
ToMIRType(ExprType et)
{
    return IsVoid(et) ? jit::MIRType::None : ToMIRType(ValType(et));
}

static inline const char*
ToCString(ExprType type)
{
    switch (type) {
      case ExprType::Void:  return "void";
      case ExprType::I32:   return "i32";
      case ExprType::I64:   return "i64";
      case ExprType::F32:   return "f32";
      case ExprType::F64:   return "f64";
      case ExprType::I8x16: return "i8x16";
      case ExprType::I16x8: return "i16x8";
      case ExprType::I32x4: return "i32x4";
      case ExprType::F32x4: return "f32x4";
      case ExprType::B8x16: return "b8x16";
      case ExprType::B16x8: return "b16x8";
      case ExprType::B32x4: return "b32x4";
      case ExprType::Limit:;
    }
    MOZ_CRASH("bad expression type");
}

static inline const char*
ToCString(ValType type)
{
    return ToCString(ToExprType(type));
}

// Code can be compiled either with the Baseline compiler or the Ion compiler,
// and tier-variant data are tagged with the Tier value.
//
// A tier value is used to request tier-variant aspects of code, metadata, or
// linkdata.  The tiers are normally explicit (Baseline and Ion); implicit tiers
// can be obtained through accessors on Code objects (eg, stableTier).

enum class Tier
{
    Baseline,
    Debug = Baseline,
    Ion,
    Serialized = Ion
};

// The CompileMode controls how compilation of a module is performed (notably,
// how many times we compile it).

enum class CompileMode
{
    Once,
    Tier1,
    Tier2
};

// Typed enum for whether debugging is enabled.

enum class DebugEnabled
{
    False,
    True
};

// Iterator over tiers present in a tiered data structure.

class Tiers
{
    Tier t_[2];
    uint32_t n_;

  public:
    explicit Tiers() {
        n_ = 0;
    }
    explicit Tiers(Tier t) {
        t_[0] = t;
        n_ = 1;
    }
    explicit Tiers(Tier t, Tier u) {
        MOZ_ASSERT(t != u);
        t_[0] = t;
        t_[1] = u;
        n_ = 2;
    }

    Tier* begin() {
        return t_;
    }
    Tier* end() {
        return t_ + n_;
    }
};

// The Val class represents a single WebAssembly value of a given value type,
// mostly for the purpose of numeric literals and initializers. A Val does not
// directly map to a JS value since there is not (currently) a precise
// representation of i64 values. A Val may contain non-canonical NaNs since,
// within WebAssembly, floats are not canonicalized. Canonicalization must
// happen at the JS boundary.

class Val
{
    ValType type_;
    union U {
        uint32_t i32_;
        uint64_t i64_;
        float f32_;
        double f64_;
        I8x16 i8x16_;
        I16x8 i16x8_;
        I32x4 i32x4_;
        F32x4 f32x4_;
        U() {}
    } u;

  public:
    Val() = default;

    explicit Val(uint32_t i32) : type_(ValType::I32) { u.i32_ = i32; }
    explicit Val(uint64_t i64) : type_(ValType::I64) { u.i64_ = i64; }

    explicit Val(float f32) : type_(ValType::F32) { u.f32_ = f32; }
    explicit Val(double f64) : type_(ValType::F64) { u.f64_ = f64; }

    explicit Val(const I8x16& i8x16, ValType type = ValType::I8x16) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I8x16 || type_ == ValType::B8x16);
        memcpy(u.i8x16_, i8x16, sizeof(u.i8x16_));
    }
    explicit Val(const I16x8& i16x8, ValType type = ValType::I16x8) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I16x8 || type_ == ValType::B16x8);
        memcpy(u.i16x8_, i16x8, sizeof(u.i16x8_));
    }
    explicit Val(const I32x4& i32x4, ValType type = ValType::I32x4) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I32x4 || type_ == ValType::B32x4);
        memcpy(u.i32x4_, i32x4, sizeof(u.i32x4_));
    }
    explicit Val(const F32x4& f32x4) : type_(ValType::F32x4) {
        memcpy(u.f32x4_, f32x4, sizeof(u.f32x4_));
    }

    ValType type() const { return type_; }
    bool isSimd() const { return IsSimdType(type()); }

    uint32_t i32() const { MOZ_ASSERT(type_ == ValType::I32); return u.i32_; }
    uint64_t i64() const { MOZ_ASSERT(type_ == ValType::I64); return u.i64_; }
    const float& f32() const { MOZ_ASSERT(type_ == ValType::F32); return u.f32_; }
    const double& f64() const { MOZ_ASSERT(type_ == ValType::F64); return u.f64_; }

    const I8x16& i8x16() const {
        MOZ_ASSERT(type_ == ValType::I8x16 || type_ == ValType::B8x16);
        return u.i8x16_;
    }
    const I16x8& i16x8() const {
        MOZ_ASSERT(type_ == ValType::I16x8 || type_ == ValType::B16x8);
        return u.i16x8_;
    }
    const I32x4& i32x4() const {
        MOZ_ASSERT(type_ == ValType::I32x4 || type_ == ValType::B32x4);
        return u.i32x4_;
    }
    const F32x4& f32x4() const {
        MOZ_ASSERT(type_ == ValType::F32x4);
        return u.f32x4_;
    }

    void writePayload(uint8_t* dst) const;
};

typedef Vector<Val, 0, SystemAllocPolicy> ValVector;

// The Sig class represents a WebAssembly function signature which takes a list
// of value types and returns an expression type. The engine uses two in-memory
// representations of the argument Vector's memory (when elements do not fit
// inline): normal malloc allocation (via SystemAllocPolicy) and allocation in
// a LifoAlloc (via LifoAllocPolicy). The former Sig objects can have any
// lifetime since they own the memory. The latter Sig objects must not outlive
// the associated LifoAlloc mark/release interval (which is currently the
// duration of module validation+compilation). Thus, long-lived objects like
// WasmModule must use malloced allocation.

class Sig
{
    ValTypeVector args_;
    ExprType ret_;

  public:
    Sig() : args_(), ret_(ExprType::Void) {}
    Sig(ValTypeVector&& args, ExprType ret) : args_(Move(args)), ret_(ret) {}

    MOZ_MUST_USE bool clone(const Sig& rhs) {
        ret_ = rhs.ret_;
        MOZ_ASSERT(args_.empty());
        return args_.appendAll(rhs.args_);
    }

    ValType arg(unsigned i) const { return args_[i]; }
    const ValTypeVector& args() const { return args_; }
    const ExprType& ret() const { return ret_; }

    HashNumber hash() const {
        return AddContainerToHash(args_, HashNumber(ret_));
    }
    bool operator==(const Sig& rhs) const {
        return ret() == rhs.ret() && EqualContainers(args(), rhs.args());
    }
    bool operator!=(const Sig& rhs) const {
        return !(*this == rhs);
    }

    bool hasI64ArgOrRet() const {
        if (ret() == ExprType::I64)
            return true;
        for (ValType a : args()) {
            if (a == ValType::I64)
                return true;
        }
        return false;
    }

    WASM_DECLARE_SERIALIZABLE(Sig)
};

struct SigHashPolicy
{
    typedef const Sig& Lookup;
    static HashNumber hash(Lookup sig) { return sig.hash(); }
    static bool match(const Sig* lhs, Lookup rhs) { return *lhs == rhs; }
};

// An InitExpr describes a deferred initializer expression, used to initialize
// a global or a table element offset. Such expressions are created during
// decoding and actually executed on module instantiation.

class InitExpr
{
  public:
    enum class Kind {
        Constant,
        GetGlobal
    };

  private:
    Kind kind_;
    union U {
        Val val_;
        struct {
            uint32_t index_;
            ValType type_;
        } global;
        U() {}
    } u;

  public:
    InitExpr() = default;

    explicit InitExpr(Val val) : kind_(Kind::Constant) {
        u.val_ = val;
    }

    explicit InitExpr(uint32_t globalIndex, ValType type) : kind_(Kind::GetGlobal) {
        u.global.index_ = globalIndex;
        u.global.type_ = type;
    }

    Kind kind() const { return kind_; }

    bool isVal() const { return kind() == Kind::Constant; }
    Val val() const { MOZ_ASSERT(isVal()); return u.val_; }

    uint32_t globalIndex() const { MOZ_ASSERT(kind() == Kind::GetGlobal); return u.global.index_; }

    ValType type() const {
        switch (kind()) {
          case Kind::Constant: return u.val_.type();
          case Kind::GetGlobal: return u.global.type_;
        }
        MOZ_CRASH("unexpected initExpr type");
    }
};

// CacheableChars is used to cacheably store UniqueChars.

struct CacheableChars : UniqueChars
{
    CacheableChars() = default;
    explicit CacheableChars(char* ptr) : UniqueChars(ptr) {}
    MOZ_IMPLICIT CacheableChars(UniqueChars&& rhs) : UniqueChars(Move(rhs)) {}
    WASM_DECLARE_SERIALIZABLE(CacheableChars)
};

typedef Vector<CacheableChars, 0, SystemAllocPolicy> CacheableCharsVector;

// Import describes a single wasm import. An ImportVector describes all
// of a single module's imports.
//
// ImportVector is built incrementally by ModuleGenerator and then stored
// immutably by Module.

struct Import
{
    CacheableChars module;
    CacheableChars field;
    DefinitionKind kind;

    Import() = default;
    Import(UniqueChars&& module, UniqueChars&& field, DefinitionKind kind)
      : module(Move(module)), field(Move(field)), kind(kind)
    {}

    WASM_DECLARE_SERIALIZABLE(Import)
};

typedef Vector<Import, 0, SystemAllocPolicy> ImportVector;

// Export describes the export of a definition in a Module to a field in the
// export object. For functions, Export stores an index into the
// FuncExportVector in Metadata. For memory and table exports, there is
// at most one (default) memory/table so no index is needed. Note: a single
// definition can be exported by multiple Exports in the ExportVector.
//
// ExportVector is built incrementally by ModuleGenerator and then stored
// immutably by Module.

class Export
{
    CacheableChars fieldName_;
    struct CacheablePod {
        DefinitionKind kind_;
        uint32_t index_;
    } pod;

  public:
    Export() = default;
    explicit Export(UniqueChars fieldName, uint32_t index, DefinitionKind kind);
    explicit Export(UniqueChars fieldName, DefinitionKind kind);

    const char* fieldName() const { return fieldName_.get(); }

    DefinitionKind kind() const { return pod.kind_; }
    uint32_t funcIndex() const;
    uint32_t globalIndex() const;

    WASM_DECLARE_SERIALIZABLE(Export)
};

typedef Vector<Export, 0, SystemAllocPolicy> ExportVector;

// A GlobalDesc describes a single global variable. Currently, asm.js and wasm
// exposes mutable and immutable private globals, but can't import nor export
// mutable globals.

enum class GlobalKind
{
    Import,
    Constant,
    Variable
};

class GlobalDesc
{
    union V {
        struct {
            union U {
                InitExpr initial_;
                struct {
                    ValType type_;
                    uint32_t index_;
                } import;
                U() {}
            } val;
            unsigned offset_;
            bool isMutable_;
        } var;
        Val cst_;
        V() {}
    } u;
    GlobalKind kind_;

  public:
    GlobalDesc() = default;

    explicit GlobalDesc(InitExpr initial, bool isMutable)
      : kind_((isMutable || !initial.isVal()) ? GlobalKind::Variable : GlobalKind::Constant)
    {
        if (isVariable()) {
            u.var.val.initial_ = initial;
            u.var.isMutable_ = isMutable;
            u.var.offset_ = UINT32_MAX;
        } else {
            u.cst_ = initial.val();
        }
    }

    explicit GlobalDesc(ValType type, bool isMutable, uint32_t importIndex)
      : kind_(GlobalKind::Import)
    {
        u.var.val.import.type_ = type;
        u.var.val.import.index_ = importIndex;
        u.var.isMutable_ = isMutable;
        u.var.offset_ = UINT32_MAX;
    }

    void setOffset(unsigned offset) {
        MOZ_ASSERT(!isConstant());
        MOZ_ASSERT(u.var.offset_ == UINT32_MAX);
        u.var.offset_ = offset;
    }
    unsigned offset() const {
        MOZ_ASSERT(!isConstant());
        MOZ_ASSERT(u.var.offset_ != UINT32_MAX);
        return u.var.offset_;
    }

    GlobalKind kind() const { return kind_; }
    bool isVariable() const { return kind_ == GlobalKind::Variable; }
    bool isConstant() const { return kind_ == GlobalKind::Constant; }
    bool isImport() const { return kind_ == GlobalKind::Import; }

    bool isMutable() const { return !isConstant() && u.var.isMutable_; }
    Val constantValue() const { MOZ_ASSERT(isConstant()); return u.cst_; }
    const InitExpr& initExpr() const { MOZ_ASSERT(isVariable()); return u.var.val.initial_; }
    uint32_t importIndex() const { MOZ_ASSERT(isImport()); return u.var.val.import.index_; }

    ValType type() const {
        switch (kind_) {
          case GlobalKind::Import:   return u.var.val.import.type_;
          case GlobalKind::Variable: return u.var.val.initial_.type();
          case GlobalKind::Constant: return u.cst_.type();
        }
        MOZ_CRASH("unexpected global kind");
    }
};

typedef Vector<GlobalDesc, 0, SystemAllocPolicy> GlobalDescVector;

// ElemSegment represents an element segment in the module where each element
// describes both its function index and its code range.
//
// The codeRangeIndices are laid out in a nondeterminstic order as a result of
// parallel compilation.

struct ElemSegment
{
    uint32_t tableIndex;
    InitExpr offset;
    Uint32Vector elemFuncIndices;
    Uint32Vector elemCodeRangeIndices1_;
    mutable Uint32Vector elemCodeRangeIndices2_;

    ElemSegment() = default;
    ElemSegment(uint32_t tableIndex, InitExpr offset, Uint32Vector&& elemFuncIndices)
      : tableIndex(tableIndex), offset(offset), elemFuncIndices(Move(elemFuncIndices))
    {}

    Uint32Vector& elemCodeRangeIndices(Tier t) {
        switch (t) {
          case Tier::Baseline:
            return elemCodeRangeIndices1_;
          case Tier::Ion:
            return elemCodeRangeIndices2_;
          default:
            MOZ_CRASH("No such tier");
        }
    }

    const Uint32Vector& elemCodeRangeIndices(Tier t) const {
        switch (t) {
          case Tier::Baseline:
            return elemCodeRangeIndices1_;
          case Tier::Ion:
            return elemCodeRangeIndices2_;
          default:
            MOZ_CRASH("No such tier");
        }
    }

    void setTier2(Uint32Vector&& elemCodeRangeIndices) const {
        MOZ_ASSERT(elemCodeRangeIndices2_.length() == 0);
        elemCodeRangeIndices2_ = Move(elemCodeRangeIndices);
    }

    WASM_DECLARE_SERIALIZABLE(ElemSegment)
};

// The ElemSegmentVector is laid out in a deterministic order.

typedef Vector<ElemSegment, 0, SystemAllocPolicy> ElemSegmentVector;

// DataSegment describes the offset of a data segment in the bytecode that is
// to be copied at a given offset into linear memory upon instantiation.

struct DataSegment
{
    InitExpr offset;
    uint32_t bytecodeOffset;
    uint32_t length;
};

typedef Vector<DataSegment, 0, SystemAllocPolicy> DataSegmentVector;

// SigIdDesc describes a signature id that can be used by call_indirect and
// table-entry prologues to structurally compare whether the caller and callee's
// signatures *structurally* match. To handle the general case, a Sig is
// allocated and stored in a process-wide hash table, so that pointer equality
// implies structural equality. As an optimization for the 99% case where the
// Sig has a small number of parameters, the Sig is bit-packed into a uint32
// immediate value so that integer equality implies structural equality. Both
// cases can be handled with a single comparison by always setting the LSB for
// the immediates (the LSB is necessarily 0 for allocated Sig pointers due to
// alignment).

class SigIdDesc
{
  public:
    enum class Kind { None, Immediate, Global };
    static const uintptr_t ImmediateBit = 0x1;

  private:
    Kind kind_;
    size_t bits_;

    SigIdDesc(Kind kind, size_t bits) : kind_(kind), bits_(bits) {}

  public:
    Kind kind() const { return kind_; }
    static bool isGlobal(const Sig& sig);

    SigIdDesc() : kind_(Kind::None), bits_(0) {}
    static SigIdDesc global(const Sig& sig, uint32_t globalDataOffset);
    static SigIdDesc immediate(const Sig& sig);

    bool isGlobal() const { return kind_ == Kind::Global; }

    size_t immediate() const { MOZ_ASSERT(kind_ == Kind::Immediate); return bits_; }
    uint32_t globalDataOffset() const { MOZ_ASSERT(kind_ == Kind::Global); return bits_; }
};

// SigWithId pairs a Sig with SigIdDesc, describing either how to compile code
// that compares this signature's id or, at instantiation what signature ids to
// allocate in the global hash and where to put them.

struct SigWithId : Sig
{
    SigIdDesc id;

    SigWithId() = default;
    explicit SigWithId(Sig&& sig) : Sig(Move(sig)), id() {}
    SigWithId(Sig&& sig, SigIdDesc id) : Sig(Move(sig)), id(id) {}
    void operator=(Sig&& rhs) { Sig::operator=(Move(rhs)); }

    WASM_DECLARE_SERIALIZABLE(SigWithId)
};

typedef Vector<SigWithId, 0, SystemAllocPolicy> SigWithIdVector;
typedef Vector<const SigWithId*, 0, SystemAllocPolicy> SigWithIdPtrVector;

// A wasm::Trap represents a wasm-defined trap that can occur during execution
// which triggers a WebAssembly.RuntimeError. Generated code may jump to a Trap
// symbolically, passing the bytecode offset to report as the trap offset. The
// generated jump will be bound to a tiny stub which fills the offset and
// then jumps to a per-Trap shared stub at the end of the module.

enum class Trap
{
    // The Unreachable opcode has been executed.
    Unreachable,
    // An integer arithmetic operation led to an overflow.
    IntegerOverflow,
    // Trying to coerce NaN to an integer.
    InvalidConversionToInteger,
    // Integer division by zero.
    IntegerDivideByZero,
    // Out of bounds on wasm memory accesses and asm.js SIMD/atomic accesses.
    OutOfBounds,
    // Unaligned on wasm atomic accesses; also used for non-standard ARM
    // unaligned access faults.
    UnalignedAccess,
    // call_indirect to null.
    IndirectCallToNull,
    // call_indirect signature mismatch.
    IndirectCallBadSig,

    // (asm.js only) SIMD float to int conversion failed because the input
    // wasn't in bounds.
    ImpreciseSimdConversion,

    // The internal stack space was exhausted. For compatibility, this throws
    // the same over-recursed error as JS.
    StackOverflow,

    // Signal an error that was reported in C++ code.
    ThrowReported,

    Limit
};

// A wrapper around the bytecode offset of a wasm instruction within a whole
// module, used for trap offsets or call offsets. These offsets should refer to
// the first byte of the instruction that triggered the trap / did the call and
// should ultimately derive from OpIter::bytecodeOffset.

struct BytecodeOffset
{
    static const uint32_t INVALID = -1;
    uint32_t offset;

    BytecodeOffset() : offset(INVALID) {}
    explicit BytecodeOffset(uint32_t offset) : offset(offset) {}

    bool isValid() const { return offset != INVALID; }
};

// A TrapSite (in the TrapSiteVector for a given Trap code) represents a wasm
// instruction at a given bytecode offset that can fault at the given pc offset.
// When such a fault occurs, a signal/exception handler looks up the TrapSite to
// confirm the fault is intended/safe and redirects pc to the trap stub.

struct TrapSite
{
    uint32_t pcOffset;
    BytecodeOffset bytecode;

    TrapSite() : pcOffset(-1), bytecode() {}
    TrapSite(uint32_t pcOffset, BytecodeOffset bytecode) : pcOffset(pcOffset), bytecode(bytecode) {}

    void offsetBy(uint32_t offset) {
        pcOffset += offset;
    }
};

WASM_DECLARE_POD_VECTOR(TrapSite, TrapSiteVector)

struct TrapSiteVectorArray : EnumeratedArray<Trap, Trap::Limit, TrapSiteVector>
{
    bool empty() const;
    void clear();
    void swap(TrapSiteVectorArray& rhs);
    void podResizeToFit();

    WASM_DECLARE_SERIALIZABLE(TrapSiteVectorArray)
};

// The (,Callable,Func)Offsets classes are used to record the offsets of
// different key points in a CodeRange during compilation.

struct Offsets
{
    explicit Offsets(uint32_t begin = 0, uint32_t end = 0)
      : begin(begin), end(end)
    {}

    // These define a [begin, end) contiguous range of instructions compiled
    // into a CodeRange.
    uint32_t begin;
    uint32_t end;
};

struct CallableOffsets : Offsets
{
    MOZ_IMPLICIT CallableOffsets(uint32_t ret = 0)
      : Offsets(), ret(ret)
    {}

    // The offset of the return instruction precedes 'end' by a variable number
    // of instructions due to out-of-line codegen.
    uint32_t ret;
};

struct JitExitOffsets : CallableOffsets
{
    MOZ_IMPLICIT JitExitOffsets()
      : CallableOffsets(), untrustedFPStart(0), untrustedFPEnd(0)
    {}

    // There are a few instructions in the Jit exit where FP may be trash
    // (because it may have been clobbered by the JS Jit), known as the
    // untrusted FP zone.
    uint32_t untrustedFPStart;
    uint32_t untrustedFPEnd;
};

struct FuncOffsets : CallableOffsets
{
    MOZ_IMPLICIT FuncOffsets()
      : CallableOffsets(),
        normalEntry(0),
        tierEntry(0)
    {}

    // Function CodeRanges have a table entry which takes an extra signature
    // argument which is checked against the callee's signature before falling
    // through to the normal prologue. The table entry is thus at the beginning
    // of the CodeRange and the normal entry is at some offset after the table
    // entry.
    uint32_t normalEntry;

    // The tierEntry is the point within a function to which the patching code
    // within a Tier-1 function jumps.  It could be the instruction following
    // the jump in the Tier-1 function, or the point following the standard
    // prologue within a Tier-2 function.
    uint32_t tierEntry;
};

typedef Vector<FuncOffsets, 0, SystemAllocPolicy> FuncOffsetsVector;

// A CodeRange describes a single contiguous range of code within a wasm
// module's code segment. A CodeRange describes what the code does and, for
// function bodies, the name and source coordinates of the function.

class CodeRange
{
  public:
    enum Kind {
        Function,          // function definition
        InterpEntry,       // calls into wasm from C++
        JitEntry,          // calls into wasm from jit code
        ImportInterpExit,  // slow-path calling from wasm into C++ interp
        ImportJitExit,     // fast-path calling from wasm into jit code
        BuiltinThunk,      // fast-path calling from wasm into a C++ native
        TrapExit,          // calls C++ to report and jumps to throw stub
        OldTrapExit,       // calls C++ to report and jumps to throw stub
        DebugTrap,         // calls C++ to handle debug event
        FarJumpIsland,     // inserted to connect otherwise out-of-range insns
        OutOfBoundsExit,   // stub jumped to by non-standard asm.js SIMD/Atomics
        UnalignedExit,     // stub jumped to by wasm Atomics and non-standard
                           // ARM unaligned trap
        Interrupt,         // stub executes asynchronously to interrupt wasm
        Throw              // special stack-unwinding stub jumped to by other stubs
    };

  private:
    // All fields are treated as cacheable POD:
    uint32_t begin_;
    uint32_t ret_;
    uint32_t end_;
    union {
        struct {
            uint32_t funcIndex_;
            union {
                struct {
                    uint32_t lineOrBytecode_;
                    uint8_t beginToNormalEntry_;
                    uint8_t beginToTierEntry_;
                } func;
                struct {
                    uint16_t beginToUntrustedFPStart_;
                    uint16_t beginToUntrustedFPEnd_;
                } jitExit;
            };
        };
        Trap trap_;
    } u;
    Kind kind_ : 8;

  public:
    CodeRange() = default;
    CodeRange(Kind kind, Offsets offsets);
    CodeRange(Kind kind, uint32_t funcIndex, Offsets offsets);
    CodeRange(Kind kind, CallableOffsets offsets);
    CodeRange(Kind kind, uint32_t funcIndex, CallableOffsets);
    CodeRange(uint32_t funcIndex, JitExitOffsets offsets);
    CodeRange(Trap trap, CallableOffsets offsets);
    CodeRange(uint32_t funcIndex, uint32_t lineOrBytecode, FuncOffsets offsets);

    void offsetBy(uint32_t offset) {
        begin_ += offset;
        end_ += offset;
        if (hasReturn())
            ret_ += offset;
    }

    // All CodeRanges have a begin and end.

    uint32_t begin() const {
        return begin_;
    }
    uint32_t end() const {
        return end_;
    }

    // Other fields are only available for certain CodeRange::Kinds.

    Kind kind() const {
        return kind_;
    }

    bool isFunction() const {
        return kind() == Function;
    }
    bool isImportExit() const {
        return kind() == ImportJitExit || kind() == ImportInterpExit || kind() == BuiltinThunk;
    }
    bool isImportInterpExit() const {
        return kind() == ImportInterpExit;
    }
    bool isImportJitExit() const {
        return kind() == ImportJitExit;
    }
    bool isTrapExit() const {
        return kind() == OldTrapExit || kind() == TrapExit;
    }
    bool isDebugTrap() const {
        return kind() == DebugTrap;
    }
    bool isThunk() const {
        return kind() == FarJumpIsland;
    }

    // Function, import exits and trap exits have standard callable prologues
    // and epilogues. Asynchronous frame iteration needs to know the offset of
    // the return instruction to calculate the frame pointer.

    bool hasReturn() const {
        return isFunction() || isImportExit() || kind() == OldTrapExit || isDebugTrap();
    }
    uint32_t ret() const {
        MOZ_ASSERT(hasReturn());
        return ret_;
    }

    // Functions, export stubs and import stubs all have an associated function
    // index.

    bool isJitEntry() const {
        return kind() == JitEntry;
    }
    bool isInterpEntry() const {
        return kind() == InterpEntry;
    }
    bool isEntry() const {
        return isInterpEntry() || isJitEntry();
    }
    bool hasFuncIndex() const {
        return isFunction() || isImportExit() || isEntry();
    }
    uint32_t funcIndex() const {
        MOZ_ASSERT(hasFuncIndex());
        return u.funcIndex_;
    }

    // TrapExit CodeRanges have a Trap field.

    Trap trap() const {
        MOZ_ASSERT(isTrapExit());
        return u.trap_;
    }

    // Function CodeRanges have two entry points: one for normal calls (with a
    // known signature) and one for table calls (which involves dynamic
    // signature checking).

    uint32_t funcTableEntry() const {
        MOZ_ASSERT(isFunction());
        return begin_;
    }
    uint32_t funcNormalEntry() const {
        MOZ_ASSERT(isFunction());
        return begin_ + u.func.beginToNormalEntry_;
    }
    uint32_t funcTierEntry() const {
        MOZ_ASSERT(isFunction());
        return begin_ + u.func.beginToTierEntry_;
    }
    uint32_t funcLineOrBytecode() const {
        MOZ_ASSERT(isFunction());
        return u.func.lineOrBytecode_;
    }

    // ImportJitExit have a particular range where the value of FP can't be
    // trusted for profiling and thus must be ignored.

    uint32_t jitExitUntrustedFPStart() const {
        MOZ_ASSERT(isImportJitExit());
        return begin_ + u.jitExit.beginToUntrustedFPStart_;
    }
    uint32_t jitExitUntrustedFPEnd() const {
        MOZ_ASSERT(isImportJitExit());
        return begin_ + u.jitExit.beginToUntrustedFPEnd_;
    }

    // A sorted array of CodeRanges can be looked up via BinarySearch and
    // OffsetInCode.

    struct OffsetInCode {
        size_t offset;
        explicit OffsetInCode(size_t offset) : offset(offset) {}
        bool operator==(const CodeRange& rhs) const {
            return offset >= rhs.begin() && offset < rhs.end();
        }
        bool operator<(const CodeRange& rhs) const {
            return offset < rhs.begin();
        }
    };
};

WASM_DECLARE_POD_VECTOR(CodeRange, CodeRangeVector)

extern const CodeRange*
LookupInSorted(const CodeRangeVector& codeRanges, CodeRange::OffsetInCode target);

// While the frame-pointer chain allows the stack to be unwound without
// metadata, Error.stack still needs to know the line/column of every call in
// the chain. A CallSiteDesc describes a single callsite to which CallSite adds
// the metadata necessary to walk up to the next frame. Lastly CallSiteAndTarget
// adds the function index of the callee.

class CallSiteDesc
{
    uint32_t lineOrBytecode_ : 29;
    uint32_t kind_ : 3;
  public:
    enum Kind {
        Func,       // pc-relative call to a specific function
        Dynamic,    // dynamic callee called via register
        Symbolic,   // call to a single symbolic callee
        OldTrapExit,// call to a trap exit (being removed)
        EnterFrame, // call to a enter frame handler
        LeaveFrame, // call to a leave frame handler
        Breakpoint  // call to instruction breakpoint
    };
    CallSiteDesc() {}
    explicit CallSiteDesc(Kind kind)
      : lineOrBytecode_(0), kind_(kind)
    {
        MOZ_ASSERT(kind == Kind(kind_));
    }
    CallSiteDesc(uint32_t lineOrBytecode, Kind kind)
      : lineOrBytecode_(lineOrBytecode), kind_(kind)
    {
        MOZ_ASSERT(kind == Kind(kind_));
        MOZ_ASSERT(lineOrBytecode == lineOrBytecode_);
    }
    uint32_t lineOrBytecode() const { return lineOrBytecode_; }
    Kind kind() const { return Kind(kind_); }
};

class CallSite : public CallSiteDesc
{
    uint32_t returnAddressOffset_;

  public:
    CallSite() {}

    CallSite(CallSiteDesc desc, uint32_t returnAddressOffset)
      : CallSiteDesc(desc),
        returnAddressOffset_(returnAddressOffset)
    { }

    void offsetBy(int32_t delta) { returnAddressOffset_ += delta; }
    uint32_t returnAddressOffset() const { return returnAddressOffset_; }
};

WASM_DECLARE_POD_VECTOR(CallSite, CallSiteVector)

// A CallSiteTarget describes the callee of a CallSite, either a function or a
// trap exit. Although checked in debug builds, a CallSiteTarget doesn't
// officially know whether it targets a function or trap, relying on the Kind of
// the CallSite to discriminate.

class CallSiteTarget
{
    uint32_t packed_;
#ifdef DEBUG
    enum Kind { None, FuncIndex, TrapExit } kind_;
#endif

  public:
    explicit CallSiteTarget()
      : packed_(UINT32_MAX)
#ifdef DEBUG
      , kind_(None)
#endif
    {}

    explicit CallSiteTarget(uint32_t funcIndex)
      : packed_(funcIndex)
#ifdef DEBUG
      , kind_(FuncIndex)
#endif
    {}

    explicit CallSiteTarget(Trap trap)
      : packed_(uint32_t(trap))
#ifdef DEBUG
      , kind_(TrapExit)
#endif
    {}

    uint32_t funcIndex() const {
        MOZ_ASSERT(kind_ == FuncIndex);
        return packed_;
    }

    Trap trap() const {
        MOZ_ASSERT(kind_ == TrapExit);
        MOZ_ASSERT(packed_ < uint32_t(Trap::Limit));
        return Trap(packed_);
    }
};

typedef Vector<CallSiteTarget, 0, SystemAllocPolicy> CallSiteTargetVector;

// A wasm::SymbolicAddress represents a pointer to a well-known function that is
// embedded in wasm code. Since wasm code is serialized and later deserialized
// into a different address space, symbolic addresses must be used for *all*
// pointers into the address space. The MacroAssembler records a list of all
// SymbolicAddresses and the offsets of their use in the code for later patching
// during static linking.

enum class SymbolicAddress
{
    ToInt32,
#if defined(JS_CODEGEN_ARM)
    aeabi_idivmod,
    aeabi_uidivmod,
#endif
    ModD,
    SinD,
    CosD,
    TanD,
    ASinD,
    ACosD,
    ATanD,
    CeilD,
    CeilF,
    FloorD,
    FloorF,
    TruncD,
    TruncF,
    NearbyIntD,
    NearbyIntF,
    ExpD,
    LogD,
    PowD,
    ATan2D,
    HandleExecutionInterrupt,
    HandleDebugTrap,
    HandleThrow,
    ReportTrap,
    OldReportTrap,
    ReportOutOfBounds,
    ReportUnalignedAccess,
    ReportInt64JSCall,
    CallImport_Void,
    CallImport_I32,
    CallImport_I64,
    CallImport_F64,
    CoerceInPlace_ToInt32,
    CoerceInPlace_ToNumber,
    CoerceInPlace_JitEntry,
    DivI64,
    UDivI64,
    ModI64,
    UModI64,
    TruncateDoubleToInt64,
    TruncateDoubleToUint64,
    SaturatingTruncateDoubleToInt64,
    SaturatingTruncateDoubleToUint64,
    Uint64ToFloat32,
    Uint64ToDouble,
    Int64ToFloat32,
    Int64ToDouble,
    GrowMemory,
    CurrentMemory,
    WaitI32,
    WaitI64,
    Wake,
#if defined(JS_CODEGEN_MIPS32)
    js_jit_gAtomic64Lock,
#endif
    Limit
};

bool
IsRoundingFunction(SymbolicAddress callee, jit::RoundingMode* mode);

// Assumptions captures ambient state that must be the same when compiling and
// deserializing a module for the compiled code to be valid. If it's not, then
// the module must be recompiled from scratch.

struct Assumptions
{
    uint32_t              cpuId;
    JS::BuildIdCharVector buildId;

    explicit Assumptions(JS::BuildIdCharVector&& buildId);

    // If Assumptions is constructed without arguments, initBuildIdFromContext()
    // must be called to complete initialization.
    Assumptions();
    bool initBuildIdFromContext(JSContext* cx);

    bool clone(const Assumptions& other);

    bool operator==(const Assumptions& rhs) const;
    bool operator!=(const Assumptions& rhs) const { return !(*this == rhs); }

    size_t serializedSize() const;
    uint8_t* serialize(uint8_t* cursor) const;
    const uint8_t* deserialize(const uint8_t* cursor, size_t remain);
    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

// A Module can either be asm.js or wasm.

enum ModuleKind
{
    Wasm,
    AsmJS
};

enum class Shareable
{
    False,
    True
};

// Represents the resizable limits of memories and tables.

struct Limits
{
    uint32_t initial;
    Maybe<uint32_t> maximum;

    // `shared` is Shareable::False for tables but may be Shareable::True for
    // memories.
    Shareable shared;

    Limits() = default;
    explicit Limits(uint32_t initial, const Maybe<uint32_t>& maximum = Nothing(),
                    Shareable shared = Shareable::False)
      : initial(initial), maximum(maximum), shared(shared)
    {}
};

// TableDesc describes a table as well as the offset of the table's base pointer
// in global memory. Currently, wasm only has "any function" and asm.js only
// "typed function".

enum class TableKind
{
    AnyFunction,
    TypedFunction
};

struct TableDesc
{
    TableKind kind;
    bool external;
    uint32_t globalDataOffset;
    Limits limits;

    TableDesc() = default;
    TableDesc(TableKind kind, const Limits& limits)
     : kind(kind),
       external(false),
       globalDataOffset(UINT32_MAX),
       limits(limits)
    {}
};

typedef Vector<TableDesc, 0, SystemAllocPolicy> TableDescVector;

// TLS data for a single module instance.
//
// Every WebAssembly function expects to be passed a hidden TLS pointer argument
// in WasmTlsReg. The TLS pointer argument points to a TlsData struct.
// Compiled functions expect that the TLS pointer does not change for the
// lifetime of the thread.
//
// There is a TlsData per module instance per thread, so inter-module calls need
// to pass the TLS pointer appropriate for the callee module.
//
// After the TlsData struct follows the module's declared TLS variables.

struct TlsData
{
    // Pointer to the base of the default memory (or null if there is none).
    uint8_t* memoryBase;

#ifndef WASM_HUGE_MEMORY
    // Bounds check limit of memory, in bytes (or zero if there is no memory).
    uint32_t boundsCheckLimit;
#endif

    // Pointer to the Instance that contains this TLS data.
    Instance* instance;

    // The containing JSContext.
    JSContext* cx;

    // The native stack limit which is checked by prologues. Shortcut for
    // cx->stackLimitForJitCode(JS::StackForUntrustedScript).
    uintptr_t stackLimit;

    // Pointer that should be freed (due to padding before the TlsData).
    void* allocatedBase;

    // When compiling with tiering, the jumpTable has one entry for each
    // baseline-compiled function.
    void** jumpTable;

    // The globalArea must be the last field.  Globals for the module start here
    // and are inline in this structure.  16-byte alignment is required for SIMD
    // data.
    MOZ_ALIGNED_DECL(char globalArea, 16);
};

static const size_t TlsDataAlign = 16;  // = Simd128DataSize
static_assert(offsetof(TlsData, globalArea) % TlsDataAlign == 0, "aligned");

struct TlsDataDeleter
{
    void operator()(TlsData* tlsData) { js_free(tlsData->allocatedBase); }
};

typedef UniquePtr<TlsData, TlsDataDeleter> UniqueTlsData;

extern UniqueTlsData
CreateTlsData(uint32_t globalDataLength);

// ExportArg holds the unboxed operands to the wasm entry trampoline which can
// be called through an ExportFuncPtr.

struct ExportArg
{
    uint64_t lo;
    uint64_t hi;
};

typedef int32_t (*ExportFuncPtr)(ExportArg* args, TlsData* tls);

// FuncImportTls describes the region of wasm global memory allocated in the
// instance's thread-local storage for a function import. This is accessed
// directly from JIT code and mutated by Instance as exits become optimized and
// deoptimized.

struct FuncImportTls
{
    // The code to call at an import site: a wasm callee, a thunk into C++, or a
    // thunk into JIT code.
    void* code;

    // The callee's TlsData pointer, which must be loaded to WasmTlsReg (along
    // with any pinned registers) before calling 'code'.
    TlsData* tls;

    // If 'code' points into a JIT code thunk, the BaselineScript of the callee,
    // for bidirectional registration purposes.
    jit::BaselineScript* baselineScript;

    // A GC pointer which keeps the callee alive. For imported wasm functions,
    // this points to the wasm function's WasmInstanceObject. For all other
    // imported functions, 'obj' points to the JSFunction.
    GCPtrObject obj;
    static_assert(sizeof(GCPtrObject) == sizeof(void*), "for JIT access");
};

// TableTls describes the region of wasm global memory allocated in the
// instance's thread-local storage which is accessed directly from JIT code
// to bounds-check and index the table.

struct TableTls
{
    // Length of the table in number of elements (not bytes).
    uint32_t length;

    // Pointer to the array of elements (of type either ExternalTableElem or
    // void*).
    void* base;
};

// When a table can contain functions from other instances (it is "external"),
// the internal representation is an array of ExternalTableElem instead of just
// an array of code pointers.

struct ExternalTableElem
{
    // The code to call when calling this element. The table ABI is the system
    // ABI with the additional ABI requirements that:
    //  - WasmTlsReg and any pinned registers have been loaded appropriately
    //  - if this is a heterogeneous table that requires a signature check,
    //    WasmTableCallSigReg holds the signature id.
    void* code;

    // The pointer to the callee's instance's TlsData. This must be loaded into
    // WasmTlsReg before calling 'code'.
    TlsData* tls;
};

// CalleeDesc describes how to compile one of the variety of asm.js/wasm calls.
// This is hoisted into WasmTypes.h for sharing between Ion and Baseline.

class CalleeDesc
{
  public:
    enum Which {
        // Calls a function defined in the same module by its index.
        Func,

        // Calls the import identified by the offset of its FuncImportTls in
        // thread-local data.
        Import,

        // Calls a WebAssembly table (heterogeneous, index must be bounds
        // checked, callee instance depends on TableDesc).
        WasmTable,

        // Calls an asm.js table (homogeneous, masked index, same-instance).
        AsmJSTable,

        // Call a C++ function identified by SymbolicAddress.
        Builtin,

        // Like Builtin, but automatically passes Instance* as first argument.
        BuiltinInstanceMethod
    };

  private:
    // which_ shall be initialized in the static constructors
    MOZ_INIT_OUTSIDE_CTOR Which which_;
    union U {
        U() {}
        uint32_t funcIndex_;
        struct {
            uint32_t globalDataOffset_;
        } import;
        struct {
            uint32_t globalDataOffset_;
            uint32_t minLength_;
            bool external_;
            SigIdDesc sigId_;
        } table;
        SymbolicAddress builtin_;
    } u;

  public:
    CalleeDesc() {}
    static CalleeDesc function(uint32_t funcIndex) {
        CalleeDesc c;
        c.which_ = Func;
        c.u.funcIndex_ = funcIndex;
        return c;
    }
    static CalleeDesc import(uint32_t globalDataOffset) {
        CalleeDesc c;
        c.which_ = Import;
        c.u.import.globalDataOffset_ = globalDataOffset;
        return c;
    }
    static CalleeDesc wasmTable(const TableDesc& desc, SigIdDesc sigId) {
        CalleeDesc c;
        c.which_ = WasmTable;
        c.u.table.globalDataOffset_ = desc.globalDataOffset;
        c.u.table.minLength_ = desc.limits.initial;
        c.u.table.external_ = desc.external;
        c.u.table.sigId_ = sigId;
        return c;
    }
    static CalleeDesc asmJSTable(const TableDesc& desc) {
        CalleeDesc c;
        c.which_ = AsmJSTable;
        c.u.table.globalDataOffset_ = desc.globalDataOffset;
        return c;
    }
    static CalleeDesc builtin(SymbolicAddress callee) {
        CalleeDesc c;
        c.which_ = Builtin;
        c.u.builtin_ = callee;
        return c;
    }
    static CalleeDesc builtinInstanceMethod(SymbolicAddress callee) {
        CalleeDesc c;
        c.which_ = BuiltinInstanceMethod;
        c.u.builtin_ = callee;
        return c;
    }
    Which which() const {
        return which_;
    }
    uint32_t funcIndex() const {
        MOZ_ASSERT(which_ == Func);
        return u.funcIndex_;
    }
    uint32_t importGlobalDataOffset() const {
        MOZ_ASSERT(which_ == Import);
        return u.import.globalDataOffset_;
    }
    bool isTable() const {
        return which_ == WasmTable || which_ == AsmJSTable;
    }
    uint32_t tableLengthGlobalDataOffset() const {
        MOZ_ASSERT(isTable());
        return u.table.globalDataOffset_ + offsetof(TableTls, length);
    }
    uint32_t tableBaseGlobalDataOffset() const {
        MOZ_ASSERT(isTable());
        return u.table.globalDataOffset_ + offsetof(TableTls, base);
    }
    bool wasmTableIsExternal() const {
        MOZ_ASSERT(which_ == WasmTable);
        return u.table.external_;
    }
    SigIdDesc wasmTableSigId() const {
        MOZ_ASSERT(which_ == WasmTable);
        return u.table.sigId_;
    }
    uint32_t wasmTableMinLength() const {
        MOZ_ASSERT(which_ == WasmTable);
        return u.table.minLength_;
    }
    SymbolicAddress builtin() const {
        MOZ_ASSERT(which_ == Builtin || which_ == BuiltinInstanceMethod);
        return u.builtin_;
    }
};

// Because ARM has a fixed-width instruction encoding, ARM can only express a
// limited subset of immediates (in a single instruction).

extern bool
IsValidARMImmediate(uint32_t i);

extern uint32_t
RoundUpToNextValidARMImmediate(uint32_t i);

// The WebAssembly spec hard-codes the virtual page size to be 64KiB and
// requires the size of linear memory to always be a multiple of 64KiB.

static const unsigned PageSize = 64 * 1024;

// Bounds checks always compare the base of the memory access with the bounds
// check limit. If the memory access is unaligned, this means that, even if the
// bounds check succeeds, a few bytes of the access can extend past the end of
// memory. To guard against this, extra space is included in the guard region to
// catch the overflow. MaxMemoryAccessSize is a conservative approximation of
// the maximum guard space needed to catch all unaligned overflows.

static const unsigned MaxMemoryAccessSize = sizeof(Val);

#ifdef WASM_HUGE_MEMORY

// On WASM_HUGE_MEMORY platforms, every asm.js or WebAssembly memory
// unconditionally allocates a huge region of virtual memory of size
// wasm::HugeMappedSize. This allows all memory resizing to work without
// reallocation and provides enough guard space for all offsets to be folded
// into memory accesses.

static const uint64_t IndexRange = uint64_t(UINT32_MAX) + 1;
static const uint64_t OffsetGuardLimit = uint64_t(INT32_MAX) + 1;
static const uint64_t UnalignedGuardPage = PageSize;
static const uint64_t HugeMappedSize = IndexRange + OffsetGuardLimit + UnalignedGuardPage;

static_assert(MaxMemoryAccessSize <= UnalignedGuardPage, "rounded up to static page size");

#else // !WASM_HUGE_MEMORY

// On !WASM_HUGE_MEMORY platforms:
//  - To avoid OOM in ArrayBuffer::prepareForAsmJS, asm.js continues to use the
//    original ArrayBuffer allocation which has no guard region at all.
//  - For WebAssembly memories, an additional GuardSize is mapped after the
//    accessible region of the memory to catch folded (base+offset) accesses
//    where `offset < OffsetGuardLimit` as well as the overflow from unaligned
//    accesses, as described above for MaxMemoryAccessSize.

static const size_t OffsetGuardLimit = PageSize - MaxMemoryAccessSize;
static const size_t GuardSize = PageSize;

// Return whether the given immediate satisfies the constraints of the platform
// (viz. that, on ARM, IsValidARMImmediate).

extern bool
IsValidBoundsCheckImmediate(uint32_t i);

// For a given WebAssembly/asm.js max size, return the number of bytes to
// map which will necessarily be a multiple of the system page size and greater
// than maxSize. For a returned mappedSize:
//   boundsCheckLimit = mappedSize - GuardSize
//   IsValidBoundsCheckImmediate(boundsCheckLimit)

extern size_t
ComputeMappedSize(uint32_t maxSize);

#endif // WASM_HUGE_MEMORY

// Metadata for memory accesses. On WASM_HUGE_MEMORY platforms, only
// (non-SIMD/Atomic) asm.js loads and stores create a MemoryAccess so that the
// signal handler can implement the semantically-correct wraparound logic; the
// rest simply redirect to the out-of-bounds stub in the signal handler. On x86,
// the base address of memory is baked into each memory access instruction so
// the MemoryAccess records the location of each for patching. On all other
// platforms, no MemoryAccess is created.

class MemoryAccess
{
    uint32_t insnOffset_;
    uint32_t trapOutOfLineOffset_;

  public:
    MemoryAccess() = default;
    explicit MemoryAccess(uint32_t insnOffset, uint32_t trapOutOfLineOffset = UINT32_MAX)
      : insnOffset_(insnOffset),
        trapOutOfLineOffset_(trapOutOfLineOffset)
    {}

    uint32_t insnOffset() const {
        return insnOffset_;
    }
    bool hasTrapOutOfLineCode() const {
        return trapOutOfLineOffset_ != UINT32_MAX;
    }
    uint8_t* trapOutOfLineCode(uint8_t* code) const {
        MOZ_ASSERT(hasTrapOutOfLineCode());
        return code + trapOutOfLineOffset_;
    }

    void offsetBy(uint32_t delta) {
        insnOffset_ += delta;
        if (hasTrapOutOfLineCode())
            trapOutOfLineOffset_ += delta;
    }
};

WASM_DECLARE_POD_VECTOR(MemoryAccess, MemoryAccessVector)

// wasm::Frame represents the bytes pushed by the call instruction and the fixed
// prologue generated by wasm::GenerateCallablePrologue.
//
// Across all architectures it is assumed that, before the call instruction, the
// stack pointer is WasmStackAlignment-aligned. Thus after the prologue, and
// before the function has made its stack reservation, the stack alignment is
// sizeof(Frame) % WasmStackAlignment.
//
// During MacroAssembler code generation, the bytes pushed after the wasm::Frame
// are counted by masm.framePushed. Thus, the stack alignment at any point in
// time is (sizeof(wasm::Frame) + masm.framePushed) % WasmStackAlignment.

struct Frame
{
    // The caller's Frame*. See GenerateCallableEpilogue for why this must be
    // the first field of wasm::Frame (in a downward-growing stack).
    Frame* callerFP;

    // The saved value of WasmTlsReg on entry to the function. This is
    // effectively the callee's instance.
    TlsData* tls;

#if defined(JS_CODEGEN_MIPS32)
    // Double word aligned frame ensures correct alignment for wasm locals
    // on architectures that require the stack alignment to be more than word size.
    uintptr_t padding_;
#endif
    // The return address pushed by the call (in the case of ARM/MIPS the return
    // address is pushed by the first instruction of the prologue).
    void* returnAddress;

    // Helper functions:

    Instance* instance() const { return tls->instance; }
};

// A DebugFrame is a Frame with additional fields that are added after the
// normal function prologue by the baseline compiler. If a Module is compiled
// with debugging enabled, then all its code creates DebugFrames on the stack
// instead of just Frames. These extra fields are used by the Debugger API.

class DebugFrame
{
    // The results field left uninitialized and only used during the baseline
    // compiler's return sequence to allow the debugger to inspect and modify
    // the return value of a frame being debugged.
    union
    {
        int32_t resultI32_;
        int64_t resultI64_;
        float resultF32_;
        double resultF64_;
    };

    // The returnValue() method returns a HandleValue pointing to this field.
    js::Value cachedReturnJSValue_;

    // The function index of this frame. Technically, this could be derived
    // given a PC into this frame (which could lookup the CodeRange which has
    // the function index), but this isn't always readily available.
    uint32_t funcIndex_;

    // Flags whose meaning are described below.
    union
    {
        struct
        {
            bool observing_ : 1;
            bool isDebuggee_ : 1;
            bool prevUpToDate_ : 1;
            bool hasCachedSavedFrame_ : 1;
            bool hasCachedReturnJSValue_ : 1;
        };
        void* flagsWord_;
    };

    // Avoid -Wunused-private-field warnings.
  protected:
#if JS_BITS_PER_WORD == 32 && !defined(JS_CODEGEN_MIPS32)
    // See alignmentStaticAsserts().
    // For MIPS32 padding is already incorporated in the frame.
    uint32_t padding_;
#endif

  private:
    // The Frame goes at the end since the stack grows down.
    Frame frame_;

  public:
    static DebugFrame* from(Frame* fp);
    Frame& frame() { return frame_; }
    uint32_t funcIndex() const { return funcIndex_; }
    Instance* instance() const { return frame_.instance(); }
    GlobalObject* global() const;
    JSObject* environmentChain() const;
    bool getLocal(uint32_t localIndex, MutableHandleValue vp);

    // The return value must be written from the unboxed representation in the
    // results union into cachedReturnJSValue_ by updateReturnJSValue() before
    // returnValue() can return a Handle to it.

    void updateReturnJSValue();
    HandleValue returnValue() const;
    void clearReturnJSValue();

    // Once the debugger observes a frame, it must be notified via
    // onLeaveFrame() before the frame is popped. Calling observe() ensures the
    // leave frame traps are enabled. Both methods are idempotent so the caller
    // doesn't have to worry about calling them more than once.

    void observe(JSContext* cx);
    void leave(JSContext* cx);

    // The 'isDebugge' bit is initialized to false and set by the WebAssembly
    // runtime right before a frame is exposed to the debugger, as required by
    // the Debugger API. The bit is then used for Debugger-internal purposes
    // afterwards.

    bool isDebuggee() const { return isDebuggee_; }
    void setIsDebuggee() { isDebuggee_ = true; }
    void unsetIsDebuggee() { isDebuggee_ = false; }

    // These are opaque boolean flags used by the debugger to implement
    // AbstractFramePtr. They are initialized to false and not otherwise read or
    // written by wasm code or runtime.

    bool prevUpToDate() const { return prevUpToDate_; }
    void setPrevUpToDate() { prevUpToDate_ = true; }
    void unsetPrevUpToDate() { prevUpToDate_ = false; }

    bool hasCachedSavedFrame() const { return hasCachedSavedFrame_; }
    void setHasCachedSavedFrame() { hasCachedSavedFrame_ = true; }

    // DebugFrame is accessed directly by JIT code.

    static constexpr size_t offsetOfResults() { return offsetof(DebugFrame, resultI32_); }
    static constexpr size_t offsetOfFlagsWord() { return offsetof(DebugFrame, flagsWord_); }
    static constexpr size_t offsetOfFuncIndex() { return offsetof(DebugFrame, funcIndex_); }
    static constexpr size_t offsetOfFrame() { return offsetof(DebugFrame, frame_); }

    // DebugFrames are aligned to 8-byte aligned, allowing them to be placed in
    // an AbstractFramePtr.

    static const unsigned Alignment = 8;
    static void alignmentStaticAsserts();
};

} // namespace wasm
} // namespace js

#endif // wasm_types_h
