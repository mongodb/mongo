/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2014 Mozilla Foundation
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

#include "wasm/AsmJS.h"

#include "mozilla/Attributes.h"
#include "mozilla/Compression.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Unused.h"

#include "jsmath.h"
#include "jsutil.h"

#include "builtin/SIMD.h"
#include "builtin/String.h"
#include "frontend/Parser.h"
#include "gc/Policy.h"
#include "jit/AtomicOperations.h"
#include "js/MemoryMetrics.h"
#include "js/Printf.h"
#include "js/Wrapper.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/ErrorReporting.h"
#include "vm/SelfHosting.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmValidate.h"

#include "frontend/ParseNode-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"

using namespace js;
using namespace js::frontend;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CeilingLog2;
using mozilla::Compression::LZ4;
using mozilla::HashGeneric;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;
using mozilla::IsPositiveZero;
using mozilla::IsPowerOfTwo;
using mozilla::Move;
using mozilla::PodEqual;
using mozilla::PodZero;
using mozilla::PositiveInfinity;
using mozilla::Unused;
using JS::AsmJSOption;
using JS::GenericNaN;

/*****************************************************************************/

// The asm.js valid heap lengths are precisely the WASM valid heap lengths for ARM
// greater or equal to MinHeapLength
static const size_t MinHeapLength = PageSize;

static uint32_t
RoundUpToNextValidAsmJSHeapLength(uint32_t length)
{
    if (length <= MinHeapLength)
        return MinHeapLength;

    return wasm::RoundUpToNextValidARMImmediate(length);
}


/*****************************************************************************/
// asm.js module object

// The asm.js spec recognizes this set of builtin Math functions.
enum AsmJSMathBuiltinFunction
{
    AsmJSMathBuiltin_sin, AsmJSMathBuiltin_cos, AsmJSMathBuiltin_tan,
    AsmJSMathBuiltin_asin, AsmJSMathBuiltin_acos, AsmJSMathBuiltin_atan,
    AsmJSMathBuiltin_ceil, AsmJSMathBuiltin_floor, AsmJSMathBuiltin_exp,
    AsmJSMathBuiltin_log, AsmJSMathBuiltin_pow, AsmJSMathBuiltin_sqrt,
    AsmJSMathBuiltin_abs, AsmJSMathBuiltin_atan2, AsmJSMathBuiltin_imul,
    AsmJSMathBuiltin_fround, AsmJSMathBuiltin_min, AsmJSMathBuiltin_max,
    AsmJSMathBuiltin_clz32
};

// The asm.js spec will recognize this set of builtin Atomics functions.
enum AsmJSAtomicsBuiltinFunction
{
    AsmJSAtomicsBuiltin_compareExchange,
    AsmJSAtomicsBuiltin_exchange,
    AsmJSAtomicsBuiltin_load,
    AsmJSAtomicsBuiltin_store,
    AsmJSAtomicsBuiltin_add,
    AsmJSAtomicsBuiltin_sub,
    AsmJSAtomicsBuiltin_and,
    AsmJSAtomicsBuiltin_or,
    AsmJSAtomicsBuiltin_xor,
    AsmJSAtomicsBuiltin_isLockFree
};


// An AsmJSGlobal represents a JS global variable in the asm.js module function.
class AsmJSGlobal
{
  public:
    enum Which { Variable, FFI, ArrayView, ArrayViewCtor, MathBuiltinFunction,
                 AtomicsBuiltinFunction, Constant, SimdCtor, SimdOp };
    enum VarInitKind { InitConstant, InitImport };
    enum ConstantKind { GlobalConstant, MathConstant };

  private:
    struct CacheablePod {
        Which which_;
        union V {
            struct {
                VarInitKind initKind_;
                union U {
                    ValType importType_;
                    Val val_;
                    U() {}
                } u;
            } var;
            uint32_t ffiIndex_;
            Scalar::Type viewType_;
            AsmJSMathBuiltinFunction mathBuiltinFunc_;
            AsmJSAtomicsBuiltinFunction atomicsBuiltinFunc_;
            SimdType simdCtorType_;
            struct {
                SimdType type_;
                SimdOperation which_;
            } simdOp;
            struct {
                ConstantKind kind_;
                double value_;
            } constant;
            V() {}
        } u;
    } pod;
    CacheableChars field_;

    friend class ModuleValidator;

  public:
    AsmJSGlobal() = default;
    AsmJSGlobal(Which which, UniqueChars field) {
        mozilla::PodZero(&pod);  // zero padding for Valgrind
        pod.which_ = which;
        field_ = Move(field);
    }
    const char* field() const {
        return field_.get();
    }
    Which which() const {
        return pod.which_;
    }
    VarInitKind varInitKind() const {
        MOZ_ASSERT(pod.which_ == Variable);
        return pod.u.var.initKind_;
    }
    Val varInitVal() const {
        MOZ_ASSERT(pod.which_ == Variable);
        MOZ_ASSERT(pod.u.var.initKind_ == InitConstant);
        return pod.u.var.u.val_;
    }
    ValType varInitImportType() const {
        MOZ_ASSERT(pod.which_ == Variable);
        MOZ_ASSERT(pod.u.var.initKind_ == InitImport);
        return pod.u.var.u.importType_;
    }
    uint32_t ffiIndex() const {
        MOZ_ASSERT(pod.which_ == FFI);
        return pod.u.ffiIndex_;
    }
    // When a view is created from an imported constructor:
    //   var I32 = stdlib.Int32Array;
    //   var i32 = new I32(buffer);
    // the second import has nothing to validate and thus has a null field.
    Scalar::Type viewType() const {
        MOZ_ASSERT(pod.which_ == ArrayView || pod.which_ == ArrayViewCtor);
        return pod.u.viewType_;
    }
    AsmJSMathBuiltinFunction mathBuiltinFunction() const {
        MOZ_ASSERT(pod.which_ == MathBuiltinFunction);
        return pod.u.mathBuiltinFunc_;
    }
    AsmJSAtomicsBuiltinFunction atomicsBuiltinFunction() const {
        MOZ_ASSERT(pod.which_ == AtomicsBuiltinFunction);
        return pod.u.atomicsBuiltinFunc_;
    }
    SimdType simdCtorType() const {
        MOZ_ASSERT(pod.which_ == SimdCtor);
        return pod.u.simdCtorType_;
    }
    SimdOperation simdOperation() const {
        MOZ_ASSERT(pod.which_ == SimdOp);
        return pod.u.simdOp.which_;
    }
    SimdType simdOperationType() const {
        MOZ_ASSERT(pod.which_ == SimdOp);
        return pod.u.simdOp.type_;
    }
    ConstantKind constantKind() const {
        MOZ_ASSERT(pod.which_ == Constant);
        return pod.u.constant.kind_;
    }
    double constantValue() const {
        MOZ_ASSERT(pod.which_ == Constant);
        return pod.u.constant.value_;
    }

    WASM_DECLARE_SERIALIZABLE(AsmJSGlobal);
};

typedef Vector<AsmJSGlobal, 0, SystemAllocPolicy> AsmJSGlobalVector;

// An AsmJSImport is slightly different than an asm.js FFI function: a single
// asm.js FFI function can be called with many different signatures. When
// compiled to wasm, each unique FFI function paired with signature generates a
// wasm import.
class AsmJSImport
{
    uint32_t ffiIndex_;
  public:
    AsmJSImport() = default;
    explicit AsmJSImport(uint32_t ffiIndex) : ffiIndex_(ffiIndex) {}
    uint32_t ffiIndex() const { return ffiIndex_; }
};

typedef Vector<AsmJSImport, 0, SystemAllocPolicy> AsmJSImportVector;

// An AsmJSExport logically extends Export with the extra information needed for
// an asm.js exported function, viz., the offsets in module's source chars in
// case the function is toString()ed.
class AsmJSExport
{
    uint32_t funcIndex_;

    // All fields are treated as cacheable POD:
    uint32_t startOffsetInModule_;  // Store module-start-relative offsets
    uint32_t endOffsetInModule_;    // so preserved by serialization.

  public:
    AsmJSExport() { PodZero(this); }
    AsmJSExport(uint32_t funcIndex, uint32_t startOffsetInModule, uint32_t endOffsetInModule)
      : funcIndex_(funcIndex),
        startOffsetInModule_(startOffsetInModule),
        endOffsetInModule_(endOffsetInModule)
    {}
    uint32_t funcIndex() const {
        return funcIndex_;
    }
    uint32_t startOffsetInModule() const {
        return startOffsetInModule_;
    }
    uint32_t endOffsetInModule() const {
        return endOffsetInModule_;
    }
};

typedef Vector<AsmJSExport, 0, SystemAllocPolicy> AsmJSExportVector;

enum class CacheResult
{
    Hit,
    Miss
};

// Holds the immutable guts of an AsmJSModule.
//
// AsmJSMetadata is built incrementally by ModuleValidator and then shared
// immutably between AsmJSModules.

struct AsmJSMetadataCacheablePod
{
    uint32_t                numFFIs;
    uint32_t                srcLength;
    uint32_t                srcLengthWithRightBrace;
    bool                    usesSimd;

    AsmJSMetadataCacheablePod() { PodZero(this); }
};

struct js::AsmJSMetadata : Metadata, AsmJSMetadataCacheablePod
{
    AsmJSGlobalVector       asmJSGlobals;
    AsmJSImportVector       asmJSImports;
    AsmJSExportVector       asmJSExports;
    CacheableCharsVector    asmJSFuncNames;
    CacheableChars          globalArgumentName;
    CacheableChars          importArgumentName;
    CacheableChars          bufferArgumentName;

    CacheResult             cacheResult;

    // These values are not serialized since they are relative to the
    // containing script which can be different between serialization and
    // deserialization contexts. Thus, they must be set explicitly using the
    // ambient Parser/ScriptSource after deserialization.
    //
    // srcStart refers to the offset in the ScriptSource to the beginning of
    // the asm.js module function. If the function has been created with the
    // Function constructor, this will be the first character in the function
    // source. Otherwise, it will be the opening parenthesis of the arguments
    // list.
    uint32_t                toStringStart;
    uint32_t                srcStart;
    bool                    strict;
    ScriptSourceHolder      scriptSource;

    uint32_t srcEndBeforeCurly() const {
        return srcStart + srcLength;
    }
    uint32_t srcEndAfterCurly() const {
        return srcStart + srcLengthWithRightBrace;
    }

    AsmJSMetadata()
      : Metadata(ModuleKind::AsmJS),
        cacheResult(CacheResult::Miss),
        srcStart(0),
        strict(false)
    {}
    ~AsmJSMetadata() override {}

    const AsmJSExport& lookupAsmJSExport(uint32_t funcIndex) const {
        // The AsmJSExportVector isn't stored in sorted order so do a linear
        // search. This is for the super-cold and already-expensive toString()
        // path and the number of exports is generally small.
        for (const AsmJSExport& exp : asmJSExports) {
            if (exp.funcIndex() == funcIndex)
                return exp;
        }
        MOZ_CRASH("missing asm.js func export");
    }

    bool mutedErrors() const override {
        return scriptSource.get()->mutedErrors();
    }
    const char16_t* displayURL() const override {
        return scriptSource.get()->hasDisplayURL() ? scriptSource.get()->displayURL() : nullptr;
    }
    ScriptSource* maybeScriptSource() const override {
        return scriptSource.get();
    }
    bool getFuncName(const Bytes* maybeBytecode, uint32_t funcIndex, UTF8Bytes* name) const override {
        const char* p = asmJSFuncNames[funcIndex].get();
        if (!p)
            return true;
        return name->append(p, strlen(p));
    }

    AsmJSMetadataCacheablePod& pod() { return *this; }
    const AsmJSMetadataCacheablePod& pod() const { return *this; }

    WASM_DECLARE_SERIALIZABLE_OVERRIDE(AsmJSMetadata)
};

typedef RefPtr<AsmJSMetadata> MutableAsmJSMetadata;

/*****************************************************************************/
// ParseNode utilities

static inline ParseNode*
NextNode(ParseNode* pn)
{
    return pn->pn_next;
}

static inline ParseNode*
UnaryKid(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_UNARY));
    return pn->pn_kid;
}

static inline ParseNode*
BinaryRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_BINARY));
    return pn->pn_right;
}

static inline ParseNode*
BinaryLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_BINARY));
    return pn->pn_left;
}

static inline ParseNode*
ReturnExpr(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Return));
    return UnaryKid(pn);
}

static inline ParseNode*
TernaryKid1(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_TERNARY));
    return pn->pn_kid1;
}

static inline ParseNode*
TernaryKid2(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_TERNARY));
    return pn->pn_kid2;
}

static inline ParseNode*
TernaryKid3(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_TERNARY));
    return pn->pn_kid3;
}

static inline ParseNode*
ListHead(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    return pn->pn_head;
}

static inline unsigned
ListLength(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    return pn->pn_count;
}

static inline ParseNode*
CallCallee(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Call));
    return ListHead(pn);
}

static inline unsigned
CallArgListLength(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Call));
    MOZ_ASSERT(ListLength(pn) >= 1);
    return ListLength(pn) - 1;
}

static inline ParseNode*
CallArgList(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Call));
    return NextNode(ListHead(pn));
}

static inline ParseNode*
VarListHead(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Var) || pn->isKind(ParseNodeKind::Const));
    return ListHead(pn);
}

static inline bool
IsDefaultCase(ParseNode* pn)
{
    return pn->as<CaseClause>().isDefault();
}

static inline ParseNode*
CaseExpr(ParseNode* pn)
{
    return pn->as<CaseClause>().caseExpression();
}

static inline ParseNode*
CaseBody(ParseNode* pn)
{
    return pn->as<CaseClause>().statementList();
}

static inline ParseNode*
BinaryOpLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isBinaryOperation());
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(pn->pn_count == 2);
    return ListHead(pn);
}

static inline ParseNode*
BinaryOpRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isBinaryOperation());
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(pn->pn_count == 2);
    return NextNode(ListHead(pn));
}

static inline ParseNode*
BitwiseLeft(ParseNode* pn)
{
    return BinaryOpLeft(pn);
}

static inline ParseNode*
BitwiseRight(ParseNode* pn)
{
    return BinaryOpRight(pn);
}

static inline ParseNode*
MultiplyLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Star));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
MultiplyRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Star));
    return BinaryOpRight(pn);
}

static inline ParseNode*
AddSubLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Add) || pn->isKind(ParseNodeKind::Sub));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
AddSubRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Add) || pn->isKind(ParseNodeKind::Sub));
    return BinaryOpRight(pn);
}

static inline ParseNode*
DivOrModLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Div) || pn->isKind(ParseNodeKind::Mod));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
DivOrModRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Div) || pn->isKind(ParseNodeKind::Mod));
    return BinaryOpRight(pn);
}

static inline ParseNode*
ComparisonLeft(ParseNode* pn)
{
    return BinaryOpLeft(pn);
}

static inline ParseNode*
ComparisonRight(ParseNode* pn)
{
    return BinaryOpRight(pn);
}

static inline bool
IsExpressionStatement(ParseNode* pn)
{
    return pn->isKind(ParseNodeKind::ExpressionStatement);
}

static inline ParseNode*
ExpressionStatementExpr(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::ExpressionStatement));
    return UnaryKid(pn);
}

static inline PropertyName*
LoopControlMaybeLabel(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Break) || pn->isKind(ParseNodeKind::Continue));
    MOZ_ASSERT(pn->isArity(PN_NULLARY));
    return pn->as<LoopControlStatement>().label();
}

static inline PropertyName*
LabeledStatementLabel(ParseNode* pn)
{
    return pn->as<LabeledStatement>().label();
}

static inline ParseNode*
LabeledStatementStatement(ParseNode* pn)
{
    return pn->as<LabeledStatement>().statement();
}

static double
NumberNodeValue(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Number));
    return pn->pn_dval;
}

static bool
NumberNodeHasFrac(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Number));
    return pn->pn_u.number.decimalPoint == HasDecimal;
}

static ParseNode*
DotBase(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Dot));
    MOZ_ASSERT(pn->isArity(PN_NAME));
    return pn->expr();
}

static PropertyName*
DotMember(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Dot));
    MOZ_ASSERT(pn->isArity(PN_NAME));
    return pn->pn_atom->asPropertyName();
}

static ParseNode*
ElemBase(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Elem));
    return BinaryLeft(pn);
}

static ParseNode*
ElemIndex(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Elem));
    return BinaryRight(pn);
}

static inline JSFunction*
FunctionObject(ParseNode* fn)
{
    MOZ_ASSERT(fn->isKind(ParseNodeKind::Function));
    MOZ_ASSERT(fn->isArity(PN_CODE));
    return fn->pn_funbox->function();
}

static inline PropertyName*
FunctionName(ParseNode* fn)
{
    if (JSAtom* name = FunctionObject(fn)->explicitName())
        return name->asPropertyName();
    return nullptr;
}

static inline ParseNode*
FunctionStatementList(ParseNode* fn)
{
    MOZ_ASSERT(fn->pn_body->isKind(ParseNodeKind::ParamsBody));
    ParseNode* last = fn->pn_body->last();
    MOZ_ASSERT(last->isKind(ParseNodeKind::LexicalScope));
    MOZ_ASSERT(last->isEmptyScope());
    ParseNode* body = last->scopeBody();
    MOZ_ASSERT(body->isKind(ParseNodeKind::StatementList));
    return body;
}

static inline bool
IsNormalObjectField(ParseNode* pn)
{
    return pn->isKind(ParseNodeKind::Colon) &&
           pn->getOp() == JSOP_INITPROP &&
           BinaryLeft(pn)->isKind(ParseNodeKind::ObjectPropertyName);
}

static inline PropertyName*
ObjectNormalFieldName(ParseNode* pn)
{
    MOZ_ASSERT(IsNormalObjectField(pn));
    MOZ_ASSERT(BinaryLeft(pn)->isKind(ParseNodeKind::ObjectPropertyName));
    return BinaryLeft(pn)->pn_atom->asPropertyName();
}

static inline ParseNode*
ObjectNormalFieldInitializer(ParseNode* pn)
{
    MOZ_ASSERT(IsNormalObjectField(pn));
    return BinaryRight(pn);
}

static inline ParseNode*
MaybeInitializer(ParseNode* pn)
{
    return pn->expr();
}

static inline bool
IsUseOfName(ParseNode* pn, PropertyName* name)
{
    return pn->isKind(ParseNodeKind::Name) && pn->name() == name;
}

static inline bool
IsIgnoredDirectiveName(JSContext* cx, JSAtom* atom)
{
    return atom != cx->names().useStrict;
}

static inline bool
IsIgnoredDirective(JSContext* cx, ParseNode* pn)
{
    return pn->isKind(ParseNodeKind::ExpressionStatement) &&
           UnaryKid(pn)->isKind(ParseNodeKind::String) &&
           IsIgnoredDirectiveName(cx, UnaryKid(pn)->pn_atom);
}

static inline bool
IsEmptyStatement(ParseNode* pn)
{
    return pn->isKind(ParseNodeKind::EmptyStatement);
}

static inline ParseNode*
SkipEmptyStatements(ParseNode* pn)
{
    while (pn && IsEmptyStatement(pn))
        pn = pn->pn_next;
    return pn;
}

static inline ParseNode*
NextNonEmptyStatement(ParseNode* pn)
{
    return SkipEmptyStatements(pn->pn_next);
}

static bool
GetToken(AsmJSParser& parser, TokenKind* tkp)
{
    auto& ts = parser.tokenStream;
    TokenKind tk;
    while (true) {
        if (!ts.getToken(&tk, TokenStreamShared::Operand))
            return false;
        if (tk != TokenKind::Semi)
            break;
    }
    *tkp = tk;
    return true;
}

static bool
PeekToken(AsmJSParser& parser, TokenKind* tkp)
{
    auto& ts = parser.tokenStream;
    TokenKind tk;
    while (true) {
        if (!ts.peekToken(&tk, TokenStream::Operand))
            return false;
        if (tk != TokenKind::Semi)
            break;
        ts.consumeKnownToken(TokenKind::Semi, TokenStreamShared::Operand);
    }
    *tkp = tk;
    return true;
}

static bool
ParseVarOrConstStatement(AsmJSParser& parser, ParseNode** var)
{
    TokenKind tk;
    if (!PeekToken(parser, &tk))
        return false;
    if (tk != TokenKind::Var && tk != TokenKind::Const) {
        *var = nullptr;
        return true;
    }

    *var = parser.statementListItem(YieldIsName);
    if (!*var)
        return false;

    MOZ_ASSERT((*var)->isKind(ParseNodeKind::Var) || (*var)->isKind(ParseNodeKind::Const));
    return true;
}

/*****************************************************************************/

// Represents the type and value of an asm.js numeric literal.
//
// A literal is a double iff the literal contains a decimal point (even if the
// fractional part is 0). Otherwise, integers may be classified:
//  fixnum: [0, 2^31)
//  negative int: [-2^31, 0)
//  big unsigned: [2^31, 2^32)
//  out of range: otherwise
// Lastly, a literal may be a float literal which is any double or integer
// literal coerced with Math.fround.
//
// This class distinguishes between signed and unsigned integer SIMD types like
// Int32x4 and Uint32x4, and so does Type below. The wasm ValType and ExprType
// enums, and the wasm::Val class do not.
class NumLit
{
  public:
    enum Which {
        Fixnum,
        NegativeInt,
        BigUnsigned,
        Double,
        Float,
        Int8x16,
        Int16x8,
        Int32x4,
        Uint8x16,
        Uint16x8,
        Uint32x4,
        Float32x4,
        Bool8x16,
        Bool16x8,
        Bool32x4,
        OutOfRangeInt = -1
    };

  private:
    Which which_;
    union {
        JS::UninitializedValue scalar_;
        SimdConstant simd_;
    } u;

  public:
    NumLit() = default;

    NumLit(Which w, const Value& v) : which_(w) {
        u.scalar_ = v;
        MOZ_ASSERT(!isSimd());
    }

    NumLit(Which w, SimdConstant c) : which_(w) {
        u.simd_ = c;
        MOZ_ASSERT(isSimd());
    }

    Which which() const {
        return which_;
    }

    int32_t toInt32() const {
        MOZ_ASSERT(which_ == Fixnum || which_ == NegativeInt || which_ == BigUnsigned);
        return u.scalar_.asValueRef().toInt32();
    }

    uint32_t toUint32() const {
        return (uint32_t)toInt32();
    }

    double toDouble() const {
        MOZ_ASSERT(which_ == Double);
        return u.scalar_.asValueRef().toDouble();
    }

    float toFloat() const {
        MOZ_ASSERT(which_ == Float);
        return float(u.scalar_.asValueRef().toDouble());
    }

    Value scalarValue() const {
        MOZ_ASSERT(which_ != OutOfRangeInt);
        return u.scalar_.asValueRef();
    }

    bool isSimd() const
    {
        return which_ == Int8x16 || which_ == Uint8x16 || which_ == Int16x8 ||
               which_ == Uint16x8 || which_ == Int32x4 || which_ == Uint32x4 ||
               which_ == Float32x4 || which_ == Bool8x16 || which_ == Bool16x8 ||
               which_ == Bool32x4;
    }

    const SimdConstant& simdValue() const {
        MOZ_ASSERT(isSimd());
        return u.simd_;
    }

    bool valid() const {
        return which_ != OutOfRangeInt;
    }

    bool isZeroBits() const {
        MOZ_ASSERT(valid());
        switch (which()) {
          case NumLit::Fixnum:
          case NumLit::NegativeInt:
          case NumLit::BigUnsigned:
            return toInt32() == 0;
          case NumLit::Double:
            return IsPositiveZero(toDouble());
          case NumLit::Float:
            return IsPositiveZero(toFloat());
          case NumLit::Int8x16:
          case NumLit::Uint8x16:
          case NumLit::Bool8x16:
            return simdValue() == SimdConstant::SplatX16(0);
          case NumLit::Int16x8:
          case NumLit::Uint16x8:
          case NumLit::Bool16x8:
            return simdValue() == SimdConstant::SplatX8(0);
          case NumLit::Int32x4:
          case NumLit::Uint32x4:
          case NumLit::Bool32x4:
            return simdValue() == SimdConstant::SplatX4(0);
          case NumLit::Float32x4:
            return simdValue() == SimdConstant::SplatX4(0.f);
          case NumLit::OutOfRangeInt:
            MOZ_CRASH("can't be here because of valid() check above");
        }
        return false;
    }

    Val value() const {
        switch (which_) {
          case NumLit::Fixnum:
          case NumLit::NegativeInt:
          case NumLit::BigUnsigned:
            return Val(toUint32());
          case NumLit::Float:
            return Val(toFloat());
          case NumLit::Double:
            return Val(toDouble());
          case NumLit::Int8x16:
          case NumLit::Uint8x16:
            return Val(simdValue().asInt8x16());
          case NumLit::Int16x8:
          case NumLit::Uint16x8:
            return Val(simdValue().asInt16x8());
          case NumLit::Int32x4:
          case NumLit::Uint32x4:
            return Val(simdValue().asInt32x4());
          case NumLit::Float32x4:
            return Val(simdValue().asFloat32x4());
          case NumLit::Bool8x16:
            return Val(simdValue().asInt8x16(), ValType::B8x16);
          case NumLit::Bool16x8:
            return Val(simdValue().asInt16x8(), ValType::B16x8);
          case NumLit::Bool32x4:
            return Val(simdValue().asInt32x4(), ValType::B32x4);
          case NumLit::OutOfRangeInt:;
        }
        MOZ_CRASH("bad literal");
    }
};

// Represents the type of a general asm.js expression.
//
// A canonical subset of types representing the coercion targets: Int, Float,
// Double, and the SIMD types. This is almost equivalent to wasm::ValType,
// except the integer SIMD types have signed/unsigned variants.
//
// Void is also part of the canonical subset which then maps to wasm::ExprType.
//
// Note that while the canonical subset distinguishes signed and unsigned SIMD
// types, it only uses |Int| to represent signed and unsigned 32-bit integers.
// This is because the scalar coersions x|0 and x>>>0 work with any kind of
// integer input, while the SIMD check functions throw a TypeError if the passed
// type doesn't match.
//
class Type
{
  public:
    enum Which {
        Fixnum = NumLit::Fixnum,
        Signed = NumLit::NegativeInt,
        Unsigned = NumLit::BigUnsigned,
        DoubleLit = NumLit::Double,
        Float = NumLit::Float,
        Int8x16 = NumLit::Int8x16,
        Int16x8 = NumLit::Int16x8,
        Int32x4 = NumLit::Int32x4,
        Uint8x16 = NumLit::Uint8x16,
        Uint16x8 = NumLit::Uint16x8,
        Uint32x4 = NumLit::Uint32x4,
        Float32x4 = NumLit::Float32x4,
        Bool8x16 = NumLit::Bool8x16,
        Bool16x8 = NumLit::Bool16x8,
        Bool32x4 = NumLit::Bool32x4,
        Double,
        MaybeDouble,
        MaybeFloat,
        Floatish,
        Int,
        Intish,
        Void
    };

  private:
    Which which_;

  public:
    Type() = default;
    MOZ_IMPLICIT Type(Which w) : which_(w) {}
    MOZ_IMPLICIT Type(SimdType type) {
        switch (type) {
          case SimdType::Int8x16:   which_ = Int8x16;   return;
          case SimdType::Int16x8:   which_ = Int16x8;   return;
          case SimdType::Int32x4:   which_ = Int32x4;   return;
          case SimdType::Uint8x16:  which_ = Uint8x16;  return;
          case SimdType::Uint16x8:  which_ = Uint16x8;  return;
          case SimdType::Uint32x4:  which_ = Uint32x4;  return;
          case SimdType::Float32x4: which_ = Float32x4; return;
          case SimdType::Bool8x16:  which_ = Bool8x16;  return;
          case SimdType::Bool16x8:  which_ = Bool16x8;  return;
          case SimdType::Bool32x4:  which_ = Bool32x4;  return;
          default:                  break;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("bad SimdType");
    }

    // Map an already canonicalized Type to the return type of a function call.
    static Type ret(Type t) {
        MOZ_ASSERT(t.isCanonical());
        // The 32-bit external type is Signed, not Int.
        return t.isInt() ? Signed: t;
    }

    static Type lit(const NumLit& lit) {
        MOZ_ASSERT(lit.valid());
        Which which = Type::Which(lit.which());
        MOZ_ASSERT(which >= Fixnum && which <= Bool32x4);
        Type t;
        t.which_ = which;
        return t;
    }

    // Map |t| to one of the canonical vartype representations of a
    // wasm::ExprType.
    static Type canonicalize(Type t) {
        switch(t.which()) {
          case Fixnum:
          case Signed:
          case Unsigned:
          case Int:
            return Int;

          case Float:
            return Float;

          case DoubleLit:
          case Double:
            return Double;

          case Void:
            return Void;

          case Int8x16:
          case Int16x8:
          case Int32x4:
          case Uint8x16:
          case Uint16x8:
          case Uint32x4:
          case Float32x4:
          case Bool8x16:
          case Bool16x8:
          case Bool32x4:
            return t;

          case MaybeDouble:
          case MaybeFloat:
          case Floatish:
          case Intish:
            // These types need some kind of coercion, they can't be mapped
            // to an ExprType.
            break;
        }
        MOZ_CRASH("Invalid vartype");
    }

    Which which() const { return which_; }

    bool operator==(Type rhs) const { return which_ == rhs.which_; }
    bool operator!=(Type rhs) const { return which_ != rhs.which_; }

    bool operator<=(Type rhs) const {
        switch (rhs.which_) {
          case Signed:      return isSigned();
          case Unsigned:    return isUnsigned();
          case DoubleLit:   return isDoubleLit();
          case Double:      return isDouble();
          case Float:       return isFloat();
          case Int8x16:     return isInt8x16();
          case Int16x8:     return isInt16x8();
          case Int32x4:     return isInt32x4();
          case Uint8x16:    return isUint8x16();
          case Uint16x8:    return isUint16x8();
          case Uint32x4:    return isUint32x4();
          case Float32x4:   return isFloat32x4();
          case Bool8x16:    return isBool8x16();
          case Bool16x8:    return isBool16x8();
          case Bool32x4:    return isBool32x4();
          case MaybeDouble: return isMaybeDouble();
          case MaybeFloat:  return isMaybeFloat();
          case Floatish:    return isFloatish();
          case Int:         return isInt();
          case Intish:      return isIntish();
          case Fixnum:      return isFixnum();
          case Void:        return isVoid();
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected rhs type");
    }

    bool isFixnum() const {
        return which_ == Fixnum;
    }

    bool isSigned() const {
        return which_ == Signed || which_ == Fixnum;
    }

    bool isUnsigned() const {
        return which_ == Unsigned || which_ == Fixnum;
    }

    bool isInt() const {
        return isSigned() || isUnsigned() || which_ == Int;
    }

    bool isIntish() const {
        return isInt() || which_ == Intish;
    }

    bool isDoubleLit() const {
        return which_ == DoubleLit;
    }

    bool isDouble() const {
        return isDoubleLit() || which_ == Double;
    }

    bool isMaybeDouble() const {
        return isDouble() || which_ == MaybeDouble;
    }

    bool isFloat() const {
        return which_ == Float;
    }

    bool isMaybeFloat() const {
        return isFloat() || which_ == MaybeFloat;
    }

    bool isFloatish() const {
        return isMaybeFloat() || which_ == Floatish;
    }

    bool isVoid() const {
        return which_ == Void;
    }

    bool isExtern() const {
        return isDouble() || isSigned();
    }

    bool isInt8x16() const {
        return which_ == Int8x16;
    }

    bool isInt16x8() const {
        return which_ == Int16x8;
    }

    bool isInt32x4() const {
        return which_ == Int32x4;
    }

    bool isUint8x16() const {
        return which_ == Uint8x16;
    }

    bool isUint16x8() const {
        return which_ == Uint16x8;
    }

    bool isUint32x4() const {
        return which_ == Uint32x4;
    }

    bool isFloat32x4() const {
        return which_ == Float32x4;
    }

    bool isBool8x16() const {
        return which_ == Bool8x16;
    }

    bool isBool16x8() const {
        return which_ == Bool16x8;
    }

    bool isBool32x4() const {
        return which_ == Bool32x4;
    }

    bool isSimd() const {
        return isInt8x16() || isInt16x8() || isInt32x4() || isUint8x16() || isUint16x8() ||
               isUint32x4() || isFloat32x4() || isBool8x16() || isBool16x8() || isBool32x4();
    }

    bool isUnsignedSimd() const {
        return isUint8x16() || isUint16x8() || isUint32x4();
    }

    // Check if this is one of the valid types for a function argument.
    bool isArgType() const {
        return isInt() || isFloat() || isDouble() || (isSimd() && !isUnsignedSimd());
    }

    // Check if this is one of the valid types for a function return value.
    bool isReturnType() const {
        return isSigned() || isFloat() || isDouble() || (isSimd() && !isUnsignedSimd()) ||
               isVoid();
    }

    // Check if this is one of the valid types for a global variable.
    bool isGlobalVarType() const {
        return isArgType();
    }

    // Check if this is one of the canonical vartype representations of a
    // wasm::ExprType. See Type::canonicalize().
    bool isCanonical() const {
        switch (which()) {
          case Int:
          case Float:
          case Double:
          case Void:
            return true;
          default:
            return isSimd();
        }
    }

    // Check if this is a canonical representation of a wasm::ValType.
    bool isCanonicalValType() const {
        return !isVoid() && isCanonical();
    }

    // Convert this canonical type to a wasm::ExprType.
    ExprType canonicalToExprType() const {
        switch (which()) {
          case Int:       return ExprType::I32;
          case Float:     return ExprType::F32;
          case Double:    return ExprType::F64;
          case Void:      return ExprType::Void;
          case Uint8x16:
          case Int8x16:   return ExprType::I8x16;
          case Uint16x8:
          case Int16x8:   return ExprType::I16x8;
          case Uint32x4:
          case Int32x4:   return ExprType::I32x4;
          case Float32x4: return ExprType::F32x4;
          case Bool8x16:  return ExprType::B8x16;
          case Bool16x8:  return ExprType::B16x8;
          case Bool32x4:  return ExprType::B32x4;
          default:        MOZ_CRASH("Need canonical type");
        }
    }

    // Convert this canonical type to a wasm::ValType.
    ValType canonicalToValType() const {
        return NonVoidToValType(canonicalToExprType());
    }

    // Convert this type to a wasm::ExprType for use in a wasm
    // block signature. This works for all types, including non-canonical
    // ones. Consequently, the type isn't valid for subsequent asm.js
    // validation; it's only valid for use in producing wasm.
    ExprType toWasmBlockSignatureType() const {
        switch (which()) {
          case Fixnum:
          case Signed:
          case Unsigned:
          case Int:
          case Intish:
            return ExprType::I32;

          case Float:
          case MaybeFloat:
          case Floatish:
            return ExprType::F32;

          case DoubleLit:
          case Double:
          case MaybeDouble:
            return ExprType::F64;

          case Void:
            return ExprType::Void;

          case Uint8x16:
          case Int8x16:   return ExprType::I8x16;
          case Uint16x8:
          case Int16x8:   return ExprType::I16x8;
          case Uint32x4:
          case Int32x4:   return ExprType::I32x4;
          case Float32x4: return ExprType::F32x4;
          case Bool8x16:  return ExprType::B8x16;
          case Bool16x8:  return ExprType::B16x8;
          case Bool32x4:  return ExprType::B32x4;
        }
        MOZ_CRASH("Invalid Type");
    }

    const char* toChars() const {
        switch (which_) {
          case Double:      return "double";
          case DoubleLit:   return "doublelit";
          case MaybeDouble: return "double?";
          case Float:       return "float";
          case Floatish:    return "floatish";
          case MaybeFloat:  return "float?";
          case Fixnum:      return "fixnum";
          case Int:         return "int";
          case Signed:      return "signed";
          case Unsigned:    return "unsigned";
          case Intish:      return "intish";
          case Int8x16:     return "int8x16";
          case Int16x8:     return "int16x8";
          case Int32x4:     return "int32x4";
          case Uint8x16:    return "uint8x16";
          case Uint16x8:    return "uint16x8";
          case Uint32x4:    return "uint32x4";
          case Float32x4:   return "float32x4";
          case Bool8x16:    return "bool8x16";
          case Bool16x8:    return "bool16x8";
          case Bool32x4:    return "bool32x4";
          case Void:        return "void";
        }
        MOZ_CRASH("Invalid Type");
    }
};

static const unsigned VALIDATION_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;

// The ModuleValidator encapsulates the entire validation of an asm.js module.
// Its lifetime goes from the validation of the top components of an asm.js
// module (all the globals), the emission of bytecode for all the functions in
// the module and the validation of function's pointer tables. It also finishes
// the compilation of all the module's stubs.
//
// Rooting note: ModuleValidator is a stack class that contains unrooted
// PropertyName (JSAtom) pointers.  This is safe because it cannot be
// constructed without a TokenStream reference.  TokenStream is itself a stack
// class that cannot be constructed without an AutoKeepAtoms being live on the
// stack, which prevents collection of atoms.
//
// ModuleValidator is marked as rooted in the rooting analysis.  Don't add
// non-JSAtom pointers, or this will break!
class MOZ_STACK_CLASS ModuleValidator
{
  public:
    class Func
    {
        PropertyName* name_;
        uint32_t sigIndex_;
        uint32_t firstUse_;
        uint32_t funcDefIndex_;

        bool defined_;

        // Available when defined:
        uint32_t srcBegin_;
        uint32_t srcEnd_;
        uint32_t line_;
        Bytes bytes_;
        Uint32Vector callSiteLineNums_;

      public:
        Func(PropertyName* name, uint32_t sigIndex, uint32_t firstUse, uint32_t funcDefIndex)
          : name_(name), sigIndex_(sigIndex), firstUse_(firstUse), funcDefIndex_(funcDefIndex),
            defined_(false), srcBegin_(0), srcEnd_(0), line_(0)
        {}

        PropertyName* name() const { return name_; }
        uint32_t sigIndex() const { return sigIndex_; }
        uint32_t firstUse() const { return firstUse_; }
        bool defined() const { return defined_; }
        uint32_t funcDefIndex() const { return funcDefIndex_; }

        void define(ParseNode* fn, uint32_t line, Bytes&& bytes, Uint32Vector&& callSiteLineNums) {
            MOZ_ASSERT(!defined_);
            defined_ = true;
            srcBegin_ = fn->pn_pos.begin;
            srcEnd_ = fn->pn_pos.end;
            line_ = line;
            bytes_ = Move(bytes);
            callSiteLineNums_ = Move(callSiteLineNums);
        }

        uint32_t srcBegin() const { MOZ_ASSERT(defined_); return srcBegin_; }
        uint32_t srcEnd() const { MOZ_ASSERT(defined_); return srcEnd_; }
        uint32_t line() const { MOZ_ASSERT(defined_); return line_; }
        const Bytes& bytes() const { MOZ_ASSERT(defined_); return bytes_; }
        Uint32Vector& callSiteLineNums() { MOZ_ASSERT(defined_); return callSiteLineNums_; }
    };

    typedef Vector<const Func*> ConstFuncVector;
    typedef Vector<Func> FuncVector;

    class Table
    {
        uint32_t sigIndex_;
        PropertyName* name_;
        uint32_t firstUse_;
        uint32_t mask_;
        bool defined_;

        Table(Table&& rhs) = delete;

      public:
        Table(uint32_t sigIndex, PropertyName* name, uint32_t firstUse, uint32_t mask)
          : sigIndex_(sigIndex), name_(name), firstUse_(firstUse), mask_(mask), defined_(false)
        {}

        uint32_t sigIndex() const { return sigIndex_; }
        PropertyName* name() const { return name_; }
        uint32_t firstUse() const { return firstUse_; }
        unsigned mask() const { return mask_; }
        bool defined() const { return defined_; }
        void define() { MOZ_ASSERT(!defined_); defined_ = true; }
    };

    typedef Vector<Table*> TableVector;

    class Global
    {
      public:
        enum Which {
            Variable,
            ConstantLiteral,
            ConstantImport,
            Function,
            Table,
            FFI,
            ArrayView,
            ArrayViewCtor,
            MathBuiltinFunction,
            AtomicsBuiltinFunction,
            SimdCtor,
            SimdOp
        };

      private:
        Which which_;
        union {
            struct {
                Type::Which type_;
                unsigned index_;
                NumLit literalValue_;
            } varOrConst;
            uint32_t funcDefIndex_;
            uint32_t tableIndex_;
            uint32_t ffiIndex_;
            struct {
                Scalar::Type viewType_;
            } viewInfo;
            AsmJSMathBuiltinFunction mathBuiltinFunc_;
            AsmJSAtomicsBuiltinFunction atomicsBuiltinFunc_;
            SimdType simdCtorType_;
            struct {
                SimdType type_;
                SimdOperation which_;
            } simdOp;
        } u;

        friend class ModuleValidator;
        friend class js::LifoAlloc;

        explicit Global(Which which) : which_(which) {}

      public:
        Which which() const {
            return which_;
        }
        Type varOrConstType() const {
            MOZ_ASSERT(which_ == Variable || which_ == ConstantLiteral || which_ == ConstantImport);
            return u.varOrConst.type_;
        }
        unsigned varOrConstIndex() const {
            MOZ_ASSERT(which_ == Variable || which_ == ConstantImport);
            return u.varOrConst.index_;
        }
        bool isConst() const {
            return which_ == ConstantLiteral || which_ == ConstantImport;
        }
        NumLit constLiteralValue() const {
            MOZ_ASSERT(which_ == ConstantLiteral);
            return u.varOrConst.literalValue_;
        }
        uint32_t funcDefIndex() const {
            MOZ_ASSERT(which_ == Function);
            return u.funcDefIndex_;
        }
        uint32_t tableIndex() const {
            MOZ_ASSERT(which_ == Table);
            return u.tableIndex_;
        }
        unsigned ffiIndex() const {
            MOZ_ASSERT(which_ == FFI);
            return u.ffiIndex_;
        }
        bool isAnyArrayView() const {
            return which_ == ArrayView || which_ == ArrayViewCtor;
        }
        Scalar::Type viewType() const {
            MOZ_ASSERT(isAnyArrayView());
            return u.viewInfo.viewType_;
        }
        bool isMathFunction() const {
            return which_ == MathBuiltinFunction;
        }
        AsmJSMathBuiltinFunction mathBuiltinFunction() const {
            MOZ_ASSERT(which_ == MathBuiltinFunction);
            return u.mathBuiltinFunc_;
        }
        bool isAtomicsFunction() const {
            return which_ == AtomicsBuiltinFunction;
        }
        AsmJSAtomicsBuiltinFunction atomicsBuiltinFunction() const {
            MOZ_ASSERT(which_ == AtomicsBuiltinFunction);
            return u.atomicsBuiltinFunc_;
        }
        bool isSimdCtor() const {
            return which_ == SimdCtor;
        }
        SimdType simdCtorType() const {
            MOZ_ASSERT(which_ == SimdCtor);
            return u.simdCtorType_;
        }
        bool isSimdOperation() const {
            return which_ == SimdOp;
        }
        SimdOperation simdOperation() const {
            MOZ_ASSERT(which_ == SimdOp);
            return u.simdOp.which_;
        }
        SimdType simdOperationType() const {
            MOZ_ASSERT(which_ == SimdOp);
            return u.simdOp.type_;
        }
    };

    struct MathBuiltin
    {
        enum Kind { Function, Constant };
        Kind kind;

        union {
            double cst;
            AsmJSMathBuiltinFunction func;
        } u;

        MathBuiltin() : kind(Kind(-1)) {}
        explicit MathBuiltin(double cst) : kind(Constant) {
            u.cst = cst;
        }
        explicit MathBuiltin(AsmJSMathBuiltinFunction func) : kind(Function) {
            u.func = func;
        }
    };

    struct ArrayView
    {
        ArrayView(PropertyName* name, Scalar::Type type)
          : name(name), type(type)
        {}

        PropertyName* name;
        Scalar::Type type;
    };

  private:
    class HashableSig
    {
        uint32_t sigIndex_;
        const SigWithIdVector& sigs_;

      public:
        HashableSig(uint32_t sigIndex, const SigWithIdVector& sigs)
          : sigIndex_(sigIndex), sigs_(sigs)
        {}
        uint32_t sigIndex() const {
            return sigIndex_;
        }
        const Sig& sig() const {
            return sigs_[sigIndex_];
        }

        // Implement HashPolicy:
        typedef const Sig& Lookup;
        static HashNumber hash(Lookup l) {
            return l.hash();
        }
        static bool match(HashableSig lhs, Lookup rhs) {
            return lhs.sig() == rhs;
        }
    };

    class NamedSig : public HashableSig
    {
        PropertyName* name_;

      public:
        NamedSig(PropertyName* name, uint32_t sigIndex, const SigWithIdVector& sigs)
          : HashableSig(sigIndex, sigs), name_(name)
        {}
        PropertyName* name() const {
            return name_;
        }

        // Implement HashPolicy:
        struct Lookup {
            PropertyName* name;
            const Sig& sig;
            Lookup(PropertyName* name, const Sig& sig) : name(name), sig(sig) {}
        };
        static HashNumber hash(Lookup l) {
            return HashGeneric(l.name, l.sig.hash());
        }
        static bool match(NamedSig lhs, Lookup rhs) {
            return lhs.name() == rhs.name && lhs.sig() == rhs.sig;
        }
    };

    typedef HashSet<HashableSig, HashableSig> SigSet;
    typedef HashMap<NamedSig, uint32_t, NamedSig> FuncImportMap;
    typedef HashMap<PropertyName*, Global*> GlobalMap;
    typedef HashMap<PropertyName*, MathBuiltin> MathNameMap;
    typedef HashMap<PropertyName*, AsmJSAtomicsBuiltinFunction> AtomicsNameMap;
    typedef HashMap<PropertyName*, SimdOperation> SimdOperationNameMap;
    typedef Vector<ArrayView> ArrayViewVector;

    JSContext*            cx_;
    AsmJSParser&          parser_;
    ParseNode*            moduleFunctionNode_;
    PropertyName*         moduleFunctionName_;
    PropertyName*         globalArgumentName_;
    PropertyName*         importArgumentName_;
    PropertyName*         bufferArgumentName_;
    MathNameMap           standardLibraryMathNames_;
    AtomicsNameMap        standardLibraryAtomicsNames_;
    SimdOperationNameMap  standardLibrarySimdOpNames_;
    RootedFunction        dummyFunction_;

    // Validation-internal state:
    LifoAlloc             validationLifo_;
    FuncVector            funcDefs_;
    TableVector           tables_;
    GlobalMap             globalMap_;
    SigSet                sigSet_;
    FuncImportMap         funcImportMap_;
    ArrayViewVector       arrayViews_;
    bool                  atomicsPresent_;
    bool                  simdPresent_;

    // State used to build the AsmJSModule in finish():
    ModuleEnvironment     env_;
    MutableAsmJSMetadata  asmJSMetadata_;

    // Error reporting:
    UniqueChars           errorString_;
    uint32_t              errorOffset_;
    bool                  errorOverRecursed_;

    // Helpers:
    bool addStandardLibraryMathName(const char* name, AsmJSMathBuiltinFunction func) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        MathBuiltin builtin(func);
        return standardLibraryMathNames_.putNew(atom->asPropertyName(), builtin);
    }
    bool addStandardLibraryMathName(const char* name, double cst) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        MathBuiltin builtin(cst);
        return standardLibraryMathNames_.putNew(atom->asPropertyName(), builtin);
    }
    bool addStandardLibraryAtomicsName(const char* name, AsmJSAtomicsBuiltinFunction func) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        return standardLibraryAtomicsNames_.putNew(atom->asPropertyName(), func);
    }
    bool addStandardLibrarySimdOpName(const char* name, SimdOperation op) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        return standardLibrarySimdOpNames_.putNew(atom->asPropertyName(), op);
    }
    bool newSig(Sig&& sig, uint32_t* sigIndex) {
        if (env_.sigs.length() >= MaxTypes)
            return failCurrentOffset("too many signatures");

        *sigIndex = env_.sigs.length();
        return env_.sigs.append(Move(sig));
    }
    bool declareSig(Sig&& sig, uint32_t* sigIndex) {
        SigSet::AddPtr p = sigSet_.lookupForAdd(sig);
        if (p) {
            *sigIndex = p->sigIndex();
            MOZ_ASSERT(env_.sigs[*sigIndex] == sig);
            return true;
        }

        return newSig(Move(sig), sigIndex) &&
               sigSet_.add(p, HashableSig(*sigIndex, env_.sigs));
    }

  public:
    ModuleValidator(JSContext* cx, AsmJSParser& parser, ParseNode* moduleFunctionNode)
      : cx_(cx),
        parser_(parser),
        moduleFunctionNode_(moduleFunctionNode),
        moduleFunctionName_(FunctionName(moduleFunctionNode)),
        globalArgumentName_(nullptr),
        importArgumentName_(nullptr),
        bufferArgumentName_(nullptr),
        standardLibraryMathNames_(cx),
        standardLibraryAtomicsNames_(cx),
        standardLibrarySimdOpNames_(cx),
        dummyFunction_(cx),
        validationLifo_(VALIDATION_LIFO_DEFAULT_CHUNK_SIZE),
        funcDefs_(cx),
        tables_(cx),
        globalMap_(cx),
        sigSet_(cx),
        funcImportMap_(cx),
        arrayViews_(cx),
        atomicsPresent_(false),
        simdPresent_(false),
        env_(CompileMode::Once, Tier::Ion, DebugEnabled::False,
             cx->compartment()->creationOptions().getSharedMemoryAndAtomicsEnabled()
               ? Shareable::True
               : Shareable::False,
             ModuleKind::AsmJS),
        errorString_(nullptr),
        errorOffset_(UINT32_MAX),
        errorOverRecursed_(false)
    {
        env_.minMemoryLength = RoundUpToNextValidAsmJSHeapLength(0);
    }

    ~ModuleValidator() {
        if (errorString_) {
            MOZ_ASSERT(errorOffset_ != UINT32_MAX);
            typeFailure(errorOffset_, errorString_.get());
        }
        if (errorOverRecursed_)
            ReportOverRecursed(cx_);
    }

  private:
    void typeFailure(uint32_t offset, ...) {
        va_list args;
        va_start(args, offset);

        auto& ts = tokenStream();
        ErrorMetadata metadata;
        if (ts.computeErrorMetadata(&metadata, offset)) {
            if (ts.anyCharsAccess().options().throwOnAsmJSValidationFailureOption) {
                ReportCompileError(cx_, Move(metadata), nullptr, JSREPORT_ERROR,
                                   JSMSG_USE_ASM_TYPE_FAIL, args);
            } else {
                // asm.js type failure is indicated by calling one of the fail*
                // functions below.  These functions always return false to
                // halt asm.js parsing.  Whether normal parsing is attempted as
                // fallback, depends whether an exception is also set.
                //
                // If warning succeeds, no exception is set.  If warning fails,
                // an exception is set and execution will halt.  Thus it's safe
                // and correct to ignore the return value here.
                Unused << ts.anyCharsAccess().compileWarning(Move(metadata), nullptr,
                                                             JSREPORT_WARNING,
                                                             JSMSG_USE_ASM_TYPE_FAIL, args);
            }
        }

        va_end(args);
    }

  public:
    bool init() {
        asmJSMetadata_ = cx_->new_<AsmJSMetadata>();
        if (!asmJSMetadata_)
            return false;

        asmJSMetadata_->toStringStart = moduleFunctionNode_->pn_funbox->toStringStart;
        asmJSMetadata_->srcStart = moduleFunctionNode_->pn_body->pn_pos.begin;
        asmJSMetadata_->strict = parser_.pc->sc()->strict() &&
                                 !parser_.pc->sc()->hasExplicitUseStrict();
        asmJSMetadata_->scriptSource.reset(parser_.ss);

        if (!globalMap_.init() || !sigSet_.init() || !funcImportMap_.init())
            return false;

        if (!standardLibraryMathNames_.init() ||
            !addStandardLibraryMathName("sin", AsmJSMathBuiltin_sin) ||
            !addStandardLibraryMathName("cos", AsmJSMathBuiltin_cos) ||
            !addStandardLibraryMathName("tan", AsmJSMathBuiltin_tan) ||
            !addStandardLibraryMathName("asin", AsmJSMathBuiltin_asin) ||
            !addStandardLibraryMathName("acos", AsmJSMathBuiltin_acos) ||
            !addStandardLibraryMathName("atan", AsmJSMathBuiltin_atan) ||
            !addStandardLibraryMathName("ceil", AsmJSMathBuiltin_ceil) ||
            !addStandardLibraryMathName("floor", AsmJSMathBuiltin_floor) ||
            !addStandardLibraryMathName("exp", AsmJSMathBuiltin_exp) ||
            !addStandardLibraryMathName("log", AsmJSMathBuiltin_log) ||
            !addStandardLibraryMathName("pow", AsmJSMathBuiltin_pow) ||
            !addStandardLibraryMathName("sqrt", AsmJSMathBuiltin_sqrt) ||
            !addStandardLibraryMathName("abs", AsmJSMathBuiltin_abs) ||
            !addStandardLibraryMathName("atan2", AsmJSMathBuiltin_atan2) ||
            !addStandardLibraryMathName("imul", AsmJSMathBuiltin_imul) ||
            !addStandardLibraryMathName("clz32", AsmJSMathBuiltin_clz32) ||
            !addStandardLibraryMathName("fround", AsmJSMathBuiltin_fround) ||
            !addStandardLibraryMathName("min", AsmJSMathBuiltin_min) ||
            !addStandardLibraryMathName("max", AsmJSMathBuiltin_max) ||
            !addStandardLibraryMathName("E", M_E) ||
            !addStandardLibraryMathName("LN10", M_LN10) ||
            !addStandardLibraryMathName("LN2", M_LN2) ||
            !addStandardLibraryMathName("LOG2E", M_LOG2E) ||
            !addStandardLibraryMathName("LOG10E", M_LOG10E) ||
            !addStandardLibraryMathName("PI", M_PI) ||
            !addStandardLibraryMathName("SQRT1_2", M_SQRT1_2) ||
            !addStandardLibraryMathName("SQRT2", M_SQRT2))
        {
            return false;
        }

        if (!standardLibraryAtomicsNames_.init() ||
            !addStandardLibraryAtomicsName("compareExchange", AsmJSAtomicsBuiltin_compareExchange) ||
            !addStandardLibraryAtomicsName("exchange", AsmJSAtomicsBuiltin_exchange) ||
            !addStandardLibraryAtomicsName("load", AsmJSAtomicsBuiltin_load) ||
            !addStandardLibraryAtomicsName("store", AsmJSAtomicsBuiltin_store) ||
            !addStandardLibraryAtomicsName("add", AsmJSAtomicsBuiltin_add) ||
            !addStandardLibraryAtomicsName("sub", AsmJSAtomicsBuiltin_sub) ||
            !addStandardLibraryAtomicsName("and", AsmJSAtomicsBuiltin_and) ||
            !addStandardLibraryAtomicsName("or", AsmJSAtomicsBuiltin_or) ||
            !addStandardLibraryAtomicsName("xor", AsmJSAtomicsBuiltin_xor) ||
            !addStandardLibraryAtomicsName("isLockFree", AsmJSAtomicsBuiltin_isLockFree))
        {
            return false;
        }

#define ADDSTDLIBSIMDOPNAME(op) || !addStandardLibrarySimdOpName(#op, SimdOperation::Fn_##op)
        if (!standardLibrarySimdOpNames_.init()
            FORALL_SIMD_ASMJS_OP(ADDSTDLIBSIMDOPNAME))
        {
            return false;
        }
#undef ADDSTDLIBSIMDOPNAME

        // This flows into FunctionBox, so must be tenured.
        dummyFunction_ = NewScriptedFunction(cx_, 0, JSFunction::INTERPRETED, nullptr,
                                             /* proto = */ nullptr, gc::AllocKind::FUNCTION,
                                             TenuredObject);
        if (!dummyFunction_)
            return false;

        return true;
    }

    JSContext* cx() const                    { return cx_; }
    PropertyName* moduleFunctionName() const { return moduleFunctionName_; }
    PropertyName* globalArgumentName() const { return globalArgumentName_; }
    PropertyName* importArgumentName() const { return importArgumentName_; }
    PropertyName* bufferArgumentName() const { return bufferArgumentName_; }
    const ModuleEnvironment& env()           { return env_; }

    AsmJSParser& parser() const { return parser_; }

    auto tokenStream() const
      -> decltype(parser_.tokenStream)&
    {
        return parser_.tokenStream;
    }

    RootedFunction& dummyFunction()          { return dummyFunction_; }
    bool supportsSimd() const                { return cx_->jitSupportsSimd(); }
    bool atomicsPresent() const              { return atomicsPresent_; }
    uint32_t minMemoryLength() const         { return env_.minMemoryLength; }

    void initModuleFunctionName(PropertyName* name) {
        MOZ_ASSERT(!moduleFunctionName_);
        moduleFunctionName_ = name;
    }
    MOZ_MUST_USE bool initGlobalArgumentName(PropertyName* n) {
        globalArgumentName_ = n;
        if (n) {
            MOZ_ASSERT(n->isTenured());
            asmJSMetadata_->globalArgumentName = StringToNewUTF8CharsZ(cx_, *n);
            if (!asmJSMetadata_->globalArgumentName)
                return false;
        }
        return true;
    }
    MOZ_MUST_USE bool initImportArgumentName(PropertyName* n) {
        importArgumentName_ = n;
        if (n) {
            MOZ_ASSERT(n->isTenured());
            asmJSMetadata_->importArgumentName = StringToNewUTF8CharsZ(cx_, *n);
            if (!asmJSMetadata_->importArgumentName)
                return false;
        }
        return true;
    }
    MOZ_MUST_USE bool initBufferArgumentName(PropertyName* n) {
        bufferArgumentName_ = n;
        if (n) {
            MOZ_ASSERT(n->isTenured());
            asmJSMetadata_->bufferArgumentName = StringToNewUTF8CharsZ(cx_, *n);
            if (!asmJSMetadata_->bufferArgumentName)
                return false;
        }
        return true;
    }
    bool addGlobalVarInit(PropertyName* var, const NumLit& lit, Type type, bool isConst) {
        MOZ_ASSERT(type.isGlobalVarType());
        MOZ_ASSERT(type == Type::canonicalize(Type::lit(lit)));

        uint32_t index = env_.globals.length();
        if (!env_.globals.emplaceBack(type.canonicalToValType(), !isConst, index))
            return false;

        Global::Which which = isConst ? Global::ConstantLiteral : Global::Variable;
        Global* global = validationLifo_.new_<Global>(which);
        if (!global)
            return false;
        global->u.varOrConst.index_ = index;
        global->u.varOrConst.type_ = (isConst ? Type::lit(lit) : type).which();
        if (isConst)
            global->u.varOrConst.literalValue_ = lit;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Variable, nullptr);
        g.pod.u.var.initKind_ = AsmJSGlobal::InitConstant;
        g.pod.u.var.u.val_ = lit.value();
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addGlobalVarImport(PropertyName* var, PropertyName* field, Type type, bool isConst) {
        MOZ_ASSERT(type.isGlobalVarType());

        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        uint32_t index = env_.globals.length();
        ValType valType = type.canonicalToValType();
        if (!env_.globals.emplaceBack(valType, !isConst, index))
            return false;

        Global::Which which = isConst ? Global::ConstantImport : Global::Variable;
        Global* global = validationLifo_.new_<Global>(which);
        if (!global)
            return false;
        global->u.varOrConst.index_ = index;
        global->u.varOrConst.type_ = type.which();
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Variable, Move(fieldChars));
        g.pod.u.var.initKind_ = AsmJSGlobal::InitImport;
        g.pod.u.var.u.importType_ = valType;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addArrayView(PropertyName* var, Scalar::Type vt, PropertyName* maybeField) {
        UniqueChars fieldChars;
        if (maybeField) {
            fieldChars = StringToNewUTF8CharsZ(cx_, *maybeField);
            if (!fieldChars)
                return false;
        }

        if (!arrayViews_.append(ArrayView(var, vt)))
            return false;

        Global* global = validationLifo_.new_<Global>(Global::ArrayView);
        if (!global)
            return false;
        global->u.viewInfo.viewType_ = vt;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::ArrayView, Move(fieldChars));
        g.pod.u.viewType_ = vt;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addMathBuiltinFunction(PropertyName* var, AsmJSMathBuiltinFunction func,
                                PropertyName* field)
    {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::MathBuiltinFunction);
        if (!global)
            return false;
        global->u.mathBuiltinFunc_ = func;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::MathBuiltinFunction, Move(fieldChars));
        g.pod.u.mathBuiltinFunc_ = func;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
  private:
    bool addGlobalDoubleConstant(PropertyName* var, double constant) {
        Global* global = validationLifo_.new_<Global>(Global::ConstantLiteral);
        if (!global)
            return false;
        global->u.varOrConst.type_ = Type::Double;
        global->u.varOrConst.literalValue_ = NumLit(NumLit::Double, DoubleValue(constant));
        return globalMap_.putNew(var, global);
    }
  public:
    bool addMathBuiltinConstant(PropertyName* var, double constant, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        if (!addGlobalDoubleConstant(var, constant))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Constant, Move(fieldChars));
        g.pod.u.constant.value_ = constant;
        g.pod.u.constant.kind_ = AsmJSGlobal::MathConstant;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addGlobalConstant(PropertyName* var, double constant, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        if (!addGlobalDoubleConstant(var, constant))
            return false;

        AsmJSGlobal g(AsmJSGlobal::Constant, Move(fieldChars));
        g.pod.u.constant.value_ = constant;
        g.pod.u.constant.kind_ = AsmJSGlobal::GlobalConstant;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addAtomicsBuiltinFunction(PropertyName* var, AsmJSAtomicsBuiltinFunction func,
                                   PropertyName* field)
    {
        if (!JitOptions.asmJSAtomicsEnable)
            return failCurrentOffset("asm.js Atomics only enabled when asmjs.atomics.enable is set");

        atomicsPresent_ = true;

        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::AtomicsBuiltinFunction);
        if (!global)
            return false;
        global->u.atomicsBuiltinFunc_ = func;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::AtomicsBuiltinFunction, Move(fieldChars));
        g.pod.u.atomicsBuiltinFunc_ = func;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addSimdCtor(PropertyName* var, SimdType type, PropertyName* field) {
        simdPresent_ = true;

        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::SimdCtor);
        if (!global)
            return false;
        global->u.simdCtorType_ = type;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::SimdCtor, Move(fieldChars));
        g.pod.u.simdCtorType_ = type;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addSimdOperation(PropertyName* var, SimdType type, SimdOperation op, PropertyName* field) {
        simdPresent_ = true;

        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::SimdOp);
        if (!global)
            return false;
        global->u.simdOp.type_ = type;
        global->u.simdOp.which_ = op;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::SimdOp, Move(fieldChars));
        g.pod.u.simdOp.type_ = type;
        g.pod.u.simdOp.which_ = op;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addArrayViewCtor(PropertyName* var, Scalar::Type vt, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        Global* global = validationLifo_.new_<Global>(Global::ArrayViewCtor);
        if (!global)
            return false;
        global->u.viewInfo.viewType_ = vt;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::ArrayViewCtor, Move(fieldChars));
        g.pod.u.viewType_ = vt;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addFFI(PropertyName* var, PropertyName* field) {
        UniqueChars fieldChars = StringToNewUTF8CharsZ(cx_, *field);
        if (!fieldChars)
            return false;

        if (asmJSMetadata_->numFFIs == UINT32_MAX)
            return false;
        uint32_t ffiIndex = asmJSMetadata_->numFFIs++;

        Global* global = validationLifo_.new_<Global>(Global::FFI);
        if (!global)
            return false;
        global->u.ffiIndex_ = ffiIndex;
        if (!globalMap_.putNew(var, global))
            return false;

        AsmJSGlobal g(AsmJSGlobal::FFI, Move(fieldChars));
        g.pod.u.ffiIndex_ = ffiIndex;
        return asmJSMetadata_->asmJSGlobals.append(Move(g));
    }
    bool addExportField(const Func& func, PropertyName* maybeField) {
        // Record the field name of this export.
        CacheableChars fieldChars;
        if (maybeField)
            fieldChars = StringToNewUTF8CharsZ(cx_, *maybeField);
        else
            fieldChars = DuplicateString("");
        if (!fieldChars)
            return false;

        // Declare which function is exported which gives us an index into the
        // module ExportVector.
        uint32_t funcIndex = funcImportMap_.count() + func.funcDefIndex();
        if (!env_.exports.emplaceBack(Move(fieldChars), funcIndex, DefinitionKind::Function))
            return false;

        // The exported function might have already been exported in which case
        // the index will refer into the range of AsmJSExports.
        return asmJSMetadata_->asmJSExports.emplaceBack(funcIndex,
                                                        func.srcBegin() - asmJSMetadata_->srcStart,
                                                        func.srcEnd() - asmJSMetadata_->srcStart);
    }
    bool addFuncDef(PropertyName* name, uint32_t firstUse, Sig&& sig, Func** func) {
        uint32_t sigIndex;
        if (!declareSig(Move(sig), &sigIndex))
            return false;

        uint32_t funcDefIndex = funcDefs_.length();
        if (funcDefIndex >= MaxFuncs)
            return failCurrentOffset("too many functions");

        Global* global = validationLifo_.new_<Global>(Global::Function);
        if (!global)
            return false;
        global->u.funcDefIndex_ = funcDefIndex;
        if (!globalMap_.putNew(name, global))
            return false;
        if (!funcDefs_.emplaceBack(name, sigIndex, firstUse, funcDefIndex))
            return false;
        *func = &funcDefs_.back();
        return true;
    }
    bool declareFuncPtrTable(Sig&& sig, PropertyName* name, uint32_t firstUse, uint32_t mask,
                             uint32_t* tableIndex)
    {
        if (mask > MaxTableInitialLength)
            return failCurrentOffset("function pointer table too big");

        MOZ_ASSERT(env_.tables.length() == tables_.length());
        *tableIndex = env_.tables.length();

        uint32_t sigIndex;
        if (!newSig(Move(sig), &sigIndex))
            return false;

        MOZ_ASSERT(sigIndex >= env_.asmJSSigToTableIndex.length());
        if (!env_.asmJSSigToTableIndex.resize(sigIndex + 1))
            return false;

        env_.asmJSSigToTableIndex[sigIndex] = env_.tables.length();
        if (!env_.tables.emplaceBack(TableKind::TypedFunction, Limits(mask + 1)))
            return false;

        Global* global = validationLifo_.new_<Global>(Global::Table);
        if (!global)
            return false;

        global->u.tableIndex_ = *tableIndex;
        if (!globalMap_.putNew(name, global))
            return false;

        Table* t = validationLifo_.new_<Table>(sigIndex, name, firstUse, mask);
        return t && tables_.append(t);
    }
    bool defineFuncPtrTable(uint32_t tableIndex, Uint32Vector&& elems) {
        Table& table = *tables_[tableIndex];
        if (table.defined())
            return false;

        table.define();

        for (uint32_t& index : elems)
            index += funcImportMap_.count();

        return env_.elemSegments.emplaceBack(tableIndex, InitExpr(Val(uint32_t(0))), Move(elems));
    }
    bool declareImport(PropertyName* name, Sig&& sig, unsigned ffiIndex, uint32_t* importIndex) {
        FuncImportMap::AddPtr p = funcImportMap_.lookupForAdd(NamedSig::Lookup(name, sig));
        if (p) {
            *importIndex = p->value();
            return true;
        }

        *importIndex = funcImportMap_.count();
        MOZ_ASSERT(*importIndex == asmJSMetadata_->asmJSImports.length());

        if (*importIndex >= MaxImports)
            return failCurrentOffset("too many imports");

        if (!asmJSMetadata_->asmJSImports.emplaceBack(ffiIndex))
            return false;

        uint32_t sigIndex;
        if (!declareSig(Move(sig), &sigIndex))
            return false;

        return funcImportMap_.add(p, NamedSig(name, sigIndex, env_.sigs), *importIndex);
    }

    bool tryConstantAccess(uint64_t start, uint64_t width) {
        MOZ_ASSERT(UINT64_MAX - start > width);
        uint64_t len = start + width;
        if (len > uint64_t(INT32_MAX) + 1)
            return false;
        len = RoundUpToNextValidAsmJSHeapLength(len);
        if (len > env_.minMemoryLength)
            env_.minMemoryLength = len;
        return true;
    }

    // Error handling.
    bool hasAlreadyFailed() const {
        return !!errorString_;
    }

    bool failOffset(uint32_t offset, const char* str) {
        MOZ_ASSERT(!hasAlreadyFailed());
        MOZ_ASSERT(errorOffset_ == UINT32_MAX);
        MOZ_ASSERT(str);
        errorOffset_ = offset;
        errorString_ = DuplicateString(str);
        return false;
    }

    bool failCurrentOffset(const char* str) {
        return failOffset(tokenStream().anyCharsAccess().currentToken().pos.begin, str);
    }

    bool fail(ParseNode* pn, const char* str) {
        return failOffset(pn->pn_pos.begin, str);
    }

    bool failfVAOffset(uint32_t offset, const char* fmt, va_list ap) MOZ_FORMAT_PRINTF(3, 0) {
        MOZ_ASSERT(!hasAlreadyFailed());
        MOZ_ASSERT(errorOffset_ == UINT32_MAX);
        MOZ_ASSERT(fmt);
        errorOffset_ = offset;
        errorString_ = JS_vsmprintf(fmt, ap);
        return false;
    }

    bool failfOffset(uint32_t offset, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
        va_list ap;
        va_start(ap, fmt);
        failfVAOffset(offset, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failf(ParseNode* pn, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
        va_list ap;
        va_start(ap, fmt);
        failfVAOffset(pn->pn_pos.begin, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failNameOffset(uint32_t offset, const char* fmt, PropertyName* name) {
        // This function is invoked without the caller properly rooting its locals.
        gc::AutoSuppressGC suppress(cx_);
        JSAutoByteString bytes;
        if (AtomToPrintableString(cx_, name, &bytes))
            failfOffset(offset, fmt, bytes.ptr());
        return false;
    }

    bool failName(ParseNode* pn, const char* fmt, PropertyName* name) {
        return failNameOffset(pn->pn_pos.begin, fmt, name);
    }

    bool failOverRecursed() {
        errorOverRecursed_ = true;
        return false;
    }

    unsigned numArrayViews() const {
        return arrayViews_.length();
    }
    const ArrayView& arrayView(unsigned i) const {
        return arrayViews_[i];
    }
    unsigned numFuncDefs() const {
        return funcDefs_.length();
    }
    const Func& funcDef(unsigned i) const {
        return funcDefs_[i];
    }
    unsigned numFuncPtrTables() const {
        return tables_.length();
    }
    Table& table(unsigned i) const {
        return *tables_[i];
    }

    const Global* lookupGlobal(PropertyName* name) const {
        if (GlobalMap::Ptr p = globalMap_.lookup(name))
            return p->value();
        return nullptr;
    }

    Func* lookupFuncDef(PropertyName* name) {
        if (GlobalMap::Ptr p = globalMap_.lookup(name)) {
            Global* value = p->value();
            if (value->which() == Global::Function)
                return &funcDefs_[value->funcDefIndex()];
        }
        return nullptr;
    }

    bool lookupStandardLibraryMathName(PropertyName* name, MathBuiltin* mathBuiltin) const {
        if (MathNameMap::Ptr p = standardLibraryMathNames_.lookup(name)) {
            *mathBuiltin = p->value();
            return true;
        }
        return false;
    }
    bool lookupStandardLibraryAtomicsName(PropertyName* name, AsmJSAtomicsBuiltinFunction* atomicsBuiltin) const {
        if (AtomicsNameMap::Ptr p = standardLibraryAtomicsNames_.lookup(name)) {
            *atomicsBuiltin = p->value();
            return true;
        }
        return false;
    }
    bool lookupStandardSimdOpName(PropertyName* name, SimdOperation* op) const {
        if (SimdOperationNameMap::Ptr p = standardLibrarySimdOpNames_.lookup(name)) {
            *op = p->value();
            return true;
        }
        return false;
    }

    bool startFunctionBodies() {
        if (!arrayViews_.empty())
            env_.memoryUsage = atomicsPresent_ ? MemoryUsage::Shared : MemoryUsage::Unshared;
        else
            env_.memoryUsage = MemoryUsage::None;
        return true;
    }
    SharedModule finish() {
        MOZ_ASSERT(env_.funcSigs.empty());
        if (!env_.funcSigs.resize(funcImportMap_.count() + funcDefs_.length()))
            return nullptr;
        for (FuncImportMap::Range r = funcImportMap_.all(); !r.empty(); r.popFront()) {
            uint32_t funcIndex = r.front().value();
            MOZ_ASSERT(!env_.funcSigs[funcIndex]);
            env_.funcSigs[funcIndex] = &env_.sigs[r.front().key().sigIndex()];
        }
        for (const Func& func : funcDefs_) {
            uint32_t funcIndex = funcImportMap_.count() + func.funcDefIndex();
            MOZ_ASSERT(!env_.funcSigs[funcIndex]);
            env_.funcSigs[funcIndex] = &env_.sigs[func.sigIndex()];
        }

        if (!env_.funcImportGlobalDataOffsets.resize(funcImportMap_.count()))
            return nullptr;

        asmJSMetadata_->usesSimd = simdPresent_;

        MOZ_ASSERT(asmJSMetadata_->asmJSFuncNames.empty());
        if (!asmJSMetadata_->asmJSFuncNames.resize(funcImportMap_.count()))
            return nullptr;
        for (const Func& func : funcDefs_) {
            CacheableChars funcName = StringToNewUTF8CharsZ(cx_, *func.name());
            if (!funcName || !asmJSMetadata_->asmJSFuncNames.emplaceBack(Move(funcName)))
                return nullptr;
        }

        uint32_t endBeforeCurly = tokenStream().anyCharsAccess().currentToken().pos.end;
        asmJSMetadata_->srcLength = endBeforeCurly - asmJSMetadata_->srcStart;

        TokenPos pos;
        JS_ALWAYS_TRUE(tokenStream().peekTokenPos(&pos, TokenStreamShared::Operand));
        uint32_t endAfterCurly = pos.end;
        asmJSMetadata_->srcLengthWithRightBrace = endAfterCurly - asmJSMetadata_->srcStart;

        ScriptedCaller scriptedCaller;
        if (parser_.ss->filename()) {
            scriptedCaller.line = scriptedCaller.column = 0;  // unused
            scriptedCaller.filename = DuplicateString(parser_.ss->filename());
            if (!scriptedCaller.filename)
                return nullptr;
        }

        MutableCompileArgs args = cx_->new_<CompileArgs>();
        if (!args || !args->initFromContext(cx_, Move(scriptedCaller)))
            return nullptr;

        uint32_t codeSectionSize = 0;
        for (const Func& func : funcDefs_)
            codeSectionSize += func.bytes().length();

        env_.codeSection.emplace();
        env_.codeSection->start = 0;
        env_.codeSection->size = codeSectionSize;

        // asm.js does not have any wasm bytecode to save; view-source is
        // provided through the ScriptSource.
        SharedBytes bytes = cx_->new_<ShareableBytes>();
        if (!bytes)
            return nullptr;

        ModuleGenerator mg(*args, &env_, nullptr, nullptr);
        if (!mg.init(asmJSMetadata_.get()))
            return nullptr;

        for (Func& func : funcDefs_) {
            if (!mg.compileFuncDef(funcImportMap_.count() + func.funcDefIndex(), func.line(),
                                   func.bytes().begin(), func.bytes().end(),
                                   Move(func.callSiteLineNums()))) {
                return nullptr;
            }
        }

        if (!mg.finishFuncDefs())
            return nullptr;

        return mg.finishModule(*bytes);
    }
};

/*****************************************************************************/
// Numeric literal utilities

static bool
IsNumericNonFloatLiteral(ParseNode* pn)
{
    // Note: '-' is never rolled into the number; numbers are always positive
    // and negations must be applied manually.
    return pn->isKind(ParseNodeKind::Number) ||
           (pn->isKind(ParseNodeKind::Neg) && UnaryKid(pn)->isKind(ParseNodeKind::Number));
}

static bool
IsCallToGlobal(ModuleValidator& m, ParseNode* pn, const ModuleValidator::Global** global)
{
    if (!pn->isKind(ParseNodeKind::Call))
        return false;

    ParseNode* callee = CallCallee(pn);
    if (!callee->isKind(ParseNodeKind::Name))
        return false;

    *global = m.lookupGlobal(callee->name());
    return !!*global;
}

static bool
IsCoercionCall(ModuleValidator& m, ParseNode* pn, Type* coerceTo, ParseNode** coercedExpr)
{
    const ModuleValidator::Global* global;
    if (!IsCallToGlobal(m, pn, &global))
        return false;

    if (CallArgListLength(pn) != 1)
        return false;

    if (coercedExpr)
        *coercedExpr = CallArgList(pn);

    if (global->isMathFunction() && global->mathBuiltinFunction() == AsmJSMathBuiltin_fround) {
        *coerceTo = Type::Float;
        return true;
    }

    if (global->isSimdOperation() && global->simdOperation() == SimdOperation::Fn_check) {
        *coerceTo = global->simdOperationType();
        return true;
    }

    return false;
}

static bool
IsFloatLiteral(ModuleValidator& m, ParseNode* pn)
{
    ParseNode* coercedExpr;
    Type coerceTo;
    if (!IsCoercionCall(m, pn, &coerceTo, &coercedExpr))
        return false;
    // Don't fold into || to avoid clang/memcheck bug (bug 1077031).
    if (!coerceTo.isFloat())
        return false;
    return IsNumericNonFloatLiteral(coercedExpr);
}

static bool
IsSimdTuple(ModuleValidator& m, ParseNode* pn, SimdType* type)
{
    const ModuleValidator::Global* global;
    if (!IsCallToGlobal(m, pn, &global))
        return false;

    if (!global->isSimdCtor())
        return false;

    if (CallArgListLength(pn) != GetSimdLanes(global->simdCtorType()))
        return false;

    *type = global->simdCtorType();
    return true;
}

static bool
IsNumericLiteral(ModuleValidator& m, ParseNode* pn);

static NumLit
ExtractNumericLiteral(ModuleValidator& m, ParseNode* pn);

static inline bool
IsLiteralInt(ModuleValidator& m, ParseNode* pn, uint32_t* u32);

static bool
IsSimdLiteral(ModuleValidator& m, ParseNode* pn)
{
    SimdType type;
    if (!IsSimdTuple(m, pn, &type))
        return false;

    ParseNode* arg = CallArgList(pn);
    unsigned length = GetSimdLanes(type);
    for (unsigned i = 0; i < length; i++) {
        if (!IsNumericLiteral(m, arg))
            return false;

        uint32_t _;
        switch (type) {
          case SimdType::Int8x16:
          case SimdType::Int16x8:
          case SimdType::Int32x4:
          case SimdType::Uint8x16:
          case SimdType::Uint16x8:
          case SimdType::Uint32x4:
          case SimdType::Bool8x16:
          case SimdType::Bool16x8:
          case SimdType::Bool32x4:
            if (!IsLiteralInt(m, arg, &_))
                return false;
            break;
          case SimdType::Float32x4:
            if (!IsNumericNonFloatLiteral(arg))
                return false;
            break;
          default:
            MOZ_CRASH("unhandled simd type");
        }

        arg = NextNode(arg);
    }

    MOZ_ASSERT(arg == nullptr);
    return true;
}

static bool
IsNumericLiteral(ModuleValidator& m, ParseNode* pn)
{
    return IsNumericNonFloatLiteral(pn) || IsFloatLiteral(m, pn) || IsSimdLiteral(m, pn);
}

// The JS grammar treats -42 as -(42) (i.e., with separate grammar
// productions) for the unary - and literal 42). However, the asm.js spec
// recognizes -42 (modulo parens, so -(42) and -((42))) as a single literal
// so fold the two potential parse nodes into a single double value.
static double
ExtractNumericNonFloatValue(ParseNode* pn, ParseNode** out = nullptr)
{
    MOZ_ASSERT(IsNumericNonFloatLiteral(pn));

    if (pn->isKind(ParseNodeKind::Neg)) {
        pn = UnaryKid(pn);
        if (out)
            *out = pn;
        return -NumberNodeValue(pn);
    }

    return NumberNodeValue(pn);
}

static NumLit
ExtractSimdValue(ModuleValidator& m, ParseNode* pn)
{
    MOZ_ASSERT(IsSimdLiteral(m, pn));

    SimdType type = SimdType::Count;
    JS_ALWAYS_TRUE(IsSimdTuple(m, pn, &type));
    MOZ_ASSERT(CallArgListLength(pn) == GetSimdLanes(type));

    ParseNode* arg = CallArgList(pn);
    switch (type) {
      case SimdType::Int8x16:
      case SimdType::Uint8x16: {
        MOZ_ASSERT(GetSimdLanes(type) == 16);
        int8_t val[16];
        for (size_t i = 0; i < 16; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = int8_t(u32);
        }
        MOZ_ASSERT(arg == nullptr);
        NumLit::Which w = type == SimdType::Uint8x16 ? NumLit::Uint8x16 : NumLit::Int8x16;
        return NumLit(w, SimdConstant::CreateX16(val));
      }
      case SimdType::Int16x8:
      case SimdType::Uint16x8: {
        MOZ_ASSERT(GetSimdLanes(type) == 8);
        int16_t val[8];
        for (size_t i = 0; i < 8; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = int16_t(u32);
        }
        MOZ_ASSERT(arg == nullptr);
        NumLit::Which w = type == SimdType::Uint16x8 ? NumLit::Uint16x8 : NumLit::Int16x8;
        return NumLit(w, SimdConstant::CreateX8(val));
      }
      case SimdType::Int32x4:
      case SimdType::Uint32x4: {
        MOZ_ASSERT(GetSimdLanes(type) == 4);
        int32_t val[4];
        for (size_t i = 0; i < 4; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = int32_t(u32);
        }
        MOZ_ASSERT(arg == nullptr);
        NumLit::Which w = type == SimdType::Uint32x4 ? NumLit::Uint32x4 : NumLit::Int32x4;
        return NumLit(w, SimdConstant::CreateX4(val));
      }
      case SimdType::Float32x4: {
        MOZ_ASSERT(GetSimdLanes(type) == 4);
        float val[4];
        for (size_t i = 0; i < 4; i++, arg = NextNode(arg))
            val[i] = float(ExtractNumericNonFloatValue(arg));
        MOZ_ASSERT(arg == nullptr);
        return NumLit(NumLit::Float32x4, SimdConstant::CreateX4(val));
      }
      case SimdType::Bool8x16: {
        MOZ_ASSERT(GetSimdLanes(type) == 16);
        int8_t val[16];
        for (size_t i = 0; i < 16; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = u32 ? -1 : 0;
        }
        MOZ_ASSERT(arg == nullptr);
        return NumLit(NumLit::Bool8x16, SimdConstant::CreateX16(val));
      }
      case SimdType::Bool16x8: {
        MOZ_ASSERT(GetSimdLanes(type) == 8);
        int16_t val[8];
        for (size_t i = 0; i < 8; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = u32 ? -1 : 0;
        }
        MOZ_ASSERT(arg == nullptr);
        return NumLit(NumLit::Bool16x8, SimdConstant::CreateX8(val));
      }
      case SimdType::Bool32x4: {
        MOZ_ASSERT(GetSimdLanes(type) == 4);
        int32_t val[4];
        for (size_t i = 0; i < 4; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = u32 ? -1 : 0;
        }
        MOZ_ASSERT(arg == nullptr);
        return NumLit(NumLit::Bool32x4, SimdConstant::CreateX4(val));
      }
      default:
        break;
    }

    MOZ_CRASH("Unexpected SIMD type.");
}

static NumLit
ExtractNumericLiteral(ModuleValidator& m, ParseNode* pn)
{
    MOZ_ASSERT(IsNumericLiteral(m, pn));

    if (pn->isKind(ParseNodeKind::Call)) {
        // Float literals are explicitly coerced and thus the coerced literal may be
        // any valid (non-float) numeric literal.
        if (CallArgListLength(pn) == 1) {
            pn = CallArgList(pn);
            double d = ExtractNumericNonFloatValue(pn);
            return NumLit(NumLit::Float, DoubleValue(d));
        }

        return ExtractSimdValue(m, pn);
    }

    double d = ExtractNumericNonFloatValue(pn, &pn);

    // The asm.js spec syntactically distinguishes any literal containing a
    // decimal point or the literal -0 as having double type.
    if (NumberNodeHasFrac(pn) || IsNegativeZero(d))
        return NumLit(NumLit::Double, DoubleValue(d));

    // The syntactic checks above rule out these double values.
    MOZ_ASSERT(!IsNegativeZero(d));
    MOZ_ASSERT(!IsNaN(d));

    // Although doubles can only *precisely* represent 53-bit integers, they
    // can *imprecisely* represent integers much bigger than an int64_t.
    // Furthermore, d may be inf or -inf. In both cases, casting to an int64_t
    // is undefined, so test against the integer bounds using doubles.
    if (d < double(INT32_MIN) || d > double(UINT32_MAX))
        return NumLit(NumLit::OutOfRangeInt, UndefinedValue());

    // With the above syntactic and range limitations, d is definitely an
    // integer in the range [INT32_MIN, UINT32_MAX] range.
    int64_t i64 = int64_t(d);
    if (i64 >= 0) {
        if (i64 <= INT32_MAX)
            return NumLit(NumLit::Fixnum, Int32Value(i64));
        MOZ_ASSERT(i64 <= UINT32_MAX);
        return NumLit(NumLit::BigUnsigned, Int32Value(uint32_t(i64)));
    }
    MOZ_ASSERT(i64 >= INT32_MIN);
    return NumLit(NumLit::NegativeInt, Int32Value(i64));
}

static inline bool
IsLiteralInt(const NumLit& lit, uint32_t* u32)
{
    switch (lit.which()) {
      case NumLit::Fixnum:
      case NumLit::BigUnsigned:
      case NumLit::NegativeInt:
        *u32 = lit.toUint32();
        return true;
      case NumLit::Double:
      case NumLit::Float:
      case NumLit::OutOfRangeInt:
      case NumLit::Int8x16:
      case NumLit::Uint8x16:
      case NumLit::Int16x8:
      case NumLit::Uint16x8:
      case NumLit::Int32x4:
      case NumLit::Uint32x4:
      case NumLit::Float32x4:
      case NumLit::Bool8x16:
      case NumLit::Bool16x8:
      case NumLit::Bool32x4:
        return false;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad literal type");
}

static inline bool
IsLiteralInt(ModuleValidator& m, ParseNode* pn, uint32_t* u32)
{
    return IsNumericLiteral(m, pn) &&
           IsLiteralInt(ExtractNumericLiteral(m, pn), u32);
}

/*****************************************************************************/

namespace {

#define CASE(TYPE, OP) case SimdOperation::Fn_##OP: return MozOp::TYPE##OP;
#define I8x16CASE(OP) CASE(I8x16, OP)
#define I16x8CASE(OP) CASE(I16x8, OP)
#define I32x4CASE(OP) CASE(I32x4, OP)
#define F32x4CASE(OP) CASE(F32x4, OP)
#define B8x16CASE(OP) CASE(B8x16, OP)
#define B16x8CASE(OP) CASE(B16x8, OP)
#define B32x4CASE(OP) CASE(B32x4, OP)
#define ENUMERATE(TYPE, FOR_ALL, DO)                                     \
    switch(op) {                                                         \
        case SimdOperation::Constructor: return MozOp::TYPE##Constructor;\
        FOR_ALL(DO)                                                      \
        default: break;                                                  \
    }

static inline MozOp
SimdToOp(SimdType type, SimdOperation op)
{
    switch (type) {
      case SimdType::Uint8x16:
        // Handle the special unsigned opcodes, then fall through to Int8x16.
        switch (op) {
          case SimdOperation::Fn_addSaturate:        return MozOp::I8x16addSaturateU;
          case SimdOperation::Fn_subSaturate:        return MozOp::I8x16subSaturateU;
          case SimdOperation::Fn_extractLane:        return MozOp::I8x16extractLaneU;
          case SimdOperation::Fn_shiftRightByScalar: return MozOp::I8x16shiftRightByScalarU;
          case SimdOperation::Fn_lessThan:           return MozOp::I8x16lessThanU;
          case SimdOperation::Fn_lessThanOrEqual:    return MozOp::I8x16lessThanOrEqualU;
          case SimdOperation::Fn_greaterThan:        return MozOp::I8x16greaterThanU;
          case SimdOperation::Fn_greaterThanOrEqual: return MozOp::I8x16greaterThanOrEqualU;
          case SimdOperation::Fn_fromInt8x16Bits:    return MozOp::Limit;
          default: break;
        }
        MOZ_FALLTHROUGH;
      case SimdType::Int8x16:
        // Bitcasts Uint8x16 <--> Int8x16 become noops.
        switch (op) {
          case SimdOperation::Fn_fromUint8x16Bits: return MozOp::Limit;
          case SimdOperation::Fn_fromUint16x8Bits: return MozOp::I8x16fromInt16x8Bits;
          case SimdOperation::Fn_fromUint32x4Bits: return MozOp::I8x16fromInt32x4Bits;
          default: break;
        }
        ENUMERATE(I8x16, FORALL_INT8X16_ASMJS_OP, I8x16CASE)
        break;

      case SimdType::Uint16x8:
        // Handle the special unsigned opcodes, then fall through to Int16x8.
        switch(op) {
          case SimdOperation::Fn_addSaturate:        return MozOp::I16x8addSaturateU;
          case SimdOperation::Fn_subSaturate:        return MozOp::I16x8subSaturateU;
          case SimdOperation::Fn_extractLane:        return MozOp::I16x8extractLaneU;
          case SimdOperation::Fn_shiftRightByScalar: return MozOp::I16x8shiftRightByScalarU;
          case SimdOperation::Fn_lessThan:           return MozOp::I16x8lessThanU;
          case SimdOperation::Fn_lessThanOrEqual:    return MozOp::I16x8lessThanOrEqualU;
          case SimdOperation::Fn_greaterThan:        return MozOp::I16x8greaterThanU;
          case SimdOperation::Fn_greaterThanOrEqual: return MozOp::I16x8greaterThanOrEqualU;
          case SimdOperation::Fn_fromInt16x8Bits:    return MozOp::Limit;
          default: break;
        }
        MOZ_FALLTHROUGH;
      case SimdType::Int16x8:
        // Bitcasts Uint16x8 <--> Int16x8 become noops.
        switch (op) {
          case SimdOperation::Fn_fromUint8x16Bits: return MozOp::I16x8fromInt8x16Bits;
          case SimdOperation::Fn_fromUint16x8Bits: return MozOp::Limit;
          case SimdOperation::Fn_fromUint32x4Bits: return MozOp::I16x8fromInt32x4Bits;
          default: break;
        }
        ENUMERATE(I16x8, FORALL_INT16X8_ASMJS_OP, I16x8CASE)
        break;

      case SimdType::Uint32x4:
        // Handle the special unsigned opcodes, then fall through to Int32x4.
        switch(op) {
          case SimdOperation::Fn_shiftRightByScalar: return MozOp::I32x4shiftRightByScalarU;
          case SimdOperation::Fn_lessThan:           return MozOp::I32x4lessThanU;
          case SimdOperation::Fn_lessThanOrEqual:    return MozOp::I32x4lessThanOrEqualU;
          case SimdOperation::Fn_greaterThan:        return MozOp::I32x4greaterThanU;
          case SimdOperation::Fn_greaterThanOrEqual: return MozOp::I32x4greaterThanOrEqualU;
          case SimdOperation::Fn_fromFloat32x4:      return MozOp::I32x4fromFloat32x4U;
          case SimdOperation::Fn_fromInt32x4Bits:    return MozOp::Limit;
          default: break;
        }
        MOZ_FALLTHROUGH;
      case SimdType::Int32x4:
        // Bitcasts Uint32x4 <--> Int32x4 become noops.
        switch (op) {
          case SimdOperation::Fn_fromUint8x16Bits: return MozOp::I32x4fromInt8x16Bits;
          case SimdOperation::Fn_fromUint16x8Bits: return MozOp::I32x4fromInt16x8Bits;
          case SimdOperation::Fn_fromUint32x4Bits: return MozOp::Limit;
          default: break;
        }
        ENUMERATE(I32x4, FORALL_INT32X4_ASMJS_OP, I32x4CASE)
        break;

      case SimdType::Float32x4:
        switch (op) {
          case SimdOperation::Fn_fromUint8x16Bits: return MozOp::F32x4fromInt8x16Bits;
          case SimdOperation::Fn_fromUint16x8Bits: return MozOp::F32x4fromInt16x8Bits;
          case SimdOperation::Fn_fromUint32x4Bits: return MozOp::F32x4fromInt32x4Bits;
          default: break;
        }
        ENUMERATE(F32x4, FORALL_FLOAT32X4_ASMJS_OP, F32x4CASE)
        break;

      case SimdType::Bool8x16:
        ENUMERATE(B8x16, FORALL_BOOL_SIMD_OP, B8x16CASE)
        break;

      case SimdType::Bool16x8:
        ENUMERATE(B16x8, FORALL_BOOL_SIMD_OP, B16x8CASE)
        break;

      case SimdType::Bool32x4:
        ENUMERATE(B32x4, FORALL_BOOL_SIMD_OP, B32x4CASE)
        break;

      default: break;
    }
    MOZ_CRASH("unexpected SIMD (type, operator) combination");
}

#undef CASE
#undef I8x16CASE
#undef I16x8CASE
#undef I32x4CASE
#undef F32x4CASE
#undef B8x16CASE
#undef B16x8CASE
#undef B32x4CASE
#undef ENUMERATE

typedef Vector<PropertyName*, 4, SystemAllocPolicy> NameVector;

// Encapsulates the building of an asm bytecode function from an asm.js function
// source code, packing the asm.js code into the asm bytecode form that can
// be decoded and compiled with a FunctionCompiler.
class MOZ_STACK_CLASS FunctionValidator
{
  public:
    struct Local
    {
        Type type;
        unsigned slot;
        Local(Type t, unsigned slot) : type(t), slot(slot) {
            MOZ_ASSERT(type.isCanonicalValType());
        }
    };

  private:
    typedef HashMap<PropertyName*, Local> LocalMap;
    typedef HashMap<PropertyName*, uint32_t> LabelMap;

    ModuleValidator&  m_;
    ParseNode*        fn_;
    Bytes             bytes_;
    Encoder           encoder_;
    Uint32Vector      callSiteLineNums_;
    LocalMap          locals_;

    // Labels
    LabelMap          breakLabels_;
    LabelMap          continueLabels_;
    Uint32Vector      breakableStack_;
    Uint32Vector      continuableStack_;
    uint32_t          blockDepth_;

    bool              hasAlreadyReturned_;
    ExprType          ret_;

  public:
    FunctionValidator(ModuleValidator& m, ParseNode* fn)
      : m_(m),
        fn_(fn),
        encoder_(bytes_),
        locals_(m.cx()),
        breakLabels_(m.cx()),
        continueLabels_(m.cx()),
        blockDepth_(0),
        hasAlreadyReturned_(false),
        ret_(ExprType::Limit)
    {}

    ModuleValidator& m() const        { return m_; }
    JSContext* cx() const             { return m_.cx(); }
    ParseNode* fn() const             { return fn_; }

    bool init() {
        return locals_.init() &&
               breakLabels_.init() &&
               continueLabels_.init();
    }

    void define(ModuleValidator::Func* func, unsigned line) {
        MOZ_ASSERT(!blockDepth_);
        MOZ_ASSERT(breakableStack_.empty());
        MOZ_ASSERT(continuableStack_.empty());
        MOZ_ASSERT(breakLabels_.empty());
        MOZ_ASSERT(continueLabels_.empty());
        func->define(fn_, line, Move(bytes_), Move(callSiteLineNums_));
    }

    bool fail(ParseNode* pn, const char* str) {
        return m_.fail(pn, str);
    }

    bool failf(ParseNode* pn, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
        va_list ap;
        va_start(ap, fmt);
        m_.failfVAOffset(pn->pn_pos.begin, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failName(ParseNode* pn, const char* fmt, PropertyName* name) {
        return m_.failName(pn, fmt, name);
    }

    /***************************************************** Local scope setup */

    bool addLocal(ParseNode* pn, PropertyName* name, Type type) {
        LocalMap::AddPtr p = locals_.lookupForAdd(name);
        if (p)
            return failName(pn, "duplicate local name '%s' not allowed", name);
        return locals_.add(p, name, Local(type, locals_.count()));
    }

    /****************************** For consistency of returns in a function */

    bool hasAlreadyReturned() const {
        return hasAlreadyReturned_;
    }

    ExprType returnedType() const {
        return ret_;
    }

    void setReturnedType(ExprType ret) {
        ret_ = ret;
        hasAlreadyReturned_ = true;
    }

    /**************************************************************** Labels */
  private:
    bool writeBr(uint32_t absolute, Op op = Op::Br) {
        MOZ_ASSERT(op == Op::Br || op == Op::BrIf);
        MOZ_ASSERT(absolute < blockDepth_);
        return encoder().writeOp(op) &&
               encoder().writeVarU32(blockDepth_ - 1 - absolute);
    }
    void removeLabel(PropertyName* label, LabelMap* map) {
        LabelMap::Ptr p = map->lookup(label);
        MOZ_ASSERT(p);
        map->remove(p);
    }

  public:
    bool pushBreakableBlock() {
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               breakableStack_.append(blockDepth_++);
    }
    bool popBreakableBlock() {
        JS_ALWAYS_TRUE(breakableStack_.popCopy() == --blockDepth_);
        return encoder().writeOp(Op::End);
    }

    bool pushUnbreakableBlock(const NameVector* labels = nullptr) {
        if (labels) {
            for (PropertyName* label : *labels) {
                if (!breakLabels_.putNew(label, blockDepth_))
                    return false;
            }
        }
        blockDepth_++;
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void));
    }
    bool popUnbreakableBlock(const NameVector* labels = nullptr) {
        if (labels) {
            for (PropertyName* label : *labels)
                removeLabel(label, &breakLabels_);
        }
        --blockDepth_;
        return encoder().writeOp(Op::End);
    }

    bool pushContinuableBlock() {
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               continuableStack_.append(blockDepth_++);
    }
    bool popContinuableBlock() {
        JS_ALWAYS_TRUE(continuableStack_.popCopy() == --blockDepth_);
        return encoder().writeOp(Op::End);
    }

    bool pushLoop() {
        return encoder().writeOp(Op::Block) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               encoder().writeOp(Op::Loop) &&
               encoder().writeFixedU8(uint8_t(ExprType::Void)) &&
               breakableStack_.append(blockDepth_++) &&
               continuableStack_.append(blockDepth_++);
    }
    bool popLoop() {
        JS_ALWAYS_TRUE(continuableStack_.popCopy() == --blockDepth_);
        JS_ALWAYS_TRUE(breakableStack_.popCopy() == --blockDepth_);
        return encoder().writeOp(Op::End) &&
               encoder().writeOp(Op::End);
    }

    bool pushIf(size_t* typeAt) {
        ++blockDepth_;
        return encoder().writeOp(Op::If) &&
               encoder().writePatchableFixedU7(typeAt);
    }
    bool switchToElse() {
        MOZ_ASSERT(blockDepth_ > 0);
        return encoder().writeOp(Op::Else);
    }
    void setIfType(size_t typeAt, ExprType type) {
        encoder().patchFixedU7(typeAt, uint8_t(type));
    }
    bool popIf() {
        MOZ_ASSERT(blockDepth_ > 0);
        --blockDepth_;
        return encoder().writeOp(Op::End);
    }
    bool popIf(size_t typeAt, ExprType type) {
        MOZ_ASSERT(blockDepth_ > 0);
        --blockDepth_;
        if (!encoder().writeOp(Op::End))
            return false;

        setIfType(typeAt, type);
        return true;
    }

    bool writeBreakIf() {
        return writeBr(breakableStack_.back(), Op::BrIf);
    }
    bool writeContinueIf() {
        return writeBr(continuableStack_.back(), Op::BrIf);
    }
    bool writeUnlabeledBreakOrContinue(bool isBreak) {
        return writeBr(isBreak? breakableStack_.back() : continuableStack_.back());
    }
    bool writeContinue() {
        return writeBr(continuableStack_.back());
    }

    bool addLabels(const NameVector& labels, uint32_t relativeBreakDepth,
                   uint32_t relativeContinueDepth)
    {
        for (PropertyName* label : labels) {
            if (!breakLabels_.putNew(label, blockDepth_ + relativeBreakDepth))
                return false;
            if (!continueLabels_.putNew(label, blockDepth_ + relativeContinueDepth))
                return false;
        }
        return true;
    }
    void removeLabels(const NameVector& labels) {
        for (PropertyName* label : labels) {
            removeLabel(label, &breakLabels_);
            removeLabel(label, &continueLabels_);
        }
    }
    bool writeLabeledBreakOrContinue(PropertyName* label, bool isBreak) {
        LabelMap& map = isBreak ? breakLabels_ : continueLabels_;
        if (LabelMap::Ptr p = map.lookup(label))
            return writeBr(p->value());
        MOZ_CRASH("nonexistent label");
    }

    /*************************************************** Read-only interface */

    const Local* lookupLocal(PropertyName* name) const {
        if (auto p = locals_.lookup(name))
            return &p->value();
        return nullptr;
    }

    const ModuleValidator::Global* lookupGlobal(PropertyName* name) const {
        if (locals_.has(name))
            return nullptr;
        return m_.lookupGlobal(name);
    }

    size_t numLocals() const { return locals_.count(); }

    /**************************************************** Encoding interface */

    Encoder& encoder() { return encoder_; }

    MOZ_MUST_USE bool writeInt32Lit(int32_t i32) {
        return encoder().writeOp(Op::I32Const) &&
               encoder().writeVarS32(i32);
    }
    MOZ_MUST_USE bool writeConstExpr(const NumLit& lit) {
        switch (lit.which()) {
          case NumLit::Fixnum:
          case NumLit::NegativeInt:
          case NumLit::BigUnsigned:
            return writeInt32Lit(lit.toInt32());
          case NumLit::Float:
            return encoder().writeOp(Op::F32Const) &&
                   encoder().writeFixedF32(lit.toFloat());
          case NumLit::Double:
            return encoder().writeOp(Op::F64Const) &&
                   encoder().writeFixedF64(lit.toDouble());
          case NumLit::Int8x16:
          case NumLit::Uint8x16:
            return encoder().writeOp(MozOp::I8x16Const) &&
                   encoder().writeFixedI8x16(lit.simdValue().asInt8x16());
          case NumLit::Int16x8:
          case NumLit::Uint16x8:
            return encoder().writeOp(MozOp::I16x8Const) &&
                   encoder().writeFixedI16x8(lit.simdValue().asInt16x8());
          case NumLit::Int32x4:
          case NumLit::Uint32x4:
            return encoder().writeOp(MozOp::I32x4Const) &&
                   encoder().writeFixedI32x4(lit.simdValue().asInt32x4());
          case NumLit::Float32x4:
            return encoder().writeOp(MozOp::F32x4Const) &&
                   encoder().writeFixedF32x4(lit.simdValue().asFloat32x4());
          case NumLit::Bool8x16:
            // Boolean vectors use the Int8x16 memory representation.
            return encoder().writeOp(MozOp::B8x16Const) &&
                   encoder().writeFixedI8x16(lit.simdValue().asInt8x16());
          case NumLit::Bool16x8:
            // Boolean vectors use the Int16x8 memory representation.
            return encoder().writeOp(MozOp::B16x8Const) &&
                   encoder().writeFixedI16x8(lit.simdValue().asInt16x8());
          case NumLit::Bool32x4:
            // Boolean vectors use the Int32x4 memory representation.
            return encoder().writeOp(MozOp::B32x4Const) &&
                   encoder().writeFixedI32x4(lit.simdValue().asInt32x4());
          case NumLit::OutOfRangeInt:
            break;
        }
        MOZ_CRASH("unexpected literal type");
    }
    MOZ_MUST_USE bool writeCall(ParseNode* pn, Op op) {
        if (!encoder().writeOp(op))
            return false;

        TokenStreamAnyChars& anyChars = m().tokenStream().anyCharsAccess();
        return callSiteLineNums_.append(anyChars.srcCoords.lineNum(pn->pn_pos.begin));
    }
    MOZ_MUST_USE bool writeCall(ParseNode* pn, MozOp op) {
        if (!encoder().writeOp(op))
            return false;

        TokenStreamAnyChars& anyChars = m().tokenStream().anyCharsAccess();
        return callSiteLineNums_.append(anyChars.srcCoords.lineNum(pn->pn_pos.begin));
    }
    MOZ_MUST_USE bool prepareCall(ParseNode* pn) {
        TokenStreamAnyChars& anyChars = m().tokenStream().anyCharsAccess();
        return callSiteLineNums_.append(anyChars.srcCoords.lineNum(pn->pn_pos.begin));
    }
    MOZ_MUST_USE bool writeSimdOp(SimdType simdType, SimdOperation simdOp) {
        MozOp op = SimdToOp(simdType, simdOp);
        if (op == MozOp::Limit)
            return true;
        return encoder().writeOp(op);
    }
};

} /* anonymous namespace */

/*****************************************************************************/
// asm.js type-checking and code-generation algorithm

static bool
CheckIdentifier(ModuleValidator& m, ParseNode* usepn, PropertyName* name)
{
    if (name == m.cx()->names().arguments || name == m.cx()->names().eval)
        return m.failName(usepn, "'%s' is not an allowed identifier", name);
    return true;
}

static bool
CheckModuleLevelName(ModuleValidator& m, ParseNode* usepn, PropertyName* name)
{
    if (!CheckIdentifier(m, usepn, name))
        return false;

    if (name == m.moduleFunctionName() ||
        name == m.globalArgumentName() ||
        name == m.importArgumentName() ||
        name == m.bufferArgumentName() ||
        m.lookupGlobal(name))
    {
        return m.failName(usepn, "duplicate name '%s' not allowed", name);
    }

    return true;
}

static bool
CheckFunctionHead(ModuleValidator& m, ParseNode* fn)
{
    if (fn->pn_funbox->hasRest())
        return m.fail(fn, "rest args not allowed");
    if (fn->pn_funbox->isExprBody())
        return m.fail(fn, "expression closures not allowed");
    if (fn->pn_funbox->hasDestructuringArgs)
        return m.fail(fn, "destructuring args not allowed");
    return true;
}

static bool
CheckArgument(ModuleValidator& m, ParseNode* arg, PropertyName** name)
{
    *name = nullptr;

    if (!arg->isKind(ParseNodeKind::Name))
        return m.fail(arg, "argument is not a plain name");

    if (!CheckIdentifier(m, arg, arg->name()))
        return false;

    *name = arg->name();
    return true;
}

static bool
CheckModuleArgument(ModuleValidator& m, ParseNode* arg, PropertyName** name)
{
    if (!CheckArgument(m, arg, name))
        return false;

    if (!CheckModuleLevelName(m, arg, *name))
        return false;

    return true;
}

static bool
CheckModuleArguments(ModuleValidator& m, ParseNode* fn)
{
    unsigned numFormals;
    ParseNode* arg1 = FunctionFormalParametersList(fn, &numFormals);
    ParseNode* arg2 = arg1 ? NextNode(arg1) : nullptr;
    ParseNode* arg3 = arg2 ? NextNode(arg2) : nullptr;

    if (numFormals > 3)
        return m.fail(fn, "asm.js modules takes at most 3 argument");

    PropertyName* arg1Name = nullptr;
    if (arg1 && !CheckModuleArgument(m, arg1, &arg1Name))
        return false;
    if (!m.initGlobalArgumentName(arg1Name))
        return false;

    PropertyName* arg2Name = nullptr;
    if (arg2 && !CheckModuleArgument(m, arg2, &arg2Name))
        return false;
    if (!m.initImportArgumentName(arg2Name))
        return false;

    PropertyName* arg3Name = nullptr;
    if (arg3 && !CheckModuleArgument(m, arg3, &arg3Name))
        return false;
    if (!m.initBufferArgumentName(arg3Name))
        return false;

    return true;
}

static bool
CheckPrecedingStatements(ModuleValidator& m, ParseNode* stmtList)
{
    MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));

    ParseNode* stmt = ListHead(stmtList);
    for (unsigned i = 0, n = ListLength(stmtList); i < n; i++) {
        if (!IsIgnoredDirective(m.cx(), stmt))
            return m.fail(stmt, "invalid asm.js statement");
    }

    return true;
}

static bool
CheckGlobalVariableInitConstant(ModuleValidator& m, PropertyName* varName, ParseNode* initNode,
                                bool isConst)
{
    NumLit lit = ExtractNumericLiteral(m, initNode);
    if (!lit.valid())
        return m.fail(initNode, "global initializer is out of representable integer range");

    Type canonicalType = Type::canonicalize(Type::lit(lit));
    if (!canonicalType.isGlobalVarType())
        return m.fail(initNode, "global variable type not allowed");

    return m.addGlobalVarInit(varName, lit, canonicalType, isConst);
}

static bool
CheckTypeAnnotation(ModuleValidator& m, ParseNode* coercionNode, Type* coerceTo,
                    ParseNode** coercedExpr = nullptr)
{
    switch (coercionNode->getKind()) {
      case ParseNodeKind::BitOr: {
        ParseNode* rhs = BitwiseRight(coercionNode);
        uint32_t i;
        if (!IsLiteralInt(m, rhs, &i) || i != 0)
            return m.fail(rhs, "must use |0 for argument/return coercion");
        *coerceTo = Type::Int;
        if (coercedExpr)
            *coercedExpr = BitwiseLeft(coercionNode);
        return true;
      }
      case ParseNodeKind::Pos: {
        *coerceTo = Type::Double;
        if (coercedExpr)
            *coercedExpr = UnaryKid(coercionNode);
        return true;
      }
      case ParseNodeKind::Call: {
        if (IsCoercionCall(m, coercionNode, coerceTo, coercedExpr))
            return true;
        break;
      }
      default:;
    }

    return m.fail(coercionNode, "must be of the form +x, x|0, fround(x), or a SIMD check(x)");
}

static bool
CheckGlobalVariableInitImport(ModuleValidator& m, PropertyName* varName, ParseNode* initNode,
                              bool isConst)
{
    Type coerceTo;
    ParseNode* coercedExpr;
    if (!CheckTypeAnnotation(m, initNode, &coerceTo, &coercedExpr))
        return false;

    if (!coercedExpr->isKind(ParseNodeKind::Dot))
        return m.failName(coercedExpr, "invalid import expression for global '%s'", varName);

    if (!coerceTo.isGlobalVarType())
        return m.fail(initNode, "global variable type not allowed");

    ParseNode* base = DotBase(coercedExpr);
    PropertyName* field = DotMember(coercedExpr);

    PropertyName* importName = m.importArgumentName();
    if (!importName)
        return m.fail(coercedExpr, "cannot import without an asm.js foreign parameter");
    if (!IsUseOfName(base, importName))
        return m.failName(coercedExpr, "base of import expression must be '%s'", importName);

    return m.addGlobalVarImport(varName, field, coerceTo, isConst);
}

static bool
IsArrayViewCtorName(ModuleValidator& m, PropertyName* name, Scalar::Type* type)
{
    JSAtomState& names = m.cx()->names();
    if (name == names.Int8Array) {
        *type = Scalar::Int8;
    } else if (name == names.Uint8Array) {
        *type = Scalar::Uint8;
    } else if (name == names.Int16Array) {
        *type = Scalar::Int16;
    } else if (name == names.Uint16Array) {
        *type = Scalar::Uint16;
    } else if (name == names.Int32Array) {
        *type = Scalar::Int32;
    } else if (name == names.Uint32Array) {
        *type = Scalar::Uint32;
    } else if (name == names.Float32Array) {
        *type = Scalar::Float32;
    } else if (name == names.Float64Array) {
        *type = Scalar::Float64;
    } else {
        return false;
    }
    return true;
}

static bool
CheckNewArrayViewArgs(ModuleValidator& m, ParseNode* ctorExpr, PropertyName* bufferName)
{
    ParseNode* bufArg = NextNode(ctorExpr);
    if (!bufArg || NextNode(bufArg) != nullptr)
        return m.fail(ctorExpr, "array view constructor takes exactly one argument");

    if (!IsUseOfName(bufArg, bufferName))
        return m.failName(bufArg, "argument to array view constructor must be '%s'", bufferName);

    return true;
}

static bool
CheckNewArrayView(ModuleValidator& m, PropertyName* varName, ParseNode* newExpr)
{
    PropertyName* globalName = m.globalArgumentName();
    if (!globalName)
        return m.fail(newExpr, "cannot create array view without an asm.js global parameter");

    PropertyName* bufferName = m.bufferArgumentName();
    if (!bufferName)
        return m.fail(newExpr, "cannot create array view without an asm.js heap parameter");

    ParseNode* ctorExpr = ListHead(newExpr);

    PropertyName* field;
    Scalar::Type type;
    if (ctorExpr->isKind(ParseNodeKind::Dot)) {
        ParseNode* base = DotBase(ctorExpr);

        if (!IsUseOfName(base, globalName))
            return m.failName(base, "expecting '%s.*Array", globalName);

        field = DotMember(ctorExpr);
        if (!IsArrayViewCtorName(m, field, &type))
            return m.fail(ctorExpr, "could not match typed array name");
    } else {
        if (!ctorExpr->isKind(ParseNodeKind::Name))
            return m.fail(ctorExpr, "expecting name of imported array view constructor");

        PropertyName* globalName = ctorExpr->name();
        const ModuleValidator::Global* global = m.lookupGlobal(globalName);
        if (!global)
            return m.failName(ctorExpr, "%s not found in module global scope", globalName);

        if (global->which() != ModuleValidator::Global::ArrayViewCtor)
            return m.failName(ctorExpr, "%s must be an imported array view constructor", globalName);

        field = nullptr;
        type = global->viewType();
    }

    if (!CheckNewArrayViewArgs(m, ctorExpr, bufferName))
        return false;

    return m.addArrayView(varName, type, field);
}

static bool
IsSimdValidOperationType(SimdType type, SimdOperation op)
{
#define CASE(op) case SimdOperation::Fn_##op:
    switch(type) {
      case SimdType::Int8x16:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromUint8x16Bits:
          case SimdOperation::Fn_fromUint16x8Bits:
          case SimdOperation::Fn_fromUint32x4Bits:
          FORALL_INT8X16_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Int16x8:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromUint8x16Bits:
          case SimdOperation::Fn_fromUint16x8Bits:
          case SimdOperation::Fn_fromUint32x4Bits:
          FORALL_INT16X8_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Int32x4:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromUint8x16Bits:
          case SimdOperation::Fn_fromUint16x8Bits:
          case SimdOperation::Fn_fromUint32x4Bits:
          FORALL_INT32X4_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Uint8x16:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromInt8x16Bits:
          case SimdOperation::Fn_fromUint16x8Bits:
          case SimdOperation::Fn_fromUint32x4Bits:
          FORALL_INT8X16_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Uint16x8:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromUint8x16Bits:
          case SimdOperation::Fn_fromInt16x8Bits:
          case SimdOperation::Fn_fromUint32x4Bits:
          FORALL_INT16X8_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Uint32x4:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromUint8x16Bits:
          case SimdOperation::Fn_fromUint16x8Bits:
          case SimdOperation::Fn_fromInt32x4Bits:
          FORALL_INT32X4_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Float32x4:
        switch (op) {
          case SimdOperation::Constructor:
          case SimdOperation::Fn_fromUint8x16Bits:
          case SimdOperation::Fn_fromUint16x8Bits:
          case SimdOperation::Fn_fromUint32x4Bits:
          FORALL_FLOAT32X4_ASMJS_OP(CASE) return true;
          default: return false;
        }
        break;
      case SimdType::Bool8x16:
      case SimdType::Bool16x8:
      case SimdType::Bool32x4:
        switch (op) {
          case SimdOperation::Constructor:
          FORALL_BOOL_SIMD_OP(CASE) return true;
          default: return false;
        }
        break;
      default:
        // Unimplemented SIMD type.
        return false;
    }
#undef CASE
}

static bool
CheckGlobalMathImport(ModuleValidator& m, ParseNode* initNode, PropertyName* varName,
                      PropertyName* field)
{
    // Math builtin, with the form glob.Math.[[builtin]]
    ModuleValidator::MathBuiltin mathBuiltin;
    if (!m.lookupStandardLibraryMathName(field, &mathBuiltin))
        return m.failName(initNode, "'%s' is not a standard Math builtin", field);

    switch (mathBuiltin.kind) {
      case ModuleValidator::MathBuiltin::Function:
        return m.addMathBuiltinFunction(varName, mathBuiltin.u.func, field);
      case ModuleValidator::MathBuiltin::Constant:
        return m.addMathBuiltinConstant(varName, mathBuiltin.u.cst, field);
      default:
        break;
    }
    MOZ_CRASH("unexpected or uninitialized math builtin type");
}

static bool
CheckGlobalAtomicsImport(ModuleValidator& m, ParseNode* initNode, PropertyName* varName,
                         PropertyName* field)
{
    // Atomics builtin, with the form glob.Atomics.[[builtin]]
    AsmJSAtomicsBuiltinFunction func;
    if (!m.lookupStandardLibraryAtomicsName(field, &func))
        return m.failName(initNode, "'%s' is not a standard Atomics builtin", field);

    return m.addAtomicsBuiltinFunction(varName, func, field);
}

static bool
CheckGlobalSimdImport(ModuleValidator& m, ParseNode* initNode, PropertyName* varName,
                      PropertyName* field)
{
    if (!m.supportsSimd())
        return m.fail(initNode, "SIMD is not supported on this platform");

    // SIMD constructor, with the form glob.SIMD.[[type]]
    SimdType simdType;
    if (!IsSimdTypeName(m.cx()->names(), field, &simdType))
        return m.failName(initNode, "'%s' is not a standard SIMD type", field);

    // IsSimdTypeName will return true for any SIMD type supported by the VM.
    //
    // Since we may not support all of those SIMD types in asm.js, use the
    // asm.js-specific IsSimdValidOperationType() to check if this specific
    // constructor is supported in asm.js.
    if (!IsSimdValidOperationType(simdType, SimdOperation::Constructor))
        return m.failName(initNode, "'%s' is not a supported SIMD type", field);

    return m.addSimdCtor(varName, simdType, field);
}

static bool
CheckGlobalSimdOperationImport(ModuleValidator& m, const ModuleValidator::Global* global,
                               ParseNode* initNode, PropertyName* varName, PropertyName* opName)
{
    SimdType simdType = global->simdCtorType();
    SimdOperation simdOp;
    if (!m.lookupStandardSimdOpName(opName, &simdOp))
        return m.failName(initNode, "'%s' is not a standard SIMD operation", opName);
    if (!IsSimdValidOperationType(simdType, simdOp))
        return m.failName(initNode, "'%s' is not an operation supported by the SIMD type", opName);
    return m.addSimdOperation(varName, simdType, simdOp, opName);
}

static bool
CheckGlobalDotImport(ModuleValidator& m, PropertyName* varName, ParseNode* initNode)
{
    ParseNode* base = DotBase(initNode);
    PropertyName* field = DotMember(initNode);

    if (base->isKind(ParseNodeKind::Dot)) {
        ParseNode* global = DotBase(base);
        PropertyName* mathOrAtomicsOrSimd = DotMember(base);

        PropertyName* globalName = m.globalArgumentName();
        if (!globalName)
            return m.fail(base, "import statement requires the module have a stdlib parameter");

        if (!IsUseOfName(global, globalName)) {
            if (global->isKind(ParseNodeKind::Dot)) {
                return m.failName(base, "imports can have at most two dot accesses "
                                        "(e.g. %s.Math.sin)", globalName);
            }
            return m.failName(base, "expecting %s.*", globalName);
        }

        if (mathOrAtomicsOrSimd == m.cx()->names().Math)
            return CheckGlobalMathImport(m, initNode, varName, field);
        if (mathOrAtomicsOrSimd == m.cx()->names().Atomics)
            return CheckGlobalAtomicsImport(m, initNode, varName, field);
        if (mathOrAtomicsOrSimd == m.cx()->names().SIMD)
            return CheckGlobalSimdImport(m, initNode, varName, field);
        return m.failName(base, "expecting %s.{Math|SIMD}", globalName);
    }

    if (!base->isKind(ParseNodeKind::Name))
        return m.fail(base, "expected name of variable or parameter");

    if (base->name() == m.globalArgumentName()) {
        if (field == m.cx()->names().NaN)
            return m.addGlobalConstant(varName, GenericNaN(), field);
        if (field == m.cx()->names().Infinity)
            return m.addGlobalConstant(varName, PositiveInfinity<double>(), field);

        Scalar::Type type;
        if (IsArrayViewCtorName(m, field, &type))
            return m.addArrayViewCtor(varName, type, field);

        return m.failName(initNode, "'%s' is not a standard constant or typed array name", field);
    }

    if (base->name() == m.importArgumentName())
        return m.addFFI(varName, field);

    const ModuleValidator::Global* global = m.lookupGlobal(base->name());
    if (!global)
        return m.failName(initNode, "%s not found in module global scope", base->name());

    if (!global->isSimdCtor())
        return m.failName(base, "expecting SIMD constructor name, got %s", field);

    return CheckGlobalSimdOperationImport(m, global, initNode, varName, field);
}

static bool
CheckModuleGlobal(ModuleValidator& m, ParseNode* var, bool isConst)
{
    if (!var->isKind(ParseNodeKind::Name))
        return m.fail(var, "import variable is not a plain name");

    if (!CheckModuleLevelName(m, var, var->name()))
        return false;

    ParseNode* initNode = MaybeInitializer(var);
    if (!initNode)
        return m.fail(var, "module import needs initializer");

    if (IsNumericLiteral(m, initNode))
        return CheckGlobalVariableInitConstant(m, var->name(), initNode, isConst);

    if (initNode->isKind(ParseNodeKind::BitOr) ||
        initNode->isKind(ParseNodeKind::Pos) ||
        initNode->isKind(ParseNodeKind::Call))
    {
        return CheckGlobalVariableInitImport(m, var->name(), initNode, isConst);
    }

    if (initNode->isKind(ParseNodeKind::New))
        return CheckNewArrayView(m, var->name(), initNode);

    if (initNode->isKind(ParseNodeKind::Dot))
        return CheckGlobalDotImport(m, var->name(), initNode);

    return m.fail(initNode, "unsupported import expression");
}

static bool
CheckModuleProcessingDirectives(ModuleValidator& m)
{
    auto& ts = m.parser().tokenStream;
    while (true) {
        bool matched;
        if (!ts.matchToken(&matched, TokenKind::String, TokenStreamShared::Operand))
            return false;
        if (!matched)
            return true;

        if (!IsIgnoredDirectiveName(m.cx(), ts.anyCharsAccess().currentToken().atom()))
            return m.failCurrentOffset("unsupported processing directive");

        TokenKind tt;
        if (!ts.getToken(&tt))
            return false;
        if (tt != TokenKind::Semi)
            return m.failCurrentOffset("expected semicolon after string literal");
    }
}

static bool
CheckModuleGlobals(ModuleValidator& m)
{
    while (true) {
        ParseNode* varStmt;
        if (!ParseVarOrConstStatement(m.parser(), &varStmt))
            return false;
        if (!varStmt)
            break;
        for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
            if (!CheckModuleGlobal(m, var, varStmt->isKind(ParseNodeKind::Const)))
                return false;
        }
    }

    return true;
}

static bool
ArgFail(FunctionValidator& f, PropertyName* argName, ParseNode* stmt)
{
    return f.failName(stmt, "expecting argument type declaration for '%s' of the "
                      "form 'arg = arg|0' or 'arg = +arg' or 'arg = fround(arg)'", argName);
}

static bool
CheckArgumentType(FunctionValidator& f, ParseNode* stmt, PropertyName* name, Type* type)
{
    if (!stmt || !IsExpressionStatement(stmt))
        return ArgFail(f, name, stmt ? stmt : f.fn());

    ParseNode* initNode = ExpressionStatementExpr(stmt);
    if (!initNode->isKind(ParseNodeKind::Assign))
        return ArgFail(f, name, stmt);

    ParseNode* argNode = BinaryLeft(initNode);
    ParseNode* coercionNode = BinaryRight(initNode);

    if (!IsUseOfName(argNode, name))
        return ArgFail(f, name, stmt);

    ParseNode* coercedExpr;
    if (!CheckTypeAnnotation(f.m(), coercionNode, type, &coercedExpr))
        return false;

    if (!type->isArgType())
        return f.failName(stmt, "invalid type for argument '%s'", name);

    if (!IsUseOfName(coercedExpr, name))
        return ArgFail(f, name, stmt);

    return true;
}

static bool
CheckProcessingDirectives(ModuleValidator& m, ParseNode** stmtIter)
{
    ParseNode* stmt = *stmtIter;

    while (stmt && IsIgnoredDirective(m.cx(), stmt))
        stmt = NextNode(stmt);

    *stmtIter = stmt;
    return true;
}

static bool
CheckArguments(FunctionValidator& f, ParseNode** stmtIter, ValTypeVector* argTypes)
{
    ParseNode* stmt = *stmtIter;

    unsigned numFormals;
    ParseNode* argpn = FunctionFormalParametersList(f.fn(), &numFormals);

    for (unsigned i = 0; i < numFormals; i++, argpn = NextNode(argpn), stmt = NextNode(stmt)) {
        PropertyName* name;
        if (!CheckArgument(f.m(), argpn, &name))
            return false;

        Type type;
        if (!CheckArgumentType(f, stmt, name, &type))
            return false;

        if (!argTypes->append(type.canonicalToValType()))
            return false;

        if (!f.addLocal(argpn, name, type))
            return false;
    }

    *stmtIter = stmt;
    return true;
}

static bool
IsLiteralOrConst(FunctionValidator& f, ParseNode* pn, NumLit* lit)
{
    if (pn->isKind(ParseNodeKind::Name)) {
        const ModuleValidator::Global* global = f.lookupGlobal(pn->name());
        if (!global || global->which() != ModuleValidator::Global::ConstantLiteral)
            return false;

        *lit = global->constLiteralValue();
        return true;
    }

    if (!IsNumericLiteral(f.m(), pn))
        return false;

    *lit = ExtractNumericLiteral(f.m(), pn);
    return true;
}

static bool
CheckFinalReturn(FunctionValidator& f, ParseNode* lastNonEmptyStmt)
{
    if (!f.encoder().writeOp(Op::End))
        return false;

    if (!f.hasAlreadyReturned()) {
        f.setReturnedType(ExprType::Void);
        return true;
    }

    if (!lastNonEmptyStmt->isKind(ParseNodeKind::Return) && !IsVoid(f.returnedType()))
        return f.fail(lastNonEmptyStmt, "void incompatible with previous return type");

    return true;
}

static bool
CheckVariable(FunctionValidator& f, ParseNode* var, ValTypeVector* types, Vector<NumLit>* inits)
{
    if (!var->isKind(ParseNodeKind::Name))
        return f.fail(var, "local variable is not a plain name");

    PropertyName* name = var->name();

    if (!CheckIdentifier(f.m(), var, name))
        return false;

    ParseNode* initNode = MaybeInitializer(var);
    if (!initNode)
        return f.failName(var, "var '%s' needs explicit type declaration via an initial value", name);

    NumLit lit;
    if (!IsLiteralOrConst(f, initNode, &lit))
        return f.failName(var, "var '%s' initializer must be literal or const literal", name);

    if (!lit.valid())
        return f.failName(var, "var '%s' initializer out of range", name);

    Type type = Type::canonicalize(Type::lit(lit));

    return f.addLocal(var, name, type) &&
           types->append(type.canonicalToValType()) &&
           inits->append(lit);
}

static bool
CheckVariables(FunctionValidator& f, ParseNode** stmtIter)
{
    ParseNode* stmt = *stmtIter;

    uint32_t firstVar = f.numLocals();

    ValTypeVector types;
    Vector<NumLit> inits(f.cx());

    for (; stmt && stmt->isKind(ParseNodeKind::Var); stmt = NextNonEmptyStatement(stmt)) {
        for (ParseNode* var = VarListHead(stmt); var; var = NextNode(var)) {
            if (!CheckVariable(f, var, &types, &inits))
                return false;
        }
    }

    MOZ_ASSERT(f.encoder().empty());

    if (!EncodeLocalEntries(f.encoder(), types))
        return false;

    for (uint32_t i = 0; i < inits.length(); i++) {
        NumLit lit = inits[i];
        if (lit.isZeroBits())
            continue;
        if (!f.writeConstExpr(lit))
            return false;
        if (!f.encoder().writeOp(Op::SetLocal))
            return false;
        if (!f.encoder().writeVarU32(firstVar + i))
            return false;
    }

    *stmtIter = stmt;
    return true;
}

static bool
CheckExpr(FunctionValidator& f, ParseNode* op, Type* type);

static bool
CheckNumericLiteral(FunctionValidator& f, ParseNode* num, Type* type)
{
    NumLit lit = ExtractNumericLiteral(f.m(), num);
    if (!lit.valid())
        return f.fail(num, "numeric literal out of representable integer range");
    *type = Type::lit(lit);
    return f.writeConstExpr(lit);
}

static bool
CheckVarRef(FunctionValidator& f, ParseNode* varRef, Type* type)
{
    PropertyName* name = varRef->name();

    if (const FunctionValidator::Local* local = f.lookupLocal(name)) {
        if (!f.encoder().writeOp(Op::GetLocal))
            return false;
        if (!f.encoder().writeVarU32(local->slot))
            return false;
        *type = local->type;
        return true;
    }

    if (const ModuleValidator::Global* global = f.lookupGlobal(name)) {
        switch (global->which()) {
          case ModuleValidator::Global::ConstantLiteral:
            *type = global->varOrConstType();
            return f.writeConstExpr(global->constLiteralValue());
          case ModuleValidator::Global::ConstantImport:
          case ModuleValidator::Global::Variable: {
            *type = global->varOrConstType();
            return f.encoder().writeOp(Op::GetGlobal) &&
                   f.encoder().writeVarU32(global->varOrConstIndex());
          }
          case ModuleValidator::Global::Function:
          case ModuleValidator::Global::FFI:
          case ModuleValidator::Global::MathBuiltinFunction:
          case ModuleValidator::Global::AtomicsBuiltinFunction:
          case ModuleValidator::Global::Table:
          case ModuleValidator::Global::ArrayView:
          case ModuleValidator::Global::ArrayViewCtor:
          case ModuleValidator::Global::SimdCtor:
          case ModuleValidator::Global::SimdOp:
            break;
        }
        return f.failName(varRef, "'%s' may not be accessed by ordinary expressions", name);
    }

    return f.failName(varRef, "'%s' not found in local or asm.js module scope", name);
}

static inline bool
IsLiteralOrConstInt(FunctionValidator& f, ParseNode* pn, uint32_t* u32)
{
    NumLit lit;
    if (!IsLiteralOrConst(f, pn, &lit))
        return false;

    return IsLiteralInt(lit, u32);
}

static const int32_t NoMask = -1;
static const bool YesSimd = true;
static const bool NoSimd = false;

static bool
CheckArrayAccess(FunctionValidator& f, ParseNode* viewName, ParseNode* indexExpr,
                 bool isSimd, Scalar::Type* viewType)
{
    if (!viewName->isKind(ParseNodeKind::Name))
        return f.fail(viewName, "base of array access must be a typed array view name");

    const ModuleValidator::Global* global = f.lookupGlobal(viewName->name());
    if (!global || !global->isAnyArrayView())
        return f.fail(viewName, "base of array access must be a typed array view name");

    *viewType = global->viewType();

    uint32_t index;
    if (IsLiteralOrConstInt(f, indexExpr, &index)) {
        uint64_t byteOffset = uint64_t(index) << TypedArrayShift(*viewType);
        uint64_t width = isSimd ? Simd128DataSize : TypedArrayElemSize(*viewType);
        if (!f.m().tryConstantAccess(byteOffset, width))
            return f.fail(indexExpr, "constant index out of range");

        return f.writeInt32Lit(byteOffset);
    }

    // Mask off the low bits to account for the clearing effect of a right shift
    // followed by the left shift implicit in the array access. E.g., H32[i>>2]
    // loses the low two bits.
    int32_t mask = ~(TypedArrayElemSize(*viewType) - 1);

    if (indexExpr->isKind(ParseNodeKind::Rsh)) {
        ParseNode* shiftAmountNode = BitwiseRight(indexExpr);

        uint32_t shift;
        if (!IsLiteralInt(f.m(), shiftAmountNode, &shift))
            return f.failf(shiftAmountNode, "shift amount must be constant");

        unsigned requiredShift = TypedArrayShift(*viewType);
        if (shift != requiredShift)
            return f.failf(shiftAmountNode, "shift amount must be %u", requiredShift);

        ParseNode* pointerNode = BitwiseLeft(indexExpr);

        Type pointerType;
        if (!CheckExpr(f, pointerNode, &pointerType))
            return false;

        if (!pointerType.isIntish())
            return f.failf(pointerNode, "%s is not a subtype of int", pointerType.toChars());
    } else {
        // For SIMD access, and legacy scalar access compatibility, accept
        // Int8/Uint8 accesses with no shift.
        if (TypedArrayShift(*viewType) != 0)
            return f.fail(indexExpr, "index expression isn't shifted; must be an Int8/Uint8 access");

        MOZ_ASSERT(mask == NoMask);

        ParseNode* pointerNode = indexExpr;

        Type pointerType;
        if (!CheckExpr(f, pointerNode, &pointerType))
            return false;

        if (isSimd) {
            if (!pointerType.isIntish())
                return f.failf(pointerNode, "%s is not a subtype of intish", pointerType.toChars());
        } else {
            if (!pointerType.isInt())
                return f.failf(pointerNode, "%s is not a subtype of int", pointerType.toChars());
        }
    }

    // Don't generate the mask op if there is no need for it which could happen for
    // a shift of zero or a SIMD access.
    if (mask != NoMask) {
        return f.writeInt32Lit(mask) &&
               f.encoder().writeOp(Op::I32And);
    }

    return true;
}

static bool
CheckAndPrepareArrayAccess(FunctionValidator& f, ParseNode* viewName, ParseNode* indexExpr,
                           bool isSimd, Scalar::Type* viewType)
{
    return CheckArrayAccess(f, viewName, indexExpr, isSimd, viewType);
}

static bool
WriteArrayAccessFlags(FunctionValidator& f, Scalar::Type viewType)
{
    // asm.js only has naturally-aligned accesses.
    size_t align = TypedArrayElemSize(viewType);
    MOZ_ASSERT(IsPowerOfTwo(align));
    if (!f.encoder().writeFixedU8(CeilingLog2(align)))
        return false;

    // asm.js doesn't have constant offsets, so just encode a 0.
    if (!f.encoder().writeVarU32(0))
        return false;

    return true;
}

static bool
CheckLoadArray(FunctionValidator& f, ParseNode* elem, Type* type)
{
    Scalar::Type viewType;

    if (!CheckAndPrepareArrayAccess(f, ElemBase(elem), ElemIndex(elem), NoSimd, &viewType))
        return false;

    switch (viewType) {
      case Scalar::Int8:    if (!f.encoder().writeOp(Op::I32Load8S))  return false; break;
      case Scalar::Uint8:   if (!f.encoder().writeOp(Op::I32Load8U))  return false; break;
      case Scalar::Int16:   if (!f.encoder().writeOp(Op::I32Load16S)) return false; break;
      case Scalar::Uint16:  if (!f.encoder().writeOp(Op::I32Load16U)) return false; break;
      case Scalar::Uint32:
      case Scalar::Int32:   if (!f.encoder().writeOp(Op::I32Load))    return false; break;
      case Scalar::Float32: if (!f.encoder().writeOp(Op::F32Load))    return false; break;
      case Scalar::Float64: if (!f.encoder().writeOp(Op::F64Load))    return false; break;
      default: MOZ_CRASH("unexpected scalar type");
    }

    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        *type = Type::Intish;
        break;
      case Scalar::Float32:
        *type = Type::MaybeFloat;
        break;
      case Scalar::Float64:
        *type = Type::MaybeDouble;
        break;
      default: MOZ_CRASH("Unexpected array type");
    }

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    return true;
}

static bool
CheckStoreArray(FunctionValidator& f, ParseNode* lhs, ParseNode* rhs, Type* type)
{
    Scalar::Type viewType;
    if (!CheckAndPrepareArrayAccess(f, ElemBase(lhs), ElemIndex(lhs), NoSimd, &viewType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        if (!rhsType.isIntish())
            return f.failf(lhs, "%s is not a subtype of intish", rhsType.toChars());
        break;
      case Scalar::Float32:
        if (!rhsType.isMaybeDouble() && !rhsType.isFloatish())
            return f.failf(lhs, "%s is not a subtype of double? or floatish", rhsType.toChars());
        break;
      case Scalar::Float64:
        if (!rhsType.isMaybeFloat() && !rhsType.isMaybeDouble())
            return f.failf(lhs, "%s is not a subtype of float? or double?", rhsType.toChars());
        break;
      default:
        MOZ_CRASH("Unexpected view type");
    }

    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Uint8:
        if (!f.encoder().writeOp(MozOp::I32TeeStore8))
            return false;
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        if (!f.encoder().writeOp(MozOp::I32TeeStore16))
            return false;
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        if (!f.encoder().writeOp(MozOp::I32TeeStore))
            return false;
        break;
      case Scalar::Float32:
        if (rhsType.isFloatish()) {
            if (!f.encoder().writeOp(MozOp::F32TeeStore))
                return false;
        } else {
            if (!f.encoder().writeOp(MozOp::F64TeeStoreF32))
                return false;
        }
        break;
      case Scalar::Float64:
        if (rhsType.isFloatish()) {
            if (!f.encoder().writeOp(MozOp::F32TeeStoreF64))
                return false;
        } else {
            if (!f.encoder().writeOp(MozOp::F64TeeStore))
                return false;
        }
        break;
      default: MOZ_CRASH("unexpected scalar type");
    }

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = rhsType;
    return true;
}

static bool
CheckAssignName(FunctionValidator& f, ParseNode* lhs, ParseNode* rhs, Type* type)
{
    RootedPropertyName name(f.cx(), lhs->name());

    if (const FunctionValidator::Local* lhsVar = f.lookupLocal(name)) {
        Type rhsType;
        if (!CheckExpr(f, rhs, &rhsType))
            return false;

        if (!f.encoder().writeOp(Op::TeeLocal))
            return false;
        if (!f.encoder().writeVarU32(lhsVar->slot))
            return false;

        if (!(rhsType <= lhsVar->type)) {
            return f.failf(lhs, "%s is not a subtype of %s",
                           rhsType.toChars(), lhsVar->type.toChars());
        }
        *type = rhsType;
        return true;
    }

    if (const ModuleValidator::Global* global = f.lookupGlobal(name)) {
        if (global->which() != ModuleValidator::Global::Variable)
            return f.failName(lhs, "'%s' is not a mutable variable", name);

        Type rhsType;
        if (!CheckExpr(f, rhs, &rhsType))
            return false;

        Type globType = global->varOrConstType();
        if (!(rhsType <= globType))
            return f.failf(lhs, "%s is not a subtype of %s", rhsType.toChars(), globType.toChars());
        if (!f.encoder().writeOp(MozOp::TeeGlobal))
            return false;
        if (!f.encoder().writeVarU32(global->varOrConstIndex()))
            return false;

        *type = rhsType;
        return true;
    }

    return f.failName(lhs, "'%s' not found in local or asm.js module scope", name);
}

static bool
CheckAssign(FunctionValidator& f, ParseNode* assign, Type* type)
{
    MOZ_ASSERT(assign->isKind(ParseNodeKind::Assign));

    ParseNode* lhs = BinaryLeft(assign);
    ParseNode* rhs = BinaryRight(assign);

    if (lhs->getKind() == ParseNodeKind::Elem)
        return CheckStoreArray(f, lhs, rhs, type);

    if (lhs->getKind() == ParseNodeKind::Name)
        return CheckAssignName(f, lhs, rhs, type);

    return f.fail(assign, "left-hand side of assignment must be a variable or array access");
}

static bool
CheckMathIMul(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 2)
        return f.fail(call, "Math.imul must be passed 2 arguments");

    ParseNode* lhs = CallArgList(call);
    ParseNode* rhs = NextNode(lhs);

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (!lhsType.isIntish())
        return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
    if (!rhsType.isIntish())
        return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());

    *type = Type::Signed;
    return f.encoder().writeOp(Op::I32Mul);
}

static bool
CheckMathClz32(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.clz32 must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (!argType.isIntish())
        return f.failf(arg, "%s is not a subtype of intish", argType.toChars());

    *type = Type::Fixnum;
    return f.encoder().writeOp(Op::I32Clz);
}

static bool
CheckMathAbs(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.abs must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (argType.isSigned()) {
        *type = Type::Unsigned;
        return f.encoder().writeOp(MozOp::I32Abs);
    }

    if (argType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Abs);
    }

    if (argType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Abs);
    }

    return f.failf(call, "%s is not a subtype of signed, float? or double?", argType.toChars());
}

static bool
CheckMathSqrt(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.sqrt must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (argType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Sqrt);
    }

    if (argType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Sqrt);
    }

    return f.failf(call, "%s is neither a subtype of double? nor float?", argType.toChars());
}

static bool
CheckMathMinMax(FunctionValidator& f, ParseNode* callNode, bool isMax, Type* type)
{
    if (CallArgListLength(callNode) < 2)
        return f.fail(callNode, "Math.min/max must be passed at least 2 arguments");

    ParseNode* firstArg = CallArgList(callNode);
    Type firstType;
    if (!CheckExpr(f, firstArg, &firstType))
        return false;

    Op op = Op::Limit;
    MozOp mozOp = MozOp::Limit;
    if (firstType.isMaybeDouble()) {
        *type = Type::Double;
        firstType = Type::MaybeDouble;
        op = isMax ? Op::F64Max : Op::F64Min;
    } else if (firstType.isMaybeFloat()) {
        *type = Type::Float;
        firstType = Type::MaybeFloat;
        op = isMax ? Op::F32Max : Op::F32Min;
    } else if (firstType.isSigned()) {
        *type = Type::Signed;
        firstType = Type::Signed;
        mozOp = isMax ? MozOp::I32Max : MozOp::I32Min;
    } else {
        return f.failf(firstArg, "%s is not a subtype of double?, float? or signed",
                       firstType.toChars());
    }

    unsigned numArgs = CallArgListLength(callNode);
    ParseNode* nextArg = NextNode(firstArg);
    for (unsigned i = 1; i < numArgs; i++, nextArg = NextNode(nextArg)) {
        Type nextType;
        if (!CheckExpr(f, nextArg, &nextType))
            return false;
        if (!(nextType <= firstType))
            return f.failf(nextArg, "%s is not a subtype of %s", nextType.toChars(), firstType.toChars());

        if (op != Op::Limit) {
            if (!f.encoder().writeOp(op))
                return false;
        } else {
            if (!f.encoder().writeOp(mozOp))
                return false;
        }
    }

    return true;
}

static bool
CheckSharedArrayAtomicAccess(FunctionValidator& f, ParseNode* viewName, ParseNode* indexExpr,
                             Scalar::Type* viewType)
{
    if (!CheckAndPrepareArrayAccess(f, viewName, indexExpr, NoSimd, viewType))
        return false;

    // The global will be sane, CheckArrayAccess checks it.
    const ModuleValidator::Global* global = f.lookupGlobal(viewName->name());
    if (global->which() != ModuleValidator::Global::ArrayView)
        return f.fail(viewName, "base of array access must be a typed array view");

    MOZ_ASSERT(f.m().atomicsPresent());

    switch (*viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        return true;
      default:
        return f.failf(viewName, "not an integer array");
    }
}

static bool
WriteAtomicOperator(FunctionValidator& f, MozOp opcode, Scalar::Type viewType)
{
    return f.encoder().writeOp(opcode) &&
           f.encoder().writeFixedU8(viewType);
}

static bool
CheckAtomicsLoad(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 2)
        return f.fail(call, "Atomics.load must be passed 2 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, MozOp::I32AtomicsLoad, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsStore(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics.store must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Type rhsType;
    if (!CheckExpr(f, valueArg, &rhsType))
        return false;

    if (!rhsType.isIntish())
        return f.failf(arrayArg, "%s is not a subtype of intish", rhsType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, MozOp::I32AtomicsStore, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = rhsType;
    return true;
}

static bool
CheckAtomicsBinop(FunctionValidator& f, ParseNode* call, Type* type, AtomicOp op)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics binary operator must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Type valueArgType;
    if (!CheckExpr(f, valueArg, &valueArgType))
        return false;

    if (!valueArgType.isIntish())
        return f.failf(valueArg, "%s is not a subtype of intish", valueArgType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, MozOp::I32AtomicsBinOp, viewType))
        return false;
    if (!f.encoder().writeFixedU8(uint8_t(op)))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsIsLockFree(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Atomics.isLockFree must be passed 1 argument");

    ParseNode* sizeArg = CallArgList(call);

    uint32_t size;
    if (!IsLiteralInt(f.m(), sizeArg, &size))
        return f.fail(sizeArg, "Atomics.isLockFree requires an integer literal argument");

    *type = Type::Int;
    return f.writeInt32Lit(AtomicOperations::isLockfreeJS(size));
}

static bool
CheckAtomicsCompareExchange(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 4)
        return f.fail(call, "Atomics.compareExchange must be passed 4 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* oldValueArg = NextNode(indexArg);
    ParseNode* newValueArg = NextNode(oldValueArg);

    Type oldValueArgType;
    if (!CheckExpr(f, oldValueArg, &oldValueArgType))
        return false;

    Type newValueArgType;
    if (!CheckExpr(f, newValueArg, &newValueArgType))
        return false;

    if (!oldValueArgType.isIntish())
        return f.failf(oldValueArg, "%s is not a subtype of intish", oldValueArgType.toChars());

    if (!newValueArgType.isIntish())
        return f.failf(newValueArg, "%s is not a subtype of intish", newValueArgType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, MozOp::I32AtomicsCompareExchange, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsExchange(FunctionValidator& f, ParseNode* call, Type* type)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics.exchange must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Type valueArgType;
    if (!CheckExpr(f, valueArg, &valueArgType))
        return false;

    if (!valueArgType.isIntish())
        return f.failf(arrayArg, "%s is not a subtype of intish", valueArgType.toChars());

    Scalar::Type viewType;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType))
        return false;

    if (!WriteAtomicOperator(f, MozOp::I32AtomicsExchange, viewType))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = Type::Int;
    return true;
}

static bool
CheckAtomicsBuiltinCall(FunctionValidator& f, ParseNode* callNode, AsmJSAtomicsBuiltinFunction func,
                        Type* type)
{
    switch (func) {
      case AsmJSAtomicsBuiltin_compareExchange:
        return CheckAtomicsCompareExchange(f, callNode, type);
      case AsmJSAtomicsBuiltin_exchange:
        return CheckAtomicsExchange(f, callNode, type);
      case AsmJSAtomicsBuiltin_load:
        return CheckAtomicsLoad(f, callNode, type);
      case AsmJSAtomicsBuiltin_store:
        return CheckAtomicsStore(f, callNode, type);
      case AsmJSAtomicsBuiltin_add:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchAddOp);
      case AsmJSAtomicsBuiltin_sub:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchSubOp);
      case AsmJSAtomicsBuiltin_and:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchAndOp);
      case AsmJSAtomicsBuiltin_or:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchOrOp);
      case AsmJSAtomicsBuiltin_xor:
        return CheckAtomicsBinop(f, callNode, type, AtomicFetchXorOp);
      case AsmJSAtomicsBuiltin_isLockFree:
        return CheckAtomicsIsLockFree(f, callNode, type);
      default:
        MOZ_CRASH("unexpected atomicsBuiltin function");
    }
}

typedef bool (*CheckArgType)(FunctionValidator& f, ParseNode* argNode, Type type);

template <CheckArgType checkArg>
static bool
CheckCallArgs(FunctionValidator& f, ParseNode* callNode, ValTypeVector* args)
{
    ParseNode* argNode = CallArgList(callNode);
    for (unsigned i = 0; i < CallArgListLength(callNode); i++, argNode = NextNode(argNode)) {
        Type type;
        if (!CheckExpr(f, argNode, &type))
            return false;

        if (!checkArg(f, argNode, type))
            return false;

        if (!args->append(Type::canonicalize(type).canonicalToValType()))
            return false;
    }
    return true;
}

static bool
CheckSignatureAgainstExisting(ModuleValidator& m, ParseNode* usepn, const Sig& sig, const Sig& existing)
{
    if (sig.args().length() != existing.args().length()) {
        return m.failf(usepn, "incompatible number of arguments (%zu"
                       " here vs. %zu before)",
                       sig.args().length(), existing.args().length());
    }

    for (unsigned i = 0; i < sig.args().length(); i++) {
        if (sig.arg(i) != existing.arg(i)) {
            return m.failf(usepn, "incompatible type for argument %u: (%s here vs. %s before)", i,
                           ToCString(sig.arg(i)), ToCString(existing.arg(i)));
        }
    }

    if (sig.ret() != existing.ret()) {
        return m.failf(usepn, "%s incompatible with previous return of type %s",
                       ToCString(sig.ret()), ToCString(existing.ret()));
    }

    MOZ_ASSERT(sig == existing);
    return true;
}

static bool
CheckFunctionSignature(ModuleValidator& m, ParseNode* usepn, Sig&& sig, PropertyName* name,
                       ModuleValidator::Func** func)
{
    if (sig.args().length() > MaxParams)
        return m.failf(usepn, "too many parameters");

    ModuleValidator::Func* existing = m.lookupFuncDef(name);
    if (!existing) {
        if (!CheckModuleLevelName(m, usepn, name))
            return false;
        return m.addFuncDef(name, usepn->pn_pos.begin, Move(sig), func);
    }

    const SigWithId& existingSig = m.env().sigs[existing->sigIndex()];

    if (!CheckSignatureAgainstExisting(m, usepn, sig, existingSig))
        return false;

    *func = existing;
    return true;
}

static bool
CheckIsArgType(FunctionValidator& f, ParseNode* argNode, Type type)
{
    if (!type.isArgType())
        return f.failf(argNode,
                       "%s is not a subtype of int, float, double, or an allowed SIMD type",
                       type.toChars());

    return true;
}

static bool
CheckInternalCall(FunctionValidator& f, ParseNode* callNode, PropertyName* calleeName,
                  Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    ValTypeVector args;
    if (!CheckCallArgs<CheckIsArgType>(f, callNode, &args))
        return false;

    Sig sig(Move(args), ret.canonicalToExprType());

    ModuleValidator::Func* callee;
    if (!CheckFunctionSignature(f.m(), callNode, Move(sig), calleeName, &callee))
        return false;

    if (!f.writeCall(callNode, MozOp::OldCallDirect))
        return false;

    if (!f.encoder().writeVarU32(callee->funcDefIndex()))
        return false;

    *type = Type::ret(ret);
    return true;
}

static bool
CheckFuncPtrTableAgainstExisting(ModuleValidator& m, ParseNode* usepn, PropertyName* name,
                                 Sig&& sig, unsigned mask, uint32_t* tableIndex)
{
    if (const ModuleValidator::Global* existing = m.lookupGlobal(name)) {
        if (existing->which() != ModuleValidator::Global::Table)
            return m.failName(usepn, "'%s' is not a function-pointer table", name);

        ModuleValidator::Table& table = m.table(existing->tableIndex());
        if (mask != table.mask())
            return m.failf(usepn, "mask does not match previous value (%u)", table.mask());

        if (!CheckSignatureAgainstExisting(m, usepn, sig, m.env().sigs[table.sigIndex()]))
            return false;

        *tableIndex = existing->tableIndex();
        return true;
    }

    if (!CheckModuleLevelName(m, usepn, name))
        return false;

    if (!m.declareFuncPtrTable(Move(sig), name, usepn->pn_pos.begin, mask, tableIndex))
        return false;

    return true;
}

static bool
CheckFuncPtrCall(FunctionValidator& f, ParseNode* callNode, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    ParseNode* callee = CallCallee(callNode);
    ParseNode* tableNode = ElemBase(callee);
    ParseNode* indexExpr = ElemIndex(callee);

    if (!tableNode->isKind(ParseNodeKind::Name))
        return f.fail(tableNode, "expecting name of function-pointer array");

    PropertyName* name = tableNode->name();
    if (const ModuleValidator::Global* existing = f.lookupGlobal(name)) {
        if (existing->which() != ModuleValidator::Global::Table)
            return f.failName(tableNode, "'%s' is not the name of a function-pointer array", name);
    }

    if (!indexExpr->isKind(ParseNodeKind::BitAnd))
        return f.fail(indexExpr, "function-pointer table index expression needs & mask");

    ParseNode* indexNode = BitwiseLeft(indexExpr);
    ParseNode* maskNode = BitwiseRight(indexExpr);

    uint32_t mask;
    if (!IsLiteralInt(f.m(), maskNode, &mask) || mask == UINT32_MAX || !IsPowerOfTwo(mask + 1))
        return f.fail(maskNode, "function-pointer table index mask value must be a power of two minus 1");

    Type indexType;
    if (!CheckExpr(f, indexNode, &indexType))
        return false;

    if (!indexType.isIntish())
        return f.failf(indexNode, "%s is not a subtype of intish", indexType.toChars());

    ValTypeVector args;
    if (!CheckCallArgs<CheckIsArgType>(f, callNode, &args))
        return false;

    Sig sig(Move(args), ret.canonicalToExprType());

    uint32_t tableIndex;
    if (!CheckFuncPtrTableAgainstExisting(f.m(), tableNode, name, Move(sig), mask, &tableIndex))
        return false;

    if (!f.writeCall(callNode, MozOp::OldCallIndirect))
        return false;

    // Call signature
    if (!f.encoder().writeVarU32(f.m().table(tableIndex).sigIndex()))
        return false;

    *type = Type::ret(ret);
    return true;
}

static bool
CheckIsExternType(FunctionValidator& f, ParseNode* argNode, Type type)
{
    if (!type.isExtern())
        return f.failf(argNode, "%s is not a subtype of extern", type.toChars());
    return true;
}

static bool
CheckFFICall(FunctionValidator& f, ParseNode* callNode, unsigned ffiIndex, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    PropertyName* calleeName = CallCallee(callNode)->name();

    if (ret.isFloat())
        return f.fail(callNode, "FFI calls can't return float");
    if (ret.isSimd())
        return f.fail(callNode, "FFI calls can't return SIMD values");

    ValTypeVector args;
    if (!CheckCallArgs<CheckIsExternType>(f, callNode, &args))
        return false;

    Sig sig(Move(args), ret.canonicalToExprType());

    uint32_t importIndex;
    if (!f.m().declareImport(calleeName, Move(sig), ffiIndex, &importIndex))
        return false;

    if (!f.writeCall(callNode, Op::Call))
        return false;

    if (!f.encoder().writeVarU32(importIndex))
        return false;

    *type = Type::ret(ret);
    return true;
}

static bool
CheckFloatCoercionArg(FunctionValidator& f, ParseNode* inputNode, Type inputType)
{
    if (inputType.isMaybeDouble())
        return f.encoder().writeOp(Op::F32DemoteF64);
    if (inputType.isSigned())
        return f.encoder().writeOp(Op::F32ConvertSI32);
    if (inputType.isUnsigned())
        return f.encoder().writeOp(Op::F32ConvertUI32);
    if (inputType.isFloatish())
        return true;

    return f.failf(inputNode, "%s is not a subtype of signed, unsigned, double? or floatish",
                   inputType.toChars());
}

static bool
CheckCoercedCall(FunctionValidator& f, ParseNode* call, Type ret, Type* type);

static bool
CheckCoercionArg(FunctionValidator& f, ParseNode* arg, Type expected, Type* type)
{
    MOZ_ASSERT(expected.isCanonicalValType());

    if (arg->isKind(ParseNodeKind::Call))
        return CheckCoercedCall(f, arg, expected, type);

    Type argType;
    if (!CheckExpr(f, arg, &argType))
        return false;

    if (expected.isFloat()) {
        if (!CheckFloatCoercionArg(f, arg, argType))
            return false;
    } else if (expected.isSimd()) {
        if (!(argType <= expected))
            return f.fail(arg, "argument to SIMD coercion isn't from the correct SIMD type");
    } else {
        MOZ_CRASH("not call coercions");
    }

    *type = Type::ret(expected);
    return true;
}

static bool
CheckMathFRound(FunctionValidator& f, ParseNode* callNode, Type* type)
{
    if (CallArgListLength(callNode) != 1)
        return f.fail(callNode, "Math.fround must be passed 1 argument");

    ParseNode* argNode = CallArgList(callNode);
    Type argType;
    if (!CheckCoercionArg(f, argNode, Type::Float, &argType))
        return false;

    MOZ_ASSERT(argType == Type::Float);
    *type = Type::Float;
    return true;
}

static bool
CheckMathBuiltinCall(FunctionValidator& f, ParseNode* callNode, AsmJSMathBuiltinFunction func,
                     Type* type)
{
    unsigned arity = 0;
    Op f32 = Op::Limit;
    Op f64 = Op::Limit;
    MozOp mozf64 = MozOp::Limit;
    switch (func) {
      case AsmJSMathBuiltin_imul:   return CheckMathIMul(f, callNode, type);
      case AsmJSMathBuiltin_clz32:  return CheckMathClz32(f, callNode, type);
      case AsmJSMathBuiltin_abs:    return CheckMathAbs(f, callNode, type);
      case AsmJSMathBuiltin_sqrt:   return CheckMathSqrt(f, callNode, type);
      case AsmJSMathBuiltin_fround: return CheckMathFRound(f, callNode, type);
      case AsmJSMathBuiltin_min:    return CheckMathMinMax(f, callNode, /* isMax = */ false, type);
      case AsmJSMathBuiltin_max:    return CheckMathMinMax(f, callNode, /* isMax = */ true, type);
      case AsmJSMathBuiltin_ceil:   arity = 1; f64 = Op::F64Ceil;        f32 = Op::F32Ceil;     break;
      case AsmJSMathBuiltin_floor:  arity = 1; f64 = Op::F64Floor;       f32 = Op::F32Floor;    break;
      case AsmJSMathBuiltin_sin:    arity = 1; mozf64 = MozOp::F64Sin;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_cos:    arity = 1; mozf64 = MozOp::F64Cos;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_tan:    arity = 1; mozf64 = MozOp::F64Tan;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_asin:   arity = 1; mozf64 = MozOp::F64Asin;  f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_acos:   arity = 1; mozf64 = MozOp::F64Acos;  f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_atan:   arity = 1; mozf64 = MozOp::F64Atan;  f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_exp:    arity = 1; mozf64 = MozOp::F64Exp;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_log:    arity = 1; mozf64 = MozOp::F64Log;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_pow:    arity = 2; mozf64 = MozOp::F64Pow;   f32 = Op::Unreachable; break;
      case AsmJSMathBuiltin_atan2:  arity = 2; mozf64 = MozOp::F64Atan2; f32 = Op::Unreachable; break;
      default: MOZ_CRASH("unexpected mathBuiltin function");
    }

    unsigned actualArity = CallArgListLength(callNode);
    if (actualArity != arity)
        return f.failf(callNode, "call passed %u arguments, expected %u", actualArity, arity);

    if (!f.prepareCall(callNode))
        return false;

    Type firstType;
    ParseNode* argNode = CallArgList(callNode);
    if (!CheckExpr(f, argNode, &firstType))
        return false;

    if (!firstType.isMaybeFloat() && !firstType.isMaybeDouble())
        return f.fail(argNode, "arguments to math call should be a subtype of double? or float?");

    bool opIsDouble = firstType.isMaybeDouble();
    if (!opIsDouble && f32 == Op::Unreachable)
        return f.fail(callNode, "math builtin cannot be used as float");

    if (arity == 2) {
        Type secondType;
        argNode = NextNode(argNode);
        if (!CheckExpr(f, argNode, &secondType))
            return false;

        if (firstType.isMaybeDouble() && !secondType.isMaybeDouble())
            return f.fail(argNode, "both arguments to math builtin call should be the same type");
        if (firstType.isMaybeFloat() && !secondType.isMaybeFloat())
            return f.fail(argNode, "both arguments to math builtin call should be the same type");
    }

    if (opIsDouble) {
        if (f64 != Op::Limit) {
            if (!f.encoder().writeOp(f64))
                return false;
        } else {
            if (!f.encoder().writeOp(mozf64))
                return false;
        }
    } else {
        if (!f.encoder().writeOp(f32))
            return false;
    }

    *type = opIsDouble ? Type::Double : Type::Floatish;
    return true;
}

namespace {
// Include CheckSimdCallArgs in unnamed namespace to avoid MSVC name lookup bug.

template<class CheckArgOp>
static bool
CheckSimdCallArgs(FunctionValidator& f, ParseNode* call, unsigned expectedArity,
                  const CheckArgOp& checkArg)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != expectedArity)
        return f.failf(call, "expected %u arguments to SIMD call, got %u", expectedArity, numArgs);

    ParseNode* arg = CallArgList(call);
    for (size_t i = 0; i < numArgs; i++, arg = NextNode(arg)) {
        MOZ_ASSERT(!!arg);
        Type argType;
        if (!CheckExpr(f, arg, &argType))
            return false;
        if (!checkArg(f, arg, i, argType))
            return false;
    }

    return true;
}


class CheckArgIsSubtypeOf
{
    Type formalType_;

  public:
    explicit CheckArgIsSubtypeOf(SimdType t) : formalType_(t) {}

    bool operator()(FunctionValidator& f, ParseNode* arg, unsigned argIndex, Type actualType) const
    {
        if (!(actualType <= formalType_)) {
            return f.failf(arg, "%s is not a subtype of %s", actualType.toChars(),
                           formalType_.toChars());
        }
        return true;
    }
};

static inline Type
SimdToCoercedScalarType(SimdType t)
{
    switch (t) {
      case SimdType::Int8x16:
      case SimdType::Int16x8:
      case SimdType::Int32x4:
      case SimdType::Uint8x16:
      case SimdType::Uint16x8:
      case SimdType::Uint32x4:
      case SimdType::Bool8x16:
      case SimdType::Bool16x8:
      case SimdType::Bool32x4:
        return Type::Intish;
      case SimdType::Float32x4:
        return Type::Floatish;
      default:
        break;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected SIMD type");
}

class CheckSimdScalarArgs
{
    SimdType simdType_;
    Type formalType_;

  public:
    explicit CheckSimdScalarArgs(SimdType simdType)
      : simdType_(simdType), formalType_(SimdToCoercedScalarType(simdType))
    {}

    bool operator()(FunctionValidator& f, ParseNode* arg, unsigned argIndex, Type actualType) const
    {
        if (!(actualType <= formalType_)) {
            // As a special case, accept doublelit arguments to float32x4 ops by
            // re-emitting them as float32 constants.
            if (simdType_ != SimdType::Float32x4 || !actualType.isDoubleLit()) {
                return f.failf(arg, "%s is not a subtype of %s%s",
                               actualType.toChars(), formalType_.toChars(),
                               simdType_ == SimdType::Float32x4 ? " or doublelit" : "");
            }

            // We emitted a double literal and actually want a float32.
            return f.encoder().writeOp(Op::F32DemoteF64);
        }

        return true;
    }
};

class CheckSimdSelectArgs
{
    Type formalType_;
    Type maskType_;

  public:
    explicit CheckSimdSelectArgs(SimdType t) : formalType_(t), maskType_(GetBooleanSimdType(t)) {}

    bool operator()(FunctionValidator& f, ParseNode* arg, unsigned argIndex, Type actualType) const
    {
        // The first argument is the boolean selector, the next two are the
        // values to choose from.
        Type wantedType = argIndex == 0 ? maskType_ : formalType_;

        if (!(actualType <= wantedType)) {
            return f.failf(arg, "%s is not a subtype of %s", actualType.toChars(),
                           wantedType.toChars());
        }
        return true;
    }
};

class CheckSimdVectorScalarArgs
{
    SimdType formalSimdType_;

  public:
    explicit CheckSimdVectorScalarArgs(SimdType t) : formalSimdType_(t) {}

    bool operator()(FunctionValidator& f, ParseNode* arg, unsigned argIndex, Type actualType) const
    {
        MOZ_ASSERT(argIndex < 2);
        if (argIndex == 0) {
            // First argument is the vector
            if (!(actualType <= Type(formalSimdType_))) {
                return f.failf(arg, "%s is not a subtype of %s", actualType.toChars(),
                               Type(formalSimdType_).toChars());
            }

            return true;
        }

        // Second argument is the scalar
        return CheckSimdScalarArgs(formalSimdType_)(f, arg, argIndex, actualType);
    }
};

} // namespace

static bool
CheckSimdUnary(FunctionValidator& f, ParseNode* call, SimdType opType, SimdOperation op,
               Type* type)
{
    if (!CheckSimdCallArgs(f, call, 1, CheckArgIsSubtypeOf(opType)))
        return false;
    if (!f.writeSimdOp(opType, op))
        return false;
    *type = opType;
    return true;
}

static bool
CheckSimdBinaryShift(FunctionValidator& f, ParseNode* call, SimdType opType, SimdOperation op,
                     Type *type)
{
    if (!CheckSimdCallArgs(f, call, 2, CheckSimdVectorScalarArgs(opType)))
        return false;
    if (!f.writeSimdOp(opType, op))
        return false;
    *type = opType;
    return true;
}

static bool
CheckSimdBinaryComp(FunctionValidator& f, ParseNode* call, SimdType opType, SimdOperation op,
                    Type *type)
{
    if (!CheckSimdCallArgs(f, call, 2, CheckArgIsSubtypeOf(opType)))
        return false;
    if (!f.writeSimdOp(opType, op))
        return false;
    *type = GetBooleanSimdType(opType);
    return true;
}

static bool
CheckSimdBinary(FunctionValidator& f, ParseNode* call, SimdType opType, SimdOperation op,
                Type* type)
{
    if (!CheckSimdCallArgs(f, call, 2, CheckArgIsSubtypeOf(opType)))
        return false;
    if (!f.writeSimdOp(opType, op))
        return false;
    *type = opType;
    return true;
}

static bool
CheckSimdExtractLane(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    switch (opType) {
      case SimdType::Int8x16:
      case SimdType::Int16x8:
      case SimdType::Int32x4:   *type = Type::Signed; break;
      case SimdType::Uint8x16:
      case SimdType::Uint16x8:
      case SimdType::Uint32x4:  *type = Type::Unsigned; break;
      case SimdType::Float32x4: *type = Type::Float; break;
      case SimdType::Bool8x16:
      case SimdType::Bool16x8:
      case SimdType::Bool32x4:  *type = Type::Int; break;
      default:                  MOZ_CRASH("unhandled simd type");
    }

    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 2)
        return f.failf(call, "expected 2 arguments to SIMD extract, got %u", numArgs);

    ParseNode* arg = CallArgList(call);

    // First argument is the vector
    Type vecType;
    if (!CheckExpr(f, arg, &vecType))
        return false;
    if (!(vecType <= Type(opType))) {
        return f.failf(arg, "%s is not a subtype of %s", vecType.toChars(),
                       Type(opType).toChars());
    }

    arg = NextNode(arg);

    // Second argument is the lane < vector length
    uint32_t lane;
    if (!IsLiteralOrConstInt(f, arg, &lane))
        return f.failf(arg, "lane selector should be a constant integer literal");
    if (lane >= GetSimdLanes(opType))
        return f.failf(arg, "lane selector should be in bounds");

    if (!f.writeSimdOp(opType, SimdOperation::Fn_extractLane))
        return false;
    if (!f.encoder().writeVarU32(lane))
        return false;
    return true;
}

static bool
CheckSimdReplaceLane(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 3)
        return f.failf(call, "expected 2 arguments to SIMD replace, got %u", numArgs);

    ParseNode* arg = CallArgList(call);

    // First argument is the vector
    Type vecType;
    if (!CheckExpr(f, arg, &vecType))
        return false;
    if (!(vecType <= Type(opType))) {
        return f.failf(arg, "%s is not a subtype of %s", vecType.toChars(),
                       Type(opType).toChars());
    }

    arg = NextNode(arg);

    // Second argument is the lane < vector length
    uint32_t lane;
    if (!IsLiteralOrConstInt(f, arg, &lane))
        return f.failf(arg, "lane selector should be a constant integer literal");
    if (lane >= GetSimdLanes(opType))
        return f.failf(arg, "lane selector should be in bounds");

    arg = NextNode(arg);

    // Third argument is the scalar
    Type scalarType;
    if (!CheckExpr(f, arg, &scalarType))
        return false;
    if (!(scalarType <= SimdToCoercedScalarType(opType))) {
        if (opType == SimdType::Float32x4 && scalarType.isDoubleLit()) {
            if (!f.encoder().writeOp(Op::F32DemoteF64))
                return false;
        } else {
            return f.failf(arg, "%s is not the correct type to replace an element of %s",
                           scalarType.toChars(), vecType.toChars());
        }
    }

    if (!f.writeSimdOp(opType, SimdOperation::Fn_replaceLane))
        return false;
    if (!f.encoder().writeVarU32(lane))
        return false;
    *type = opType;
    return true;
}

typedef bool Bitcast;

namespace {
// Include CheckSimdCast in unnamed namespace to avoid MSVC name lookup bug (due to the use of Type).

static bool
CheckSimdCast(FunctionValidator& f, ParseNode* call, SimdType fromType, SimdType toType,
              SimdOperation op, Type* type)
{
    if (!CheckSimdCallArgs(f, call, 1, CheckArgIsSubtypeOf(fromType)))
        return false;
    if (!f.writeSimdOp(toType, op))
        return false;
    *type = toType;
    return true;
}

} // namespace

static bool
CheckSimdShuffleSelectors(FunctionValidator& f, ParseNode* lane,
                          mozilla::Array<uint8_t, 16>& lanes, unsigned numLanes, unsigned maxLane)
{
    for (unsigned i = 0; i < numLanes; i++, lane = NextNode(lane)) {
        uint32_t u32;
        if (!IsLiteralInt(f.m(), lane, &u32))
            return f.failf(lane, "lane selector should be a constant integer literal");
        if (u32 >= maxLane)
            return f.failf(lane, "lane selector should be less than %u", maxLane);
        lanes[i] = uint8_t(u32);
    }
    return true;
}

static bool
CheckSimdSwizzle(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    const unsigned numLanes = GetSimdLanes(opType);
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 1 + numLanes)
        return f.failf(call, "expected %u arguments to SIMD swizzle, got %u", 1 + numLanes,
                       numArgs);

    Type retType = opType;
    ParseNode* vec = CallArgList(call);
    Type vecType;
    if (!CheckExpr(f, vec, &vecType))
        return false;
    if (!(vecType <= retType))
        return f.failf(vec, "%s is not a subtype of %s", vecType.toChars(), retType.toChars());

    if (!f.writeSimdOp(opType, SimdOperation::Fn_swizzle))
        return false;

    mozilla::Array<uint8_t, 16> lanes;
    if (!CheckSimdShuffleSelectors(f, NextNode(vec), lanes, numLanes, numLanes))
        return false;

    for (unsigned i = 0; i < numLanes; i++) {
        if (!f.encoder().writeFixedU8(lanes[i]))
            return false;
    }

    *type = retType;
    return true;
}

static bool
CheckSimdShuffle(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    const unsigned numLanes = GetSimdLanes(opType);
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 2 + numLanes)
        return f.failf(call, "expected %u arguments to SIMD shuffle, got %u", 2 + numLanes,
                       numArgs);

    Type retType = opType;
    ParseNode* arg = CallArgList(call);
    for (unsigned i = 0; i < 2; i++, arg = NextNode(arg)) {
        Type type;
        if (!CheckExpr(f, arg, &type))
            return false;
        if (!(type <= retType))
            return f.failf(arg, "%s is not a subtype of %s", type.toChars(), retType.toChars());
    }

    if (!f.writeSimdOp(opType, SimdOperation::Fn_shuffle))
        return false;

    mozilla::Array<uint8_t, 16> lanes;
    if (!CheckSimdShuffleSelectors(f, arg, lanes, numLanes, 2 * numLanes))
        return false;

    for (unsigned i = 0; i < numLanes; i++) {
        if (!f.encoder().writeFixedU8(uint8_t(lanes[i])))
            return false;
    }

    *type = retType;
    return true;
}

static bool
CheckSimdLoadStoreArgs(FunctionValidator& f, ParseNode* call, Scalar::Type* viewType)
{
    ParseNode* view = CallArgList(call);
    if (!view->isKind(ParseNodeKind::Name))
        return f.fail(view, "expected Uint8Array view as SIMD.*.load/store first argument");

    ParseNode* indexExpr = NextNode(view);

    if (!CheckAndPrepareArrayAccess(f, view, indexExpr, YesSimd, viewType))
        return false;

    if (*viewType != Scalar::Uint8)
        return f.fail(view, "expected Uint8Array view as SIMD.*.load/store first argument");

    return true;
}

static bool
CheckSimdLoad(FunctionValidator& f, ParseNode* call, SimdType opType, SimdOperation op,
              Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 2)
        return f.failf(call, "expected 2 arguments to SIMD load, got %u", numArgs);

    Scalar::Type viewType;
    if (!CheckSimdLoadStoreArgs(f, call, &viewType))
        return false;

    if (!f.writeSimdOp(opType, op))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    *type = opType;
    return true;
}

static bool
CheckSimdStore(FunctionValidator& f, ParseNode* call, SimdType opType, SimdOperation op,
               Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 3)
        return f.failf(call, "expected 3 arguments to SIMD store, got %u", numArgs);

    Scalar::Type viewType;
    if (!CheckSimdLoadStoreArgs(f, call, &viewType))
        return false;

    Type retType = opType;
    ParseNode* vecExpr = NextNode(NextNode(CallArgList(call)));
    Type vecType;
    if (!CheckExpr(f, vecExpr, &vecType))
        return false;

    if (!f.writeSimdOp(opType, op))
        return false;

    if (!WriteArrayAccessFlags(f, viewType))
        return false;

    if (!(vecType <= retType))
        return f.failf(vecExpr, "%s is not a subtype of %s", vecType.toChars(), retType.toChars());

    *type = vecType;
    return true;
}

static bool
CheckSimdSelect(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    if (!CheckSimdCallArgs(f, call, 3, CheckSimdSelectArgs(opType)))
        return false;
    if (!f.writeSimdOp(opType, SimdOperation::Fn_select))
        return false;
    *type = opType;
    return true;
}

static bool
CheckSimdAllTrue(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    if (!CheckSimdCallArgs(f, call, 1, CheckArgIsSubtypeOf(opType)))
        return false;
    if (!f.writeSimdOp(opType, SimdOperation::Fn_allTrue))
        return false;
    *type = Type::Int;
    return true;
}

static bool
CheckSimdAnyTrue(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    if (!CheckSimdCallArgs(f, call, 1, CheckArgIsSubtypeOf(opType)))
        return false;
    if (!f.writeSimdOp(opType, SimdOperation::Fn_anyTrue))
        return false;
    *type = Type::Int;
    return true;
}

static bool
CheckSimdCheck(FunctionValidator& f, ParseNode* call, Type* type)
{
    Type coerceTo;
    ParseNode* argNode;
    if (!IsCoercionCall(f.m(), call, &coerceTo, &argNode))
        return f.failf(call, "expected 1 argument in call to check");
    return CheckCoercionArg(f, argNode, coerceTo, type);
}

static bool
CheckSimdSplat(FunctionValidator& f, ParseNode* call, SimdType opType, Type* type)
{
    if (!CheckSimdCallArgs(f, call, 1, CheckSimdScalarArgs(opType)))
        return false;
    if (!f.writeSimdOp(opType, SimdOperation::Fn_splat))
        return false;
    *type = opType;
    return true;
}

static bool
CheckSimdOperationCall(FunctionValidator& f, ParseNode* call, const ModuleValidator::Global* global,
                       Type* type)
{
    MOZ_ASSERT(global->isSimdOperation());

    SimdType opType = global->simdOperationType();

    switch (SimdOperation op = global->simdOperation()) {
      case SimdOperation::Fn_check:
        return CheckSimdCheck(f, call, type);

#define _CASE(OP) case SimdOperation::Fn_##OP:
      FOREACH_SHIFT_SIMD_OP(_CASE)
        return CheckSimdBinaryShift(f, call, opType, op, type);

      FOREACH_COMP_SIMD_OP(_CASE)
        return CheckSimdBinaryComp(f, call, opType, op, type);

      FOREACH_NUMERIC_SIMD_BINOP(_CASE)
      FOREACH_FLOAT_SIMD_BINOP(_CASE)
      FOREACH_BITWISE_SIMD_BINOP(_CASE)
      FOREACH_SMINT_SIMD_BINOP(_CASE)
        return CheckSimdBinary(f, call, opType, op, type);
#undef _CASE

      case SimdOperation::Fn_extractLane:
        return CheckSimdExtractLane(f, call, opType, type);
      case SimdOperation::Fn_replaceLane:
        return CheckSimdReplaceLane(f, call, opType, type);

      case SimdOperation::Fn_fromInt8x16Bits:
        return CheckSimdCast(f, call, SimdType::Int8x16, opType, op, type);
      case SimdOperation::Fn_fromUint8x16Bits:
        return CheckSimdCast(f, call, SimdType::Uint8x16, opType, op, type);
      case SimdOperation::Fn_fromInt16x8Bits:
        return CheckSimdCast(f, call, SimdType::Int16x8, opType, op, type);
      case SimdOperation::Fn_fromUint16x8Bits:
        return CheckSimdCast(f, call, SimdType::Uint16x8, opType, op, type);
      case SimdOperation::Fn_fromInt32x4:
      case SimdOperation::Fn_fromInt32x4Bits:
        return CheckSimdCast(f, call, SimdType::Int32x4, opType, op, type);
      case SimdOperation::Fn_fromUint32x4:
      case SimdOperation::Fn_fromUint32x4Bits:
        return CheckSimdCast(f, call, SimdType::Uint32x4, opType, op, type);
      case SimdOperation::Fn_fromFloat32x4:
      case SimdOperation::Fn_fromFloat32x4Bits:
        return CheckSimdCast(f, call, SimdType::Float32x4, opType, op, type);

      case SimdOperation::Fn_abs:
      case SimdOperation::Fn_neg:
      case SimdOperation::Fn_not:
      case SimdOperation::Fn_sqrt:
      case SimdOperation::Fn_reciprocalApproximation:
      case SimdOperation::Fn_reciprocalSqrtApproximation:
        return CheckSimdUnary(f, call, opType, op, type);

      case SimdOperation::Fn_swizzle:
        return CheckSimdSwizzle(f, call, opType, type);
      case SimdOperation::Fn_shuffle:
        return CheckSimdShuffle(f, call, opType, type);

      case SimdOperation::Fn_load:
      case SimdOperation::Fn_load1:
      case SimdOperation::Fn_load2:
        return CheckSimdLoad(f, call, opType, op, type);
      case SimdOperation::Fn_store:
      case SimdOperation::Fn_store1:
      case SimdOperation::Fn_store2:
        return CheckSimdStore(f, call, opType, op, type);

      case SimdOperation::Fn_select:
        return CheckSimdSelect(f, call, opType, type);

      case SimdOperation::Fn_splat:
        return CheckSimdSplat(f, call, opType, type);

      case SimdOperation::Fn_allTrue:
        return CheckSimdAllTrue(f, call, opType, type);
      case SimdOperation::Fn_anyTrue:
        return CheckSimdAnyTrue(f, call, opType, type);

      case SimdOperation::Fn_load3:
      case SimdOperation::Fn_store3:
        return f.fail(call, "asm.js does not support 3-element SIMD loads or stores");

      case SimdOperation::Constructor:
        MOZ_CRASH("constructors are handled in CheckSimdCtorCall");
      case SimdOperation::Fn_fromFloat64x2Bits:
        MOZ_CRASH("NYI");
    }
    MOZ_CRASH("unexpected simd operation in CheckSimdOperationCall");
}

static bool
CheckSimdCtorCall(FunctionValidator& f, ParseNode* call, const ModuleValidator::Global* global,
                  Type* type)
{
    MOZ_ASSERT(call->isKind(ParseNodeKind::Call));

    SimdType simdType = global->simdCtorType();
    unsigned length = GetSimdLanes(simdType);
    if (!CheckSimdCallArgs(f, call, length, CheckSimdScalarArgs(simdType)))
        return false;

    if (!f.writeSimdOp(simdType, SimdOperation::Constructor))
        return false;

    *type = simdType;
    return true;
}

static bool
CheckUncoercedCall(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(ParseNodeKind::Call));

    const ModuleValidator::Global* global;
    if (IsCallToGlobal(f.m(), expr, &global)) {
        if (global->isMathFunction())
            return CheckMathBuiltinCall(f, expr, global->mathBuiltinFunction(), type);
        if (global->isAtomicsFunction())
            return CheckAtomicsBuiltinCall(f, expr, global->atomicsBuiltinFunction(), type);
        if (global->isSimdCtor())
            return CheckSimdCtorCall(f, expr, global, type);
        if (global->isSimdOperation())
            return CheckSimdOperationCall(f, expr, global, type);
    }

    return f.fail(expr, "all function calls must either be calls to standard lib math functions, "
                        "standard atomic functions, standard SIMD constructors or operations, "
                        "ignored (via f(); or comma-expression), coerced to signed (via f()|0), "
                        "coerced to float (via fround(f())) or coerced to double (via +f())");
}

static bool
CoerceResult(FunctionValidator& f, ParseNode* expr, Type expected, Type actual,
             Type* type)
{
    MOZ_ASSERT(expected.isCanonical());

    // At this point, the bytecode resembles this:
    //      | the thing we wanted to coerce | current position |>
    switch (expected.which()) {
      case Type::Void:
        if (!actual.isVoid()) {
            if (!f.encoder().writeOp(Op::Drop))
                return false;
        }
        break;
      case Type::Int:
        if (!actual.isIntish())
            return f.failf(expr, "%s is not a subtype of intish", actual.toChars());
        break;
      case Type::Float:
        if (!CheckFloatCoercionArg(f, expr, actual))
            return false;
        break;
      case Type::Double:
        if (actual.isMaybeDouble()) {
            // No conversion necessary.
        } else if (actual.isMaybeFloat()) {
            if (!f.encoder().writeOp(Op::F64PromoteF32))
                return false;
        } else if (actual.isSigned()) {
            if (!f.encoder().writeOp(Op::F64ConvertSI32))
                return false;
        } else if (actual.isUnsigned()) {
            if (!f.encoder().writeOp(Op::F64ConvertUI32))
                return false;
        } else {
            return f.failf(expr, "%s is not a subtype of double?, float?, signed or unsigned", actual.toChars());
        }
        break;
      default:
        MOZ_ASSERT(expected.isSimd(), "Incomplete switch");
        if (actual != expected)
            return f.failf(expr, "got type %s, expected %s", actual.toChars(), expected.toChars());
        break;
    }

    *type = Type::ret(expected);
    return true;
}

static bool
CheckCoercedMathBuiltinCall(FunctionValidator& f, ParseNode* callNode, AsmJSMathBuiltinFunction func,
                            Type ret, Type* type)
{
    Type actual;
    if (!CheckMathBuiltinCall(f, callNode, func, &actual))
        return false;
    return CoerceResult(f, callNode, ret, actual, type);
}

static bool
CheckCoercedSimdCall(FunctionValidator& f, ParseNode* call, const ModuleValidator::Global* global,
                     Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    Type actual;
    if (global->isSimdCtor()) {
        if (!CheckSimdCtorCall(f, call, global, &actual))
            return false;
        MOZ_ASSERT(actual.isSimd());
    } else {
        MOZ_ASSERT(global->isSimdOperation());
        if (!CheckSimdOperationCall(f, call, global, &actual))
            return false;
    }

    return CoerceResult(f, call, ret, actual, type);
}

static bool
CheckCoercedAtomicsBuiltinCall(FunctionValidator& f, ParseNode* callNode,
                               AsmJSAtomicsBuiltinFunction func, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    Type actual;
    if (!CheckAtomicsBuiltinCall(f, callNode, func, &actual))
        return false;
    return CoerceResult(f, callNode, ret, actual, type);
}

static bool
CheckCoercedCall(FunctionValidator& f, ParseNode* call, Type ret, Type* type)
{
    MOZ_ASSERT(ret.isCanonical());

    if (!CheckRecursionLimitDontReport(f.cx()))
        return f.m().failOverRecursed();

    if (IsNumericLiteral(f.m(), call)) {
        NumLit lit = ExtractNumericLiteral(f.m(), call);
        if (!f.writeConstExpr(lit))
            return false;
        return CoerceResult(f, call, ret, Type::lit(lit), type);
    }

    ParseNode* callee = CallCallee(call);

    if (callee->isKind(ParseNodeKind::Elem))
        return CheckFuncPtrCall(f, call, ret, type);

    if (!callee->isKind(ParseNodeKind::Name))
        return f.fail(callee, "unexpected callee expression type");

    PropertyName* calleeName = callee->name();

    if (const ModuleValidator::Global* global = f.lookupGlobal(calleeName)) {
        switch (global->which()) {
          case ModuleValidator::Global::FFI:
            return CheckFFICall(f, call, global->ffiIndex(), ret, type);
          case ModuleValidator::Global::MathBuiltinFunction:
            return CheckCoercedMathBuiltinCall(f, call, global->mathBuiltinFunction(), ret, type);
          case ModuleValidator::Global::AtomicsBuiltinFunction:
            return CheckCoercedAtomicsBuiltinCall(f, call, global->atomicsBuiltinFunction(), ret, type);
          case ModuleValidator::Global::ConstantLiteral:
          case ModuleValidator::Global::ConstantImport:
          case ModuleValidator::Global::Variable:
          case ModuleValidator::Global::Table:
          case ModuleValidator::Global::ArrayView:
          case ModuleValidator::Global::ArrayViewCtor:
            return f.failName(callee, "'%s' is not callable function", callee->name());
          case ModuleValidator::Global::SimdCtor:
          case ModuleValidator::Global::SimdOp:
            return CheckCoercedSimdCall(f, call, global, ret, type);
          case ModuleValidator::Global::Function:
            break;
        }
    }

    return CheckInternalCall(f, call, calleeName, ret, type);
}

static bool
CheckPos(FunctionValidator& f, ParseNode* pos, Type* type)
{
    MOZ_ASSERT(pos->isKind(ParseNodeKind::Pos));
    ParseNode* operand = UnaryKid(pos);

    if (operand->isKind(ParseNodeKind::Call))
        return CheckCoercedCall(f, operand, Type::Double, type);

    Type actual;
    if (!CheckExpr(f, operand, &actual))
        return false;

    return CoerceResult(f, operand, Type::Double, actual, type);
}

static bool
CheckNot(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(ParseNodeKind::Not));
    ParseNode* operand = UnaryKid(expr);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (!operandType.isInt())
        return f.failf(operand, "%s is not a subtype of int", operandType.toChars());

    *type = Type::Int;
    return f.encoder().writeOp(Op::I32Eqz);
}

static bool
CheckNeg(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(ParseNodeKind::Neg));
    ParseNode* operand = UnaryKid(expr);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (operandType.isInt()) {
        *type = Type::Intish;
        return f.encoder().writeOp(MozOp::I32Neg);
    }

    if (operandType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Neg);
    }

    if (operandType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Neg);
    }

    return f.failf(operand, "%s is not a subtype of int, float? or double?", operandType.toChars());
}

static bool
CheckCoerceToInt(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(ParseNodeKind::BitNot));
    ParseNode* operand = UnaryKid(expr);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (operandType.isMaybeDouble() || operandType.isMaybeFloat()) {
        *type = Type::Signed;
        Op opcode = operandType.isMaybeDouble() ? Op::I32TruncSF64 : Op::I32TruncSF32;
        return f.encoder().writeOp(opcode);
    }

    if (!operandType.isIntish())
        return f.failf(operand, "%s is not a subtype of double?, float? or intish", operandType.toChars());

    *type = Type::Signed;
    return true;
}

static bool
CheckBitNot(FunctionValidator& f, ParseNode* neg, Type* type)
{
    MOZ_ASSERT(neg->isKind(ParseNodeKind::BitNot));
    ParseNode* operand = UnaryKid(neg);

    if (operand->isKind(ParseNodeKind::BitNot))
        return CheckCoerceToInt(f, operand, type);

    Type operandType;
    if (!CheckExpr(f, operand, &operandType))
        return false;

    if (!operandType.isIntish())
        return f.failf(operand, "%s is not a subtype of intish", operandType.toChars());

    if (!f.encoder().writeOp(MozOp::I32BitNot))
        return false;

    *type = Type::Signed;
    return true;
}

static bool
CheckAsExprStatement(FunctionValidator& f, ParseNode* exprStmt);

static bool
CheckComma(FunctionValidator& f, ParseNode* comma, Type* type)
{
    MOZ_ASSERT(comma->isKind(ParseNodeKind::Comma));
    ParseNode* operands = ListHead(comma);

    // The block depth isn't taken into account here, because a comma list can't
    // contain breaks and continues and nested control flow structures.
    if (!f.encoder().writeOp(Op::Block))
        return false;

    size_t typeAt;
    if (!f.encoder().writePatchableFixedU7(&typeAt))
        return false;

    ParseNode* pn = operands;
    for (; NextNode(pn); pn = NextNode(pn)) {
        if (!CheckAsExprStatement(f, pn))
            return false;
    }

    if (!CheckExpr(f, pn, type))
        return false;

    f.encoder().patchFixedU7(typeAt, uint8_t(type->toWasmBlockSignatureType()));

    return f.encoder().writeOp(Op::End);
}

static bool
CheckConditional(FunctionValidator& f, ParseNode* ternary, Type* type)
{
    MOZ_ASSERT(ternary->isKind(ParseNodeKind::Conditional));

    ParseNode* cond = TernaryKid1(ternary);
    ParseNode* thenExpr = TernaryKid2(ternary);
    ParseNode* elseExpr = TernaryKid3(ternary);

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;

    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    size_t typeAt;
    if (!f.pushIf(&typeAt))
        return false;

    Type thenType;
    if (!CheckExpr(f, thenExpr, &thenType))
        return false;

    if (!f.switchToElse())
        return false;

    Type elseType;
    if (!CheckExpr(f, elseExpr, &elseType))
        return false;

    if (thenType.isInt() && elseType.isInt()) {
        *type = Type::Int;
    } else if (thenType.isDouble() && elseType.isDouble()) {
        *type = Type::Double;
    } else if (thenType.isFloat() && elseType.isFloat()) {
        *type = Type::Float;
    } else if (thenType.isSimd() && elseType == thenType) {
        *type = thenType;
    } else {
        return f.failf(ternary, "then/else branches of conditional must both produce int, float, "
                       "double or SIMD types, current types are %s and %s",
                       thenType.toChars(), elseType.toChars());
    }

    if (!f.popIf(typeAt, type->toWasmBlockSignatureType()))
        return false;

    return true;
}

static bool
IsValidIntMultiplyConstant(ModuleValidator& m, ParseNode* expr)
{
    if (!IsNumericLiteral(m, expr))
        return false;

    NumLit lit = ExtractNumericLiteral(m, expr);
    switch (lit.which()) {
      case NumLit::Fixnum:
      case NumLit::NegativeInt:
        if (abs(lit.toInt32()) < (1<<20))
            return true;
        return false;
      case NumLit::BigUnsigned:
      case NumLit::Double:
      case NumLit::Float:
      case NumLit::OutOfRangeInt:
      case NumLit::Int8x16:
      case NumLit::Uint8x16:
      case NumLit::Int16x8:
      case NumLit::Uint16x8:
      case NumLit::Int32x4:
      case NumLit::Uint32x4:
      case NumLit::Float32x4:
      case NumLit::Bool8x16:
      case NumLit::Bool16x8:
      case NumLit::Bool32x4:
        return false;
    }

    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad literal");
}

static bool
CheckMultiply(FunctionValidator& f, ParseNode* star, Type* type)
{
    MOZ_ASSERT(star->isKind(ParseNodeKind::Star));
    ParseNode* lhs = MultiplyLeft(star);
    ParseNode* rhs = MultiplyRight(star);

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (lhsType.isInt() && rhsType.isInt()) {
        if (!IsValidIntMultiplyConstant(f.m(), lhs) && !IsValidIntMultiplyConstant(f.m(), rhs))
            return f.fail(star, "one arg to int multiply must be a small (-2^20, 2^20) int literal");
        *type = Type::Intish;
        return f.encoder().writeOp(Op::I32Mul);
    }

    if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *type = Type::Double;
        return f.encoder().writeOp(Op::F64Mul);
    }

    if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        *type = Type::Floatish;
        return f.encoder().writeOp(Op::F32Mul);
    }

    return f.fail(star, "multiply operands must be both int, both double? or both float?");
}

static bool
CheckAddOrSub(FunctionValidator& f, ParseNode* expr, Type* type, unsigned* numAddOrSubOut = nullptr)
{
    if (!CheckRecursionLimitDontReport(f.cx()))
        return f.m().failOverRecursed();

    MOZ_ASSERT(expr->isKind(ParseNodeKind::Add) || expr->isKind(ParseNodeKind::Sub));
    ParseNode* lhs = AddSubLeft(expr);
    ParseNode* rhs = AddSubRight(expr);

    Type lhsType, rhsType;
    unsigned lhsNumAddOrSub, rhsNumAddOrSub;

    if (lhs->isKind(ParseNodeKind::Add) || lhs->isKind(ParseNodeKind::Sub)) {
        if (!CheckAddOrSub(f, lhs, &lhsType, &lhsNumAddOrSub))
            return false;
        if (lhsType == Type::Intish)
            lhsType = Type::Int;
    } else {
        if (!CheckExpr(f, lhs, &lhsType))
            return false;
        lhsNumAddOrSub = 0;
    }

    if (rhs->isKind(ParseNodeKind::Add) || rhs->isKind(ParseNodeKind::Sub)) {
        if (!CheckAddOrSub(f, rhs, &rhsType, &rhsNumAddOrSub))
            return false;
        if (rhsType == Type::Intish)
            rhsType = Type::Int;
    } else {
        if (!CheckExpr(f, rhs, &rhsType))
            return false;
        rhsNumAddOrSub = 0;
    }

    unsigned numAddOrSub = lhsNumAddOrSub + rhsNumAddOrSub + 1;
    if (numAddOrSub > (1<<20))
        return f.fail(expr, "too many + or - without intervening coercion");

    if (lhsType.isInt() && rhsType.isInt()) {
        if (!f.encoder().writeOp(expr->isKind(ParseNodeKind::Add) ? Op::I32Add : Op::I32Sub))
            return false;
        *type = Type::Intish;
    } else if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        if (!f.encoder().writeOp(expr->isKind(ParseNodeKind::Add) ? Op::F64Add : Op::F64Sub))
            return false;
        *type = Type::Double;
    } else if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        if (!f.encoder().writeOp(expr->isKind(ParseNodeKind::Add) ? Op::F32Add : Op::F32Sub))
            return false;
        *type = Type::Floatish;
    } else {
        return f.failf(expr, "operands to + or - must both be int, float? or double?, got %s and %s",
                       lhsType.toChars(), rhsType.toChars());
    }

    if (numAddOrSubOut)
        *numAddOrSubOut = numAddOrSub;
    return true;
}

static bool
CheckDivOrMod(FunctionValidator& f, ParseNode* expr, Type* type)
{
    MOZ_ASSERT(expr->isKind(ParseNodeKind::Div) || expr->isKind(ParseNodeKind::Mod));

    ParseNode* lhs = DivOrModLeft(expr);
    ParseNode* rhs = DivOrModRight(expr);

    Type lhsType, rhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *type = Type::Double;
        if (expr->isKind(ParseNodeKind::Div))
            return f.encoder().writeOp(Op::F64Div);
        return f.encoder().writeOp(MozOp::F64Mod);
    }

    if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        *type = Type::Floatish;
        if (expr->isKind(ParseNodeKind::Div))
            return f.encoder().writeOp(Op::F32Div);
        else
            return f.fail(expr, "modulo cannot receive float arguments");
    }

    if (lhsType.isSigned() && rhsType.isSigned()) {
        *type = Type::Intish;
        return f.encoder().writeOp(expr->isKind(ParseNodeKind::Div) ? Op::I32DivS : Op::I32RemS);
    }

    if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
        *type = Type::Intish;
        return f.encoder().writeOp(expr->isKind(ParseNodeKind::Div) ? Op::I32DivU : Op::I32RemU);
    }

    return f.failf(expr, "arguments to / or %% must both be double?, float?, signed, or unsigned; "
                   "%s and %s are given", lhsType.toChars(), rhsType.toChars());
}

static bool
CheckComparison(FunctionValidator& f, ParseNode* comp, Type* type)
{
    MOZ_ASSERT(comp->isKind(ParseNodeKind::Lt) ||
               comp->isKind(ParseNodeKind::Le) ||
               comp->isKind(ParseNodeKind::Gt) ||
               comp->isKind(ParseNodeKind::Ge) ||
               comp->isKind(ParseNodeKind::Eq) ||
               comp->isKind(ParseNodeKind::Ne));

    ParseNode* lhs = ComparisonLeft(comp);
    ParseNode* rhs = ComparisonRight(comp);

    Type lhsType, rhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (!(lhsType.isSigned() && rhsType.isSigned()) &&
        !(lhsType.isUnsigned() && rhsType.isUnsigned()) &&
        !(lhsType.isDouble() && rhsType.isDouble()) &&
        !(lhsType.isFloat() && rhsType.isFloat()))
    {
        return f.failf(comp, "arguments to a comparison must both be signed, unsigned, floats or doubles; "
                       "%s and %s are given", lhsType.toChars(), rhsType.toChars());
    }

    Op stmt;
    if (lhsType.isSigned() && rhsType.isSigned()) {
        switch (comp->getKind()) {
          case ParseNodeKind::Eq: stmt = Op::I32Eq;  break;
          case ParseNodeKind::Ne: stmt = Op::I32Ne;  break;
          case ParseNodeKind::Lt: stmt = Op::I32LtS; break;
          case ParseNodeKind::Le: stmt = Op::I32LeS; break;
          case ParseNodeKind::Gt: stmt = Op::I32GtS; break;
          case ParseNodeKind::Ge: stmt = Op::I32GeS; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
        switch (comp->getKind()) {
          case ParseNodeKind::Eq: stmt = Op::I32Eq;  break;
          case ParseNodeKind::Ne: stmt = Op::I32Ne;  break;
          case ParseNodeKind::Lt: stmt = Op::I32LtU; break;
          case ParseNodeKind::Le: stmt = Op::I32LeU; break;
          case ParseNodeKind::Gt: stmt = Op::I32GtU; break;
          case ParseNodeKind::Ge: stmt = Op::I32GeU; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else if (lhsType.isDouble()) {
        switch (comp->getKind()) {
          case ParseNodeKind::Eq: stmt = Op::F64Eq; break;
          case ParseNodeKind::Ne: stmt = Op::F64Ne; break;
          case ParseNodeKind::Lt: stmt = Op::F64Lt; break;
          case ParseNodeKind::Le: stmt = Op::F64Le; break;
          case ParseNodeKind::Gt: stmt = Op::F64Gt; break;
          case ParseNodeKind::Ge: stmt = Op::F64Ge; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else if (lhsType.isFloat()) {
        switch (comp->getKind()) {
          case ParseNodeKind::Eq: stmt = Op::F32Eq; break;
          case ParseNodeKind::Ne: stmt = Op::F32Ne; break;
          case ParseNodeKind::Lt: stmt = Op::F32Lt; break;
          case ParseNodeKind::Le: stmt = Op::F32Le; break;
          case ParseNodeKind::Gt: stmt = Op::F32Gt; break;
          case ParseNodeKind::Ge: stmt = Op::F32Ge; break;
          default: MOZ_CRASH("unexpected comparison op");
        }
    } else {
        MOZ_CRASH("unexpected type");
    }

    *type = Type::Int;
    return f.encoder().writeOp(stmt);
}

static bool
CheckBitwise(FunctionValidator& f, ParseNode* bitwise, Type* type)
{
    ParseNode* lhs = BitwiseLeft(bitwise);
    ParseNode* rhs = BitwiseRight(bitwise);

    int32_t identityElement;
    bool onlyOnRight;
    switch (bitwise->getKind()) {
      case ParseNodeKind::BitOr:
        identityElement = 0;
        onlyOnRight = false;
        *type = Type::Signed;
        break;
      case ParseNodeKind::BitAnd:
        identityElement = -1;
        onlyOnRight = false;
        *type = Type::Signed;
        break;
      case ParseNodeKind::BitXor:
        identityElement = 0;
        onlyOnRight = false;
        *type = Type::Signed;
        break;
      case ParseNodeKind::Lsh:
        identityElement = 0;
        onlyOnRight = true;
        *type = Type::Signed;
        break;
      case ParseNodeKind::Rsh:
        identityElement = 0;
        onlyOnRight = true;
        *type = Type::Signed;
        break;
      case ParseNodeKind::Ursh:
        identityElement = 0;
        onlyOnRight = true;
        *type = Type::Unsigned;
        break;
      default: MOZ_CRASH("not a bitwise op");
    }

    uint32_t i;
    if (!onlyOnRight && IsLiteralInt(f.m(), lhs, &i) && i == uint32_t(identityElement)) {
        Type rhsType;
        if (!CheckExpr(f, rhs, &rhsType))
            return false;
        if (!rhsType.isIntish())
            return f.failf(bitwise, "%s is not a subtype of intish", rhsType.toChars());
        return true;
    }

    if (IsLiteralInt(f.m(), rhs, &i) && i == uint32_t(identityElement)) {
        if (bitwise->isKind(ParseNodeKind::BitOr) && lhs->isKind(ParseNodeKind::Call))
            return CheckCoercedCall(f, lhs, Type::Int, type);

        Type lhsType;
        if (!CheckExpr(f, lhs, &lhsType))
            return false;
        if (!lhsType.isIntish())
            return f.failf(bitwise, "%s is not a subtype of intish", lhsType.toChars());
        return true;
    }

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType))
        return false;

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType))
        return false;

    if (!lhsType.isIntish())
        return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
    if (!rhsType.isIntish())
        return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());

    switch (bitwise->getKind()) {
      case ParseNodeKind::BitOr:  if (!f.encoder().writeOp(Op::I32Or))   return false; break;
      case ParseNodeKind::BitAnd: if (!f.encoder().writeOp(Op::I32And))  return false; break;
      case ParseNodeKind::BitXor: if (!f.encoder().writeOp(Op::I32Xor))  return false; break;
      case ParseNodeKind::Lsh:    if (!f.encoder().writeOp(Op::I32Shl))  return false; break;
      case ParseNodeKind::Rsh:    if (!f.encoder().writeOp(Op::I32ShrS)) return false; break;
      case ParseNodeKind::Ursh:   if (!f.encoder().writeOp(Op::I32ShrU)) return false; break;
      default: MOZ_CRASH("not a bitwise op");
    }

    return true;
}

static bool
CheckExpr(FunctionValidator& f, ParseNode* expr, Type* type)
{
    if (!CheckRecursionLimitDontReport(f.cx()))
        return f.m().failOverRecursed();

    if (IsNumericLiteral(f.m(), expr))
        return CheckNumericLiteral(f, expr, type);

    switch (expr->getKind()) {
      case ParseNodeKind::Name:        return CheckVarRef(f, expr, type);
      case ParseNodeKind::Elem:        return CheckLoadArray(f, expr, type);
      case ParseNodeKind::Assign:      return CheckAssign(f, expr, type);
      case ParseNodeKind::Pos:         return CheckPos(f, expr, type);
      case ParseNodeKind::Not:         return CheckNot(f, expr, type);
      case ParseNodeKind::Neg:         return CheckNeg(f, expr, type);
      case ParseNodeKind::BitNot:      return CheckBitNot(f, expr, type);
      case ParseNodeKind::Comma:       return CheckComma(f, expr, type);
      case ParseNodeKind::Conditional: return CheckConditional(f, expr, type);
      case ParseNodeKind::Star:        return CheckMultiply(f, expr, type);
      case ParseNodeKind::Call:        return CheckUncoercedCall(f, expr, type);

      case ParseNodeKind::Add:
      case ParseNodeKind::Sub:         return CheckAddOrSub(f, expr, type);

      case ParseNodeKind::Div:
      case ParseNodeKind::Mod:         return CheckDivOrMod(f, expr, type);

      case ParseNodeKind::Lt:
      case ParseNodeKind::Le:
      case ParseNodeKind::Gt:
      case ParseNodeKind::Ge:
      case ParseNodeKind::Eq:
      case ParseNodeKind::Ne:          return CheckComparison(f, expr, type);

      case ParseNodeKind::BitOr:
      case ParseNodeKind::BitAnd:
      case ParseNodeKind::BitXor:
      case ParseNodeKind::Lsh:
      case ParseNodeKind::Rsh:
      case ParseNodeKind::Ursh:        return CheckBitwise(f, expr, type);

      default:;
    }

    return f.fail(expr, "unsupported expression");
}

static bool
CheckStatement(FunctionValidator& f, ParseNode* stmt);

static bool
CheckAsExprStatement(FunctionValidator& f, ParseNode* expr)
{
    if (expr->isKind(ParseNodeKind::Call)) {
        Type ignored;
        return CheckCoercedCall(f, expr, Type::Void, &ignored);
    }

    Type resultType;
    if (!CheckExpr(f, expr, &resultType))
        return false;

    if (!resultType.isVoid()) {
        if (!f.encoder().writeOp(Op::Drop))
            return false;
    }

    return true;
}

static bool
CheckExprStatement(FunctionValidator& f, ParseNode* exprStmt)
{
    MOZ_ASSERT(exprStmt->isKind(ParseNodeKind::ExpressionStatement));
    return CheckAsExprStatement(f, UnaryKid(exprStmt));
}

static bool
CheckLoopConditionOnEntry(FunctionValidator& f, ParseNode* cond)
{
    uint32_t maybeLit;
    if (IsLiteralInt(f.m(), cond, &maybeLit) && maybeLit)
        return true;

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    if (!f.encoder().writeOp(Op::I32Eqz))
        return false;

    // brIf (i32.eqz $f) $out
    if (!f.writeBreakIf())
        return false;

    return true;
}

static bool
CheckWhile(FunctionValidator& f, ParseNode* whileStmt, const NameVector* labels = nullptr)
{
    MOZ_ASSERT(whileStmt->isKind(ParseNodeKind::While));
    ParseNode* cond = BinaryLeft(whileStmt);
    ParseNode* body = BinaryRight(whileStmt);

    // A while loop `while(#cond) #body` is equivalent to:
    // (block $after_loop
    //    (loop $top
    //       (brIf $after_loop (i32.eq 0 #cond))
    //       #body
    //       (br $top)
    //    )
    // )
    if (labels && !f.addLabels(*labels, 0, 1))
        return false;

    if (!f.pushLoop())
        return false;

    if (!CheckLoopConditionOnEntry(f, cond))
        return false;
    if (!CheckStatement(f, body))
        return false;
    if (!f.writeContinue())
        return false;

    if (!f.popLoop())
        return false;
    if (labels)
        f.removeLabels(*labels);
    return true;
}

static bool
CheckFor(FunctionValidator& f, ParseNode* forStmt, const NameVector* labels = nullptr)
{
    MOZ_ASSERT(forStmt->isKind(ParseNodeKind::For));
    ParseNode* forHead = BinaryLeft(forStmt);
    ParseNode* body = BinaryRight(forStmt);

    if (!forHead->isKind(ParseNodeKind::ForHead))
        return f.fail(forHead, "unsupported for-loop statement");

    ParseNode* maybeInit = TernaryKid1(forHead);
    ParseNode* maybeCond = TernaryKid2(forHead);
    ParseNode* maybeInc = TernaryKid3(forHead);

    // A for-loop `for (#init; #cond; #inc) #body` is equivalent to:
    // (block                                               // depth X
    //   (#init)
    //   (block $after_loop                                 // depth X+1 (block)
    //     (loop $loop_top                                  // depth X+2 (loop)
    //       (brIf $after (eq 0 #cond))
    //       (block $after_body #body)                      // depth X+3
    //       #inc
    //       (br $loop_top)
    //     )
    //   )
    // )
    // A break in the body should break out to $after_loop, i.e. depth + 1.
    // A continue in the body should break out to $after_body, i.e. depth + 3.
    if (labels && !f.addLabels(*labels, 1, 3))
        return false;

    if (!f.pushUnbreakableBlock())
        return false;

    if (maybeInit && !CheckAsExprStatement(f, maybeInit))
        return false;

    {
        if (!f.pushLoop())
            return false;

        if (maybeCond && !CheckLoopConditionOnEntry(f, maybeCond))
            return false;

        {
            // Continuing in the body should just break out to the increment.
            if (!f.pushContinuableBlock())
                return false;
            if (!CheckStatement(f, body))
                return false;
            if (!f.popContinuableBlock())
                return false;
        }

        if (maybeInc && !CheckAsExprStatement(f, maybeInc))
            return false;

        if (!f.writeContinue())
            return false;
        if (!f.popLoop())
            return false;
    }

    if (!f.popUnbreakableBlock())
        return false;

    if (labels)
        f.removeLabels(*labels);

    return true;
}

static bool
CheckDoWhile(FunctionValidator& f, ParseNode* whileStmt, const NameVector* labels = nullptr)
{
    MOZ_ASSERT(whileStmt->isKind(ParseNodeKind::DoWhile));
    ParseNode* body = BinaryLeft(whileStmt);
    ParseNode* cond = BinaryRight(whileStmt);

    // A do-while loop `do { #body } while (#cond)` is equivalent to:
    // (block $after_loop           // depth X
    //   (loop $top                 // depth X+1
    //     (block #body)            // depth X+2
    //     (brIf #cond $top)
    //   )
    // )
    // A break should break out of the entire loop, i.e. at depth 0.
    // A continue should break out to the condition, i.e. at depth 2.
    if (labels && !f.addLabels(*labels, 0, 2))
        return false;

    if (!f.pushLoop())
        return false;

    {
        // An unlabeled continue in the body should break out to the condition.
        if (!f.pushContinuableBlock())
            return false;
        if (!CheckStatement(f, body))
            return false;
        if (!f.popContinuableBlock())
            return false;
    }

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    if (!f.writeContinueIf())
        return false;

    if (!f.popLoop())
        return false;
    if (labels)
        f.removeLabels(*labels);
    return true;
}

static bool CheckStatementList(FunctionValidator& f, ParseNode*, const NameVector* = nullptr);

static bool
CheckLabel(FunctionValidator& f, ParseNode* labeledStmt)
{
    MOZ_ASSERT(labeledStmt->isKind(ParseNodeKind::Label));

    NameVector labels;
    ParseNode* innermost = labeledStmt;
    do {
        if (!labels.append(LabeledStatementLabel(innermost)))
            return false;
        innermost = LabeledStatementStatement(innermost);
    } while (innermost->getKind() == ParseNodeKind::Label);

    switch (innermost->getKind()) {
      case ParseNodeKind::For:
        return CheckFor(f, innermost, &labels);
      case ParseNodeKind::DoWhile:
        return CheckDoWhile(f, innermost, &labels);
      case ParseNodeKind::While:
        return CheckWhile(f, innermost, &labels);
      case ParseNodeKind::StatementList:
        return CheckStatementList(f, innermost, &labels);
      default:
        break;
    }

    if (!f.pushUnbreakableBlock(&labels))
        return false;

    if (!CheckStatement(f, innermost))
        return false;

    if (!f.popUnbreakableBlock(&labels))
        return false;
    return true;
}

static bool
CheckIf(FunctionValidator& f, ParseNode* ifStmt)
{
    uint32_t numIfEnd = 1;

  recurse:
    MOZ_ASSERT(ifStmt->isKind(ParseNodeKind::If));
    ParseNode* cond = TernaryKid1(ifStmt);
    ParseNode* thenStmt = TernaryKid2(ifStmt);
    ParseNode* elseStmt = TernaryKid3(ifStmt);

    Type condType;
    if (!CheckExpr(f, cond, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    size_t typeAt;
    if (!f.pushIf(&typeAt))
        return false;

    f.setIfType(typeAt, ExprType::Void);

    if (!CheckStatement(f, thenStmt))
        return false;

    if (elseStmt) {
        if (!f.switchToElse())
            return false;

        if (elseStmt->isKind(ParseNodeKind::If)) {
            ifStmt = elseStmt;
            if (numIfEnd++ == UINT32_MAX)
                return false;
            goto recurse;
        }

        if (!CheckStatement(f, elseStmt))
            return false;
    }

    for (uint32_t i = 0; i != numIfEnd; ++i) {
        if (!f.popIf())
            return false;
    }

    return true;
}

static bool
CheckCaseExpr(FunctionValidator& f, ParseNode* caseExpr, int32_t* value)
{
    if (!IsNumericLiteral(f.m(), caseExpr))
        return f.fail(caseExpr, "switch case expression must be an integer literal");

    NumLit lit = ExtractNumericLiteral(f.m(), caseExpr);
    switch (lit.which()) {
      case NumLit::Fixnum:
      case NumLit::NegativeInt:
        *value = lit.toInt32();
        break;
      case NumLit::OutOfRangeInt:
      case NumLit::BigUnsigned:
        return f.fail(caseExpr, "switch case expression out of integer range");
      case NumLit::Double:
      case NumLit::Float:
      case NumLit::Int8x16:
      case NumLit::Uint8x16:
      case NumLit::Int16x8:
      case NumLit::Uint16x8:
      case NumLit::Int32x4:
      case NumLit::Uint32x4:
      case NumLit::Float32x4:
      case NumLit::Bool8x16:
      case NumLit::Bool16x8:
      case NumLit::Bool32x4:
        return f.fail(caseExpr, "switch case expression must be an integer literal");
    }

    return true;
}

static bool
CheckDefaultAtEnd(FunctionValidator& f, ParseNode* stmt)
{
    for (; stmt; stmt = NextNode(stmt)) {
        if (IsDefaultCase(stmt) && NextNode(stmt) != nullptr)
            return f.fail(stmt, "default label must be at the end");
    }

    return true;
}

static bool
CheckSwitchRange(FunctionValidator& f, ParseNode* stmt, int32_t* low, int32_t* high,
                 uint32_t* tableLength)
{
    if (IsDefaultCase(stmt)) {
        *low = 0;
        *high = -1;
        *tableLength = 0;
        return true;
    }

    int32_t i = 0;
    if (!CheckCaseExpr(f, CaseExpr(stmt), &i))
        return false;

    *low = *high = i;

    ParseNode* initialStmt = stmt;
    for (stmt = NextNode(stmt); stmt && !IsDefaultCase(stmt); stmt = NextNode(stmt)) {
        int32_t i = 0;
        if (!CheckCaseExpr(f, CaseExpr(stmt), &i))
            return false;

        *low = Min(*low, i);
        *high = Max(*high, i);
    }

    int64_t i64 = (int64_t(*high) - int64_t(*low)) + 1;
    if (i64 > MaxBrTableElems)
        return f.fail(initialStmt, "all switch statements generate tables; this table would be too big");

    *tableLength = uint32_t(i64);
    return true;
}

static bool
CheckSwitchExpr(FunctionValidator& f, ParseNode* switchExpr)
{
    Type exprType;
    if (!CheckExpr(f, switchExpr, &exprType))
        return false;
    if (!exprType.isSigned())
        return f.failf(switchExpr, "%s is not a subtype of signed", exprType.toChars());
    return true;
}

// A switch will be constructed as:
// - the default block wrapping all the other blocks, to be able to break
// out of the switch with an unlabeled break statement. It has two statements
// (an inner block and the default expr). asm.js rules require default to be at
// the end, so the default block always encloses all the cases blocks.
// - one block per case between low and high; undefined cases just jump to the
// default case. Each of these blocks contain two statements: the next case's
// block and the possibly empty statement list comprising the case body. The
// last block pushed is the first case so the (relative) branch target therefore
// matches the sequential order of cases.
// - one block for the br_table, so that the first break goes to the first
// case's block.
static bool
CheckSwitch(FunctionValidator& f, ParseNode* switchStmt)
{
    MOZ_ASSERT(switchStmt->isKind(ParseNodeKind::Switch));

    ParseNode* switchExpr = BinaryLeft(switchStmt);
    ParseNode* switchBody = BinaryRight(switchStmt);

    if (switchBody->isKind(ParseNodeKind::LexicalScope)) {
        if (!switchBody->isEmptyScope())
            return f.fail(switchBody, "switch body may not contain lexical declarations");
        switchBody = switchBody->scopeBody();
    }

    ParseNode* stmt = ListHead(switchBody);
    if (!stmt) {
        if (!CheckSwitchExpr(f, switchExpr))
            return false;
        if (!f.encoder().writeOp(Op::Drop))
            return false;
        return true;
    }

    if (!CheckDefaultAtEnd(f, stmt))
        return false;

    int32_t low = 0, high = 0;
    uint32_t tableLength = 0;
    if (!CheckSwitchRange(f, stmt, &low, &high, &tableLength))
        return false;

    static const uint32_t CASE_NOT_DEFINED = UINT32_MAX;

    Uint32Vector caseDepths;
    if (!caseDepths.appendN(CASE_NOT_DEFINED, tableLength))
        return false;

    uint32_t numCases = 0;
    for (ParseNode* s = stmt; s && !IsDefaultCase(s); s = NextNode(s)) {
        int32_t caseValue = ExtractNumericLiteral(f.m(), CaseExpr(s)).toInt32();

        MOZ_ASSERT(caseValue >= low);
        unsigned i = caseValue - low;
        if (caseDepths[i] != CASE_NOT_DEFINED)
            return f.fail(s, "no duplicate case labels");

        MOZ_ASSERT(numCases != CASE_NOT_DEFINED);
        caseDepths[i] = numCases++;
    }

    // Open the wrapping breakable default block.
    if (!f.pushBreakableBlock())
        return false;

    // Open all the case blocks.
    for (uint32_t i = 0; i < numCases; i++) {
        if (!f.pushUnbreakableBlock())
            return false;
    }

    // Open the br_table block.
    if (!f.pushUnbreakableBlock())
        return false;

    // The default block is the last one.
    uint32_t defaultDepth = numCases;

    // Subtract lowest case value, so that all the cases start from 0.
    if (low) {
        if (!CheckSwitchExpr(f, switchExpr))
            return false;
        if (!f.writeInt32Lit(low))
            return false;
        if (!f.encoder().writeOp(Op::I32Sub))
            return false;
    } else {
        if (!CheckSwitchExpr(f, switchExpr))
            return false;
    }

    // Start the br_table block.
    if (!f.encoder().writeOp(Op::BrTable))
        return false;

    // Write the number of cases (tableLength - 1 + 1 (default)).
    // Write the number of cases (tableLength - 1 + 1 (default)).
    if (!f.encoder().writeVarU32(tableLength))
        return false;

    // Each case value describes the relative depth to the actual block. When
    // a case is not explicitly defined, it goes to the default.
    for (size_t i = 0; i < tableLength; i++) {
        uint32_t target = caseDepths[i] == CASE_NOT_DEFINED ? defaultDepth : caseDepths[i];
        if (!f.encoder().writeVarU32(target))
            return false;
    }

    // Write the default depth.
    if (!f.encoder().writeVarU32(defaultDepth))
        return false;

    // Our br_table is done. Close its block, write the cases down in order.
    if (!f.popUnbreakableBlock())
        return false;

    for (; stmt && !IsDefaultCase(stmt); stmt = NextNode(stmt)) {
        if (!CheckStatement(f, CaseBody(stmt)))
            return false;
        if (!f.popUnbreakableBlock())
            return false;
    }

    // Write the default block.
    if (stmt && IsDefaultCase(stmt)) {
        if (!CheckStatement(f, CaseBody(stmt)))
            return false;
    }

    // Close the wrapping block.
    if (!f.popBreakableBlock())
        return false;
    return true;
}

static bool
CheckReturnType(FunctionValidator& f, ParseNode* usepn, Type ret)
{
    if (!f.hasAlreadyReturned()) {
        f.setReturnedType(ret.canonicalToExprType());
        return true;
    }

    if (f.returnedType() != ret.canonicalToExprType()) {
        return f.failf(usepn, "%s incompatible with previous return of type %s",
                       Type::ret(ret).toChars(), ToCString(f.returnedType()));
    }

    return true;
}

static bool
CheckReturn(FunctionValidator& f, ParseNode* returnStmt)
{
    ParseNode* expr = ReturnExpr(returnStmt);

    if (!expr) {
        if (!CheckReturnType(f, returnStmt, Type::Void))
            return false;
    } else {
        Type type;
        if (!CheckExpr(f, expr, &type))
            return false;

        if (!type.isReturnType())
            return f.failf(expr, "%s is not a valid return type", type.toChars());

        if (!CheckReturnType(f, expr, Type::canonicalize(type)))
            return false;
    }

    if (!f.encoder().writeOp(Op::Return))
        return false;

    return true;
}

static bool
CheckStatementList(FunctionValidator& f, ParseNode* stmtList, const NameVector* labels /*= nullptr */)
{
    MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));

    if (!f.pushUnbreakableBlock(labels))
        return false;

    for (ParseNode* stmt = ListHead(stmtList); stmt; stmt = NextNode(stmt)) {
        if (!CheckStatement(f, stmt))
            return false;
    }

    if (!f.popUnbreakableBlock(labels))
        return false;
    return true;
}

static bool
CheckLexicalScope(FunctionValidator& f, ParseNode* lexicalScope)
{
    MOZ_ASSERT(lexicalScope->isKind(ParseNodeKind::LexicalScope));

    if (!lexicalScope->isEmptyScope())
        return f.fail(lexicalScope, "cannot have 'let' or 'const' declarations");

    return CheckStatement(f, lexicalScope->scopeBody());
}

static bool
CheckBreakOrContinue(FunctionValidator& f, bool isBreak, ParseNode* stmt)
{
    if (PropertyName* maybeLabel = LoopControlMaybeLabel(stmt))
        return f.writeLabeledBreakOrContinue(maybeLabel, isBreak);
    return f.writeUnlabeledBreakOrContinue(isBreak);
}

static bool
CheckStatement(FunctionValidator& f, ParseNode* stmt)
{
    if (!CheckRecursionLimitDontReport(f.cx()))
        return f.m().failOverRecursed();

    switch (stmt->getKind()) {
      case ParseNodeKind::EmptyStatement:       return true;
      case ParseNodeKind::ExpressionStatement:  return CheckExprStatement(f, stmt);
      case ParseNodeKind::While:                return CheckWhile(f, stmt);
      case ParseNodeKind::For:                  return CheckFor(f, stmt);
      case ParseNodeKind::DoWhile:              return CheckDoWhile(f, stmt);
      case ParseNodeKind::Label:                return CheckLabel(f, stmt);
      case ParseNodeKind::If:                   return CheckIf(f, stmt);
      case ParseNodeKind::Switch:               return CheckSwitch(f, stmt);
      case ParseNodeKind::Return:               return CheckReturn(f, stmt);
      case ParseNodeKind::StatementList:        return CheckStatementList(f, stmt);
      case ParseNodeKind::Break:                return CheckBreakOrContinue(f, true, stmt);
      case ParseNodeKind::Continue:             return CheckBreakOrContinue(f, false, stmt);
      case ParseNodeKind::LexicalScope:         return CheckLexicalScope(f, stmt);
      default:;
    }

    return f.fail(stmt, "unexpected statement kind");
}

static bool
ParseFunction(ModuleValidator& m, ParseNode** fnOut, unsigned* line)
{
    auto& tokenStream = m.tokenStream();

    tokenStream.consumeKnownToken(TokenKind::Function, TokenStreamShared::Operand);

    auto& anyChars = tokenStream.anyCharsAccess();
    uint32_t toStringStart = anyChars.currentToken().pos.begin;
    *line = anyChars.srcCoords.lineNum(anyChars.currentToken().pos.end);

    TokenKind tk;
    if (!tokenStream.getToken(&tk, TokenStreamShared::Operand))
        return false;
    if (tk == TokenKind::Mul)
        return m.failCurrentOffset("unexpected generator function");
    if (!TokenKindIsPossibleIdentifier(tk))
        return false;  // The regular parser will throw a SyntaxError, no need to m.fail.

    RootedPropertyName name(m.cx(), m.parser().bindingIdentifier(YieldIsName));
    if (!name)
        return false;

    ParseNode* fn = m.parser().handler.newFunctionStatement(m.parser().pos());
    if (!fn)
        return false;

    RootedFunction& fun = m.dummyFunction();
    fun->setAtom(name);
    fun->setArgCount(0);

    ParseContext* outerpc = m.parser().pc;
    Directives directives(outerpc);
    FunctionBox* funbox = m.parser().newFunctionBox(fn, fun, toStringStart, directives,
                                                    GeneratorKind::NotGenerator,
                                                    FunctionAsyncKind::SyncFunction);
    if (!funbox)
        return false;
    funbox->initWithEnclosingParseContext(outerpc, frontend::Statement);

    Directives newDirectives = directives;
    SourceParseContext funpc(&m.parser(), funbox, &newDirectives);
    if (!funpc.init())
        return false;

    if (!m.parser().functionFormalParametersAndBody(InAllowed, YieldIsName, &fn, Statement)) {
        if (anyChars.hadError() || directives == newDirectives)
            return false;

        return m.fail(fn, "encountered new directive in function");
    }

    MOZ_ASSERT(!anyChars.hadError());
    MOZ_ASSERT(directives == newDirectives);

    *fnOut = fn;
    return true;
}

static bool
CheckFunction(ModuleValidator& m)
{
    // asm.js modules can be quite large when represented as parse trees so pop
    // the backing LifoAlloc after parsing/compiling each function.
    AsmJSParser::Mark mark = m.parser().mark();

    ParseNode* fn = nullptr;
    unsigned line = 0;
    if (!ParseFunction(m, &fn, &line))
        return false;

    if (!CheckFunctionHead(m, fn))
        return false;

    FunctionValidator f(m, fn);
    if (!f.init())
        return m.fail(fn, "internal compiler failure (probably out of memory)");

    ParseNode* stmtIter = ListHead(FunctionStatementList(fn));

    if (!CheckProcessingDirectives(m, &stmtIter))
        return false;

    ValTypeVector args;
    if (!CheckArguments(f, &stmtIter, &args))
        return false;

    if (!CheckVariables(f, &stmtIter))
        return false;

    ParseNode* lastNonEmptyStmt = nullptr;
    for (; stmtIter; stmtIter = NextNonEmptyStatement(stmtIter)) {
        lastNonEmptyStmt = stmtIter;
        if (!CheckStatement(f, stmtIter))
            return false;
    }

    if (!CheckFinalReturn(f, lastNonEmptyStmt))
        return false;

    ModuleValidator::Func* func = nullptr;
    if (!CheckFunctionSignature(m, fn, Sig(Move(args), f.returnedType()), FunctionName(fn), &func))
        return false;

    if (func->defined())
        return m.failName(fn, "function '%s' already defined", FunctionName(fn));

    f.define(func, line);

    // Release the parser's lifo memory only after the last use of a parse node.
    m.parser().release(mark);
    return true;
}

static bool
CheckAllFunctionsDefined(ModuleValidator& m)
{
    for (unsigned i = 0; i < m.numFuncDefs(); i++) {
        const ModuleValidator::Func& f = m.funcDef(i);
        if (!f.defined())
            return m.failNameOffset(f.firstUse(), "missing definition of function %s", f.name());
    }

    return true;
}

static bool
CheckFunctions(ModuleValidator& m)
{
    while (true) {
        TokenKind tk;
        if (!PeekToken(m.parser(), &tk))
            return false;

        if (tk != TokenKind::Function)
            break;

        if (!CheckFunction(m))
            return false;
    }

    return CheckAllFunctionsDefined(m);
}

static bool
CheckFuncPtrTable(ModuleValidator& m, ParseNode* var)
{
    if (!var->isKind(ParseNodeKind::Name))
        return m.fail(var, "function-pointer table name is not a plain name");

    ParseNode* arrayLiteral = MaybeInitializer(var);
    if (!arrayLiteral || !arrayLiteral->isKind(ParseNodeKind::Array))
        return m.fail(var, "function-pointer table's initializer must be an array literal");

    unsigned length = ListLength(arrayLiteral);

    if (!IsPowerOfTwo(length))
        return m.failf(arrayLiteral, "function-pointer table length must be a power of 2 (is %u)", length);

    unsigned mask = length - 1;

    Uint32Vector elemFuncDefIndices;
    const Sig* sig = nullptr;
    for (ParseNode* elem = ListHead(arrayLiteral); elem; elem = NextNode(elem)) {
        if (!elem->isKind(ParseNodeKind::Name))
            return m.fail(elem, "function-pointer table's elements must be names of functions");

        PropertyName* funcName = elem->name();
        const ModuleValidator::Func* func = m.lookupFuncDef(funcName);
        if (!func)
            return m.fail(elem, "function-pointer table's elements must be names of functions");

        const Sig& funcSig = m.env().sigs[func->sigIndex()];
        if (sig) {
            if (*sig != funcSig)
                return m.fail(elem, "all functions in table must have same signature");
        } else {
            sig = &funcSig;
        }

        if (!elemFuncDefIndices.append(func->funcDefIndex()))
            return false;
    }

    Sig copy;
    if (!copy.clone(*sig))
        return false;

    uint32_t tableIndex;
    if (!CheckFuncPtrTableAgainstExisting(m, var, var->name(), Move(copy), mask, &tableIndex))
        return false;

    if (!m.defineFuncPtrTable(tableIndex, Move(elemFuncDefIndices)))
        return m.fail(var, "duplicate function-pointer definition");

    return true;
}

static bool
CheckFuncPtrTables(ModuleValidator& m)
{
    while (true) {
        ParseNode* varStmt;
        if (!ParseVarOrConstStatement(m.parser(), &varStmt))
            return false;
        if (!varStmt)
            break;
        for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
            if (!CheckFuncPtrTable(m, var))
                return false;
        }
    }

    for (unsigned i = 0; i < m.numFuncPtrTables(); i++) {
        ModuleValidator::Table& table = m.table(i);
        if (!table.defined()) {
            return m.failNameOffset(table.firstUse(),
                                    "function-pointer table %s wasn't defined",
                                    table.name());
        }
    }

    return true;
}

static bool
CheckModuleExportFunction(ModuleValidator& m, ParseNode* pn, PropertyName* maybeFieldName = nullptr)
{
    if (!pn->isKind(ParseNodeKind::Name))
        return m.fail(pn, "expected name of exported function");

    PropertyName* funcName = pn->name();
    const ModuleValidator::Func* func = m.lookupFuncDef(funcName);
    if (!func)
        return m.failName(pn, "function '%s' not found", funcName);

    return m.addExportField(*func, maybeFieldName);
}

static bool
CheckModuleExportObject(ModuleValidator& m, ParseNode* object)
{
    MOZ_ASSERT(object->isKind(ParseNodeKind::Object));

    for (ParseNode* pn = ListHead(object); pn; pn = NextNode(pn)) {
        if (!IsNormalObjectField(pn))
            return m.fail(pn, "only normal object properties may be used in the export object literal");

        PropertyName* fieldName = ObjectNormalFieldName(pn);

        ParseNode* initNode = ObjectNormalFieldInitializer(pn);
        if (!initNode->isKind(ParseNodeKind::Name))
            return m.fail(initNode, "initializer of exported object literal must be name of function");

        if (!CheckModuleExportFunction(m, initNode, fieldName))
            return false;
    }

    return true;
}

static bool
CheckModuleReturn(ModuleValidator& m)
{
    TokenKind tk;
    if (!GetToken(m.parser(), &tk))
        return false;
    auto& ts = m.parser().tokenStream;
    if (tk != TokenKind::Return) {
        return m.failCurrentOffset((tk == TokenKind::Rc || tk == TokenKind::Eof)
                                   ? "expecting return statement"
                                   : "invalid asm.js. statement");
    }
    ts.anyCharsAccess().ungetToken();

    ParseNode* returnStmt = m.parser().statementListItem(YieldIsName);
    if (!returnStmt)
        return false;

    ParseNode* returnExpr = ReturnExpr(returnStmt);
    if (!returnExpr)
        return m.fail(returnStmt, "export statement must return something");

    if (returnExpr->isKind(ParseNodeKind::Object)) {
        if (!CheckModuleExportObject(m, returnExpr))
            return false;
    } else {
        if (!CheckModuleExportFunction(m, returnExpr))
            return false;
    }

    return true;
}

static bool
CheckModuleEnd(ModuleValidator &m)
{
    TokenKind tk;
    if (!GetToken(m.parser(), &tk))
        return false;

    if (tk != TokenKind::Eof && tk != TokenKind::Rc)
        return m.failCurrentOffset("top-level export (return) must be the last statement");

    m.parser().tokenStream.anyCharsAccess().ungetToken();
    return true;
}

static SharedModule
CheckModule(JSContext* cx, AsmJSParser& parser, ParseNode* stmtList, unsigned* time)
{
    int64_t before = PRMJ_Now();

    ParseNode* moduleFunctionNode = parser.pc->functionBox()->functionNode;
    MOZ_ASSERT(moduleFunctionNode);

    ModuleValidator m(cx, parser, moduleFunctionNode);
    if (!m.init())
        return nullptr;

    if (!CheckFunctionHead(m, moduleFunctionNode))
        return nullptr;

    if (!CheckModuleArguments(m, moduleFunctionNode))
        return nullptr;

    if (!CheckPrecedingStatements(m, stmtList))
        return nullptr;

    if (!CheckModuleProcessingDirectives(m))
        return nullptr;

    if (!CheckModuleGlobals(m))
        return nullptr;

    if (!m.startFunctionBodies())
        return nullptr;

    if (!CheckFunctions(m))
        return nullptr;

    if (!CheckFuncPtrTables(m))
        return nullptr;

    if (!CheckModuleReturn(m))
        return nullptr;

    if (!CheckModuleEnd(m))
        return nullptr;

    SharedModule module = m.finish();
    if (!module)
        return nullptr;

    *time = (PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC;
    return module;
}

/*****************************************************************************/
// Link-time validation

static bool
LinkFail(JSContext* cx, const char* str)
{
    JS_ReportErrorFlagsAndNumberASCII(cx, JSREPORT_WARNING, GetErrorMessage, nullptr,
                                      JSMSG_USE_ASM_LINK_FAIL, str);
    return false;
}

static bool
IsMaybeWrappedScriptedProxy(JSObject* obj)
{
    JSObject* unwrapped = UncheckedUnwrap(obj);
    return unwrapped && IsScriptedProxy(unwrapped);
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, HandleAtom field, MutableHandleValue v)
{
    if (!objVal.isObject())
        return LinkFail(cx, "accessing property of non-object");

    RootedObject obj(cx, &objVal.toObject());
    if (IsMaybeWrappedScriptedProxy(obj))
        return LinkFail(cx, "accessing property of a Proxy");

    Rooted<PropertyDescriptor> desc(cx);
    RootedId id(cx, AtomToId(field));
    if (!GetPropertyDescriptor(cx, obj, id, &desc))
        return false;

    if (!desc.object())
        return LinkFail(cx, "property not present on object");

    if (!desc.isDataDescriptor())
        return LinkFail(cx, "property is not a data property");

    v.set(desc.value());
    return true;
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, const char* fieldChars, MutableHandleValue v)
{
    RootedAtom field(cx, AtomizeUTF8Chars(cx, fieldChars, strlen(fieldChars)));
    if (!field)
        return false;

    return GetDataProperty(cx, objVal, field, v);
}

static bool
GetDataProperty(JSContext* cx, HandleValue objVal, ImmutablePropertyNamePtr field, MutableHandleValue v)
{
    // Help the conversion along for all the cx->names().* users.
    HandlePropertyName fieldHandle = field;
    return GetDataProperty(cx, objVal, fieldHandle, v);
}

static bool
HasObjectValueOfMethodPure(JSObject* obj, JSContext* cx)
{
    Value v;
    if (!GetPropertyPure(cx, obj, NameToId(cx->names().valueOf), &v))
        return false;

    JSFunction* fun;
    if (!IsFunctionObject(v, &fun))
        return false;

    return IsSelfHostedFunctionWithName(fun, cx->names().Object_valueOf);
}

static bool
HasPureCoercion(JSContext* cx, HandleValue v)
{
    // Unsigned SIMD types are not allowed in function signatures.
    if (IsVectorObject<Int32x4>(v) || IsVectorObject<Int16x8>(v) ||  IsVectorObject<Int8x16>(v) ||
        IsVectorObject<Bool32x4>(v) || IsVectorObject<Bool16x8>(v) ||
        IsVectorObject<Bool8x16>(v) || IsVectorObject<Float32x4>(v)) {
        return true;
    }

    // Ideally, we'd reject all non-SIMD non-primitives, but Emscripten has a
    // bug that generates code that passes functions for some imports. To avoid
    // breaking all the code that contains this bug, we make an exception for
    // functions that don't have user-defined valueOf or toString, for their
    // coercions are not observable and coercion via ToNumber/ToInt32
    // definitely produces NaN/0. We should remove this special case later once
    // most apps have been built with newer Emscripten.
    if (v.toObject().is<JSFunction>() &&
        HasNoToPrimitiveMethodPure(&v.toObject(), cx) &&
        HasObjectValueOfMethodPure(&v.toObject(), cx) &&
        HasNativeMethodPure(&v.toObject(), cx->names().toString, fun_toString, cx))
    {
        return true;
    }

    return false;
}

static bool
ValidateGlobalVariable(JSContext* cx, const AsmJSGlobal& global, HandleValue importVal, Val* val)
{
    switch (global.varInitKind()) {
      case AsmJSGlobal::InitConstant:
        *val = global.varInitVal();
        return true;

      case AsmJSGlobal::InitImport: {
        RootedValue v(cx);
        if (!GetDataProperty(cx, importVal, global.field(), &v))
            return false;

        if (!v.isPrimitive() && !HasPureCoercion(cx, v))
            return LinkFail(cx, "Imported values must be primitives");

        switch (global.varInitImportType()) {
          case ValType::I32: {
            int32_t i32;
            if (!ToInt32(cx, v, &i32))
                return false;
            *val = Val(uint32_t(i32));
            return true;
          }
          case ValType::I64:
            MOZ_CRASH("int64");
          case ValType::F32: {
            float f;
            if (!RoundFloat32(cx, v, &f))
                return false;
            *val = Val(f);
            return true;
          }
          case ValType::F64: {
            double d;
            if (!ToNumber(cx, v, &d))
                return false;
            *val = Val(d);
            return true;
          }
          case ValType::I8x16: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Int8x16>(cx, v, &simdConstant))
                return false;
            *val = Val(simdConstant.asInt8x16());
            return true;
          }
          case ValType::I16x8: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Int16x8>(cx, v, &simdConstant))
                return false;
            *val = Val(simdConstant.asInt16x8());
            return true;
          }
          case ValType::I32x4: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Int32x4>(cx, v, &simdConstant))
                return false;
            *val = Val(simdConstant.asInt32x4());
            return true;
          }
          case ValType::F32x4: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Float32x4>(cx, v, &simdConstant))
                return false;
            *val = Val(simdConstant.asFloat32x4());
            return true;
          }
          case ValType::B8x16: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Bool8x16>(cx, v, &simdConstant))
                return false;
            // Bool8x16 uses the same data layout as Int8x16.
            *val = Val(simdConstant.asInt8x16());
            return true;
          }
          case ValType::B16x8: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Bool16x8>(cx, v, &simdConstant))
                return false;
            // Bool16x8 uses the same data layout as Int16x8.
            *val = Val(simdConstant.asInt16x8());
            return true;
          }
          case ValType::B32x4: {
            SimdConstant simdConstant;
            if (!ToSimdConstant<Bool32x4>(cx, v, &simdConstant))
                return false;
            // Bool32x4 uses the same data layout as Int32x4.
            *val = Val(simdConstant.asInt32x4());
            return true;
          }
        }
      }
    }

    MOZ_CRASH("unreachable");
}

static bool
ValidateFFI(JSContext* cx, const AsmJSGlobal& global, HandleValue importVal,
            MutableHandle<FunctionVector> ffis)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, importVal, global.field(), &v))
        return false;

    if (!IsFunctionObject(v))
        return LinkFail(cx, "FFI imports must be functions");

    ffis[global.ffiIndex()].set(&v.toObject().as<JSFunction>());
    return true;
}

static bool
ValidateArrayView(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    if (!global.field())
        return true;

    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, global.field(), &v))
        return false;

    bool tac = IsTypedArrayConstructor(v, global.viewType());
    if (!tac)
        return LinkFail(cx, "bad typed array constructor");

    return true;
}

static bool
ValidateMathBuiltinFunction(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().Math, &v))
        return false;

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    Native native = nullptr;
    switch (global.mathBuiltinFunction()) {
      case AsmJSMathBuiltin_sin: native = math_sin; break;
      case AsmJSMathBuiltin_cos: native = math_cos; break;
      case AsmJSMathBuiltin_tan: native = math_tan; break;
      case AsmJSMathBuiltin_asin: native = math_asin; break;
      case AsmJSMathBuiltin_acos: native = math_acos; break;
      case AsmJSMathBuiltin_atan: native = math_atan; break;
      case AsmJSMathBuiltin_ceil: native = math_ceil; break;
      case AsmJSMathBuiltin_floor: native = math_floor; break;
      case AsmJSMathBuiltin_exp: native = math_exp; break;
      case AsmJSMathBuiltin_log: native = math_log; break;
      case AsmJSMathBuiltin_pow: native = math_pow; break;
      case AsmJSMathBuiltin_sqrt: native = math_sqrt; break;
      case AsmJSMathBuiltin_min: native = math_min; break;
      case AsmJSMathBuiltin_max: native = math_max; break;
      case AsmJSMathBuiltin_abs: native = math_abs; break;
      case AsmJSMathBuiltin_atan2: native = math_atan2; break;
      case AsmJSMathBuiltin_imul: native = math_imul; break;
      case AsmJSMathBuiltin_clz32: native = math_clz32; break;
      case AsmJSMathBuiltin_fround: native = math_fround; break;
    }

    if (!IsNativeFunction(v, native))
        return LinkFail(cx, "bad Math.* builtin function");

    return true;
}

static bool
ValidateSimdType(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal,
                 MutableHandleValue out)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().SIMD, &v))
        return false;

    SimdType type;
    if (global.which() == AsmJSGlobal::SimdCtor)
        type = global.simdCtorType();
    else
        type = global.simdOperationType();

    RootedPropertyName simdTypeName(cx, SimdTypeToName(cx->names(), type));
    if (!GetDataProperty(cx, v, simdTypeName, &v))
        return false;

    if (!v.isObject())
        return LinkFail(cx, "bad SIMD type");

    RootedObject simdDesc(cx, &v.toObject());
    if (!simdDesc->is<SimdTypeDescr>())
        return LinkFail(cx, "bad SIMD type");

    if (type != simdDesc->as<SimdTypeDescr>().type())
        return LinkFail(cx, "bad SIMD type");

    out.set(v);
    return true;
}

static bool
ValidateSimdType(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue _(cx);
    return ValidateSimdType(cx, global, globalVal, &_);
}

static bool
ValidateSimdOperation(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx);
    JS_ALWAYS_TRUE(ValidateSimdType(cx, global, globalVal, &v));

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    Native native = nullptr;
    switch (global.simdOperationType()) {
#define SET_NATIVE_INT8X16(op) case SimdOperation::Fn_##op: native = simd_int8x16_##op; break;
#define SET_NATIVE_INT16X8(op) case SimdOperation::Fn_##op: native = simd_int16x8_##op; break;
#define SET_NATIVE_INT32X4(op) case SimdOperation::Fn_##op: native = simd_int32x4_##op; break;
#define SET_NATIVE_UINT8X16(op) case SimdOperation::Fn_##op: native = simd_uint8x16_##op; break;
#define SET_NATIVE_UINT16X8(op) case SimdOperation::Fn_##op: native = simd_uint16x8_##op; break;
#define SET_NATIVE_UINT32X4(op) case SimdOperation::Fn_##op: native = simd_uint32x4_##op; break;
#define SET_NATIVE_FLOAT32X4(op) case SimdOperation::Fn_##op: native = simd_float32x4_##op; break;
#define SET_NATIVE_BOOL8X16(op) case SimdOperation::Fn_##op: native = simd_bool8x16_##op; break;
#define SET_NATIVE_BOOL16X8(op) case SimdOperation::Fn_##op: native = simd_bool16x8_##op; break;
#define SET_NATIVE_BOOL32X4(op) case SimdOperation::Fn_##op: native = simd_bool32x4_##op; break;
#define FALLTHROUGH(op) case SimdOperation::Fn_##op:
      case SimdType::Int8x16:
        switch (global.simdOperation()) {
          FORALL_INT8X16_ASMJS_OP(SET_NATIVE_INT8X16)
          SET_NATIVE_INT8X16(fromUint8x16Bits)
          SET_NATIVE_INT8X16(fromUint16x8Bits)
          SET_NATIVE_INT8X16(fromUint32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Int16x8:
        switch (global.simdOperation()) {
          FORALL_INT16X8_ASMJS_OP(SET_NATIVE_INT16X8)
          SET_NATIVE_INT16X8(fromUint8x16Bits)
          SET_NATIVE_INT16X8(fromUint16x8Bits)
          SET_NATIVE_INT16X8(fromUint32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Int32x4:
        switch (global.simdOperation()) {
          FORALL_INT32X4_ASMJS_OP(SET_NATIVE_INT32X4)
          SET_NATIVE_INT32X4(fromUint8x16Bits)
          SET_NATIVE_INT32X4(fromUint16x8Bits)
          SET_NATIVE_INT32X4(fromUint32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Uint8x16:
        switch (global.simdOperation()) {
          FORALL_INT8X16_ASMJS_OP(SET_NATIVE_UINT8X16)
          SET_NATIVE_UINT8X16(fromInt8x16Bits)
          SET_NATIVE_UINT8X16(fromUint16x8Bits)
          SET_NATIVE_UINT8X16(fromUint32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Uint16x8:
        switch (global.simdOperation()) {
          FORALL_INT16X8_ASMJS_OP(SET_NATIVE_UINT16X8)
          SET_NATIVE_UINT16X8(fromUint8x16Bits)
          SET_NATIVE_UINT16X8(fromInt16x8Bits)
          SET_NATIVE_UINT16X8(fromUint32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Uint32x4:
        switch (global.simdOperation()) {
          FORALL_INT32X4_ASMJS_OP(SET_NATIVE_UINT32X4)
          SET_NATIVE_UINT32X4(fromUint8x16Bits)
          SET_NATIVE_UINT32X4(fromUint16x8Bits)
          SET_NATIVE_UINT32X4(fromInt32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Float32x4:
        switch (global.simdOperation()) {
          FORALL_FLOAT32X4_ASMJS_OP(SET_NATIVE_FLOAT32X4)
          SET_NATIVE_FLOAT32X4(fromUint8x16Bits)
          SET_NATIVE_FLOAT32X4(fromUint16x8Bits)
          SET_NATIVE_FLOAT32X4(fromUint32x4Bits)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Bool8x16:
        switch (global.simdOperation()) {
          FORALL_BOOL_SIMD_OP(SET_NATIVE_BOOL8X16)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Bool16x8:
        switch (global.simdOperation()) {
          FORALL_BOOL_SIMD_OP(SET_NATIVE_BOOL16X8)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      case SimdType::Bool32x4:
        switch (global.simdOperation()) {
          FORALL_BOOL_SIMD_OP(SET_NATIVE_BOOL32X4)
          default: MOZ_CRASH("shouldn't have been validated in the first place");
        }
        break;
      default: MOZ_CRASH("unhandled simd type");
#undef FALLTHROUGH
#undef SET_NATIVE_INT8X16
#undef SET_NATIVE_INT16X8
#undef SET_NATIVE_INT32X4
#undef SET_NATIVE_UINT8X16
#undef SET_NATIVE_UINT16X8
#undef SET_NATIVE_UINT32X4
#undef SET_NATIVE_FLOAT32X4
#undef SET_NATIVE_BOOL8X16
#undef SET_NATIVE_BOOL16X8
#undef SET_NATIVE_BOOL32X4
#undef SET_NATIVE
    }
    if (!native || !IsNativeFunction(v, native))
        return LinkFail(cx, "bad SIMD.type.* operation");
    return true;
}

static bool
ValidateAtomicsBuiltinFunction(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx);
    if (!GetDataProperty(cx, globalVal, cx->names().Atomics, &v))
        return false;

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    Native native = nullptr;
    switch (global.atomicsBuiltinFunction()) {
      case AsmJSAtomicsBuiltin_compareExchange: native = atomics_compareExchange; break;
      case AsmJSAtomicsBuiltin_exchange: native = atomics_exchange; break;
      case AsmJSAtomicsBuiltin_load: native = atomics_load; break;
      case AsmJSAtomicsBuiltin_store: native = atomics_store; break;
      case AsmJSAtomicsBuiltin_add: native = atomics_add; break;
      case AsmJSAtomicsBuiltin_sub: native = atomics_sub; break;
      case AsmJSAtomicsBuiltin_and: native = atomics_and; break;
      case AsmJSAtomicsBuiltin_or: native = atomics_or; break;
      case AsmJSAtomicsBuiltin_xor: native = atomics_xor; break;
      case AsmJSAtomicsBuiltin_isLockFree: native = atomics_isLockFree; break;
    }

    if (!IsNativeFunction(v, native))
        return LinkFail(cx, "bad Atomics.* builtin function");

    return true;
}

static bool
ValidateConstant(JSContext* cx, const AsmJSGlobal& global, HandleValue globalVal)
{
    RootedValue v(cx, globalVal);

    if (global.constantKind() == AsmJSGlobal::MathConstant) {
        if (!GetDataProperty(cx, v, cx->names().Math, &v))
            return false;
    }

    if (!GetDataProperty(cx, v, global.field(), &v))
        return false;

    if (!v.isNumber())
        return LinkFail(cx, "math / global constant value needs to be a number");

    // NaN != NaN
    if (IsNaN(global.constantValue())) {
        if (!IsNaN(v.toNumber()))
            return LinkFail(cx, "global constant value needs to be NaN");
    } else {
        if (v.toNumber() != global.constantValue())
            return LinkFail(cx, "global constant value mismatch");
    }

    return true;
}

static bool
CheckBuffer(JSContext* cx, const AsmJSMetadata& metadata, HandleValue bufferVal,
            MutableHandle<ArrayBufferObjectMaybeShared*> buffer)
{
    if (metadata.memoryUsage == MemoryUsage::Shared) {
        if (!IsSharedArrayBuffer(bufferVal))
            return LinkFail(cx, "shared views can only be constructed onto SharedArrayBuffer");
    } else {
        if (!IsArrayBuffer(bufferVal))
            return LinkFail(cx, "unshared views can only be constructed onto ArrayBuffer");
    }

    buffer.set(&AsAnyArrayBuffer(bufferVal));
    uint32_t memoryLength = buffer->byteLength();

    if (!IsValidAsmJSHeapLength(memoryLength)) {
        UniqueChars msg(
            JS_smprintf("ArrayBuffer byteLength 0x%x is not a valid heap length. The next "
                        "valid length is 0x%x",
                        memoryLength,
                        RoundUpToNextValidAsmJSHeapLength(memoryLength)));
        if (!msg)
            return false;
        return LinkFail(cx, msg.get());
    }

    // This check is sufficient without considering the size of the loaded datum because heap
    // loads and stores start on an aligned boundary and the heap byteLength has larger alignment.
    MOZ_ASSERT((metadata.minMemoryLength - 1) <= INT32_MAX);
    if (memoryLength < metadata.minMemoryLength) {
        UniqueChars msg(
            JS_smprintf("ArrayBuffer byteLength of 0x%x is less than 0x%x (the size implied "
                        "by const heap accesses).",
                        memoryLength,
                        metadata.minMemoryLength));
        if (!msg)
            return false;
        return LinkFail(cx, msg.get());
    }

    if (buffer->is<ArrayBufferObject>()) {
        // On 64-bit, bounds checks are statically removed so the huge guard
        // region is always necessary. On 32-bit, allocating a guard page
        // requires reallocating the incoming ArrayBuffer which could trigger
        // OOM. Thus, only ask for a guard page when SIMD is used since SIMD
        // allows unaligned memory access (see MaxMemoryAccessSize comment);
#ifdef WASM_HUGE_MEMORY
        bool needGuard = true;
#else
        bool needGuard = metadata.usesSimd;
#endif
        Rooted<ArrayBufferObject*> arrayBuffer(cx, &buffer->as<ArrayBufferObject>());
        if (!ArrayBufferObject::prepareForAsmJS(cx, arrayBuffer, needGuard))
            return LinkFail(cx, "Unable to prepare ArrayBuffer for asm.js use");
    } else {
        if (!buffer->as<SharedArrayBufferObject>().isPreparedForAsmJS()) {
            if (buffer->as<SharedArrayBufferObject>().isWasm())
                return LinkFail(cx, "SharedArrayBuffer created for Wasm cannot be used for asm.js");
            if (!jit::JitOptions.asmJSAtomicsEnable)
                return LinkFail(cx, "Can link with SharedArrayBuffer only when asmjs.atomics.enable is set");
            return LinkFail(cx, "Unable to prepare SharedArrayBuffer for asm.js use");
        }
    }

    MOZ_ASSERT(buffer->isPreparedForAsmJS());
    return true;
}

static bool
GetImports(JSContext* cx, const AsmJSMetadata& metadata, HandleValue globalVal,
           HandleValue importVal, MutableHandle<FunctionVector> funcImports, ValVector* valImports)
{
    Rooted<FunctionVector> ffis(cx, FunctionVector(cx));
    if (!ffis.resize(metadata.numFFIs))
        return false;

    for (const AsmJSGlobal& global : metadata.asmJSGlobals) {
        switch (global.which()) {
          case AsmJSGlobal::Variable: {
            Val val;
            if (!ValidateGlobalVariable(cx, global, importVal, &val))
                return false;
            if (!valImports->append(val))
                return false;
            break;
          }
          case AsmJSGlobal::FFI:
            if (!ValidateFFI(cx, global, importVal, &ffis))
                return false;
            break;
          case AsmJSGlobal::ArrayView:
          case AsmJSGlobal::ArrayViewCtor:
            if (!ValidateArrayView(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::MathBuiltinFunction:
            if (!ValidateMathBuiltinFunction(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::AtomicsBuiltinFunction:
            if (!ValidateAtomicsBuiltinFunction(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::Constant:
            if (!ValidateConstant(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::SimdCtor:
            if (!ValidateSimdType(cx, global, globalVal))
                return false;
            break;
          case AsmJSGlobal::SimdOp:
            if (!ValidateSimdOperation(cx, global, globalVal))
                return false;
            break;
        }
    }

    for (const AsmJSImport& import : metadata.asmJSImports) {
        if (!funcImports.append(ffis[import.ffiIndex()]))
            return false;
    }

    return true;
}

static bool
TryInstantiate(JSContext* cx, CallArgs args, Module& module, const AsmJSMetadata& metadata,
               MutableHandleWasmInstanceObject instanceObj, MutableHandleObject exportObj)
{
    HandleValue globalVal = args.get(0);
    HandleValue importVal = args.get(1);
    HandleValue bufferVal = args.get(2);

    RootedArrayBufferObjectMaybeShared buffer(cx);
    RootedWasmMemoryObject memory(cx);
    if (module.metadata().usesMemory()) {
        if (!CheckBuffer(cx, metadata, bufferVal, &buffer))
            return false;

        memory = WasmMemoryObject::create(cx, buffer, nullptr);
        if (!memory)
            return false;
    }

    ValVector valImports;
    Rooted<FunctionVector> funcs(cx, FunctionVector(cx));
    if (!GetImports(cx, metadata, globalVal, importVal, &funcs, &valImports))
        return false;

    RootedWasmTableObject table(cx);
    if (!module.instantiate(cx, funcs, table, memory, valImports, nullptr, instanceObj))
        return false;

    exportObj.set(&instanceObj->exportsObj());
    return true;
}

static bool
HandleInstantiationFailure(JSContext* cx, CallArgs args, const AsmJSMetadata& metadata)
{
    RootedAtom name(cx, args.callee().as<JSFunction>().explicitName());

    if (cx->isExceptionPending())
        return false;

    ScriptSource* source = metadata.scriptSource.get();

    // Source discarding is allowed to affect JS semantics because it is never
    // enabled for normal JS content.
    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return false;
    if (!haveSource) {
        JS_ReportErrorASCII(cx, "asm.js link failure with source discarding enabled");
        return false;
    }

    uint32_t begin = metadata.toStringStart;
    uint32_t end = metadata.srcEndAfterCurly();
    Rooted<JSFlatString*> src(cx, source->substringDontDeflate(cx, begin, end));
    if (!src)
        return false;

    RootedFunction fun(cx, NewScriptedFunction(cx, 0, JSFunction::INTERPRETED_NORMAL,
                                               name, /* proto = */ nullptr, gc::AllocKind::FUNCTION,
                                               TenuredObject));
    if (!fun)
        return false;

    CompileOptions options(cx);
    options.setMutedErrors(source->mutedErrors())
           .setFile(source->filename())
           .setNoScriptRval(false);
    options.asmJSOption = AsmJSOption::Disabled;

    // The exported function inherits an implicit strict context if the module
    // also inherited it somehow.
    if (metadata.strict)
        options.strictOption = true;

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, src))
        return false;

    const char16_t* chars = stableChars.twoByteRange().begin().get();
    SourceBufferHolder::Ownership ownership = stableChars.maybeGiveOwnershipToCaller()
                                              ? SourceBufferHolder::GiveOwnership
                                              : SourceBufferHolder::NoOwnership;
    SourceBufferHolder srcBuf(chars, end - begin, ownership);
    if (!frontend::CompileStandaloneFunction(cx, &fun, options, srcBuf, Nothing()))
        return false;

    // Call the function we just recompiled.
    args.setCallee(ObjectValue(*fun));
    return InternalCallOrConstruct(cx, args, args.isConstructing() ? CONSTRUCT : NO_CONSTRUCT);
}

static Module&
AsmJSModuleFunctionToModule(JSFunction* fun)
{
    MOZ_ASSERT(IsAsmJSModule(fun));
    const Value& v = fun->getExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT);
    return v.toObject().as<WasmModuleObject>().module();
}

// Implements the semantics of an asm.js module function that has been successfully validated.
bool
js::InstantiateAsmJS(JSContext* cx, unsigned argc, JS::Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSFunction* callee = &args.callee().as<JSFunction>();
    Module& module = AsmJSModuleFunctionToModule(callee);
    const AsmJSMetadata& metadata = module.metadata().asAsmJS();

    RootedWasmInstanceObject instanceObj(cx);
    RootedObject exportObj(cx);
    if (!TryInstantiate(cx, args, module, metadata, &instanceObj, &exportObj)) {
        // Link-time validation checks failed, so reparse the entire asm.js
        // module from scratch to get normal interpreted bytecode which we can
        // simply Invoke. Very slow.
        return HandleInstantiationFailure(cx, args, metadata);
    }

    args.rval().set(ObjectValue(*exportObj));
    return true;
}

static JSFunction*
NewAsmJSModuleFunction(JSContext* cx, JSFunction* origFun, HandleObject moduleObj)
{
    RootedAtom name(cx, origFun->explicitName());

    JSFunction::Flags flags = origFun->isLambda() ? JSFunction::ASMJS_LAMBDA_CTOR
                                                  : JSFunction::ASMJS_CTOR;
    JSFunction* moduleFun =
        NewNativeConstructor(cx, InstantiateAsmJS, origFun->nargs(), name,
                             gc::AllocKind::FUNCTION_EXTENDED, TenuredObject,
                             flags);
    if (!moduleFun)
        return nullptr;

    moduleFun->setExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT, ObjectValue(*moduleObj));

    MOZ_ASSERT(IsAsmJSModule(moduleFun));
    return moduleFun;
}

/*****************************************************************************/
// Caching and cloning

size_t
AsmJSGlobal::serializedSize() const
{
    return sizeof(pod) +
           field_.serializedSize();
}

uint8_t*
AsmJSGlobal::serialize(uint8_t* cursor) const
{
    cursor = WriteBytes(cursor, &pod, sizeof(pod));
    cursor = field_.serialize(cursor);
    return cursor;
}

const uint8_t*
AsmJSGlobal::deserialize(const uint8_t* cursor)
{
    (cursor = ReadBytes(cursor, &pod, sizeof(pod))) &&
    (cursor = field_.deserialize(cursor));
    return cursor;
}

size_t
AsmJSGlobal::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return field_.sizeOfExcludingThis(mallocSizeOf);
}

size_t
AsmJSMetadata::serializedSize() const
{
    return Metadata::serializedSize() +
           sizeof(pod()) +
           SerializedVectorSize(asmJSGlobals) +
           SerializedPodVectorSize(asmJSImports) +
           SerializedPodVectorSize(asmJSExports) +
           SerializedVectorSize(asmJSFuncNames) +
           globalArgumentName.serializedSize() +
           importArgumentName.serializedSize() +
           bufferArgumentName.serializedSize();
}

uint8_t*
AsmJSMetadata::serialize(uint8_t* cursor) const
{
    cursor = Metadata::serialize(cursor);
    cursor = WriteBytes(cursor, &pod(), sizeof(pod()));
    cursor = SerializeVector(cursor, asmJSGlobals);
    cursor = SerializePodVector(cursor, asmJSImports);
    cursor = SerializePodVector(cursor, asmJSExports);
    cursor = SerializeVector(cursor, asmJSFuncNames);
    cursor = globalArgumentName.serialize(cursor);
    cursor = importArgumentName.serialize(cursor);
    cursor = bufferArgumentName.serialize(cursor);
    return cursor;
}

const uint8_t*
AsmJSMetadata::deserialize(const uint8_t* cursor)
{
    (cursor = Metadata::deserialize(cursor)) &&
    (cursor = ReadBytes(cursor, &pod(), sizeof(pod()))) &&
    (cursor = DeserializeVector(cursor, &asmJSGlobals)) &&
    (cursor = DeserializePodVector(cursor, &asmJSImports)) &&
    (cursor = DeserializePodVector(cursor, &asmJSExports)) &&
    (cursor = DeserializeVector(cursor, &asmJSFuncNames)) &&
    (cursor = globalArgumentName.deserialize(cursor)) &&
    (cursor = importArgumentName.deserialize(cursor)) &&
    (cursor = bufferArgumentName.deserialize(cursor));
    cacheResult = CacheResult::Hit;
    return cursor;
}

size_t
AsmJSMetadata::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const
{
    return Metadata::sizeOfExcludingThis(mallocSizeOf) +
           SizeOfVectorExcludingThis(asmJSGlobals, mallocSizeOf) +
           asmJSImports.sizeOfExcludingThis(mallocSizeOf) +
           asmJSExports.sizeOfExcludingThis(mallocSizeOf) +
           SizeOfVectorExcludingThis(asmJSFuncNames, mallocSizeOf) +
           globalArgumentName.sizeOfExcludingThis(mallocSizeOf) +
           importArgumentName.sizeOfExcludingThis(mallocSizeOf) +
           bufferArgumentName.sizeOfExcludingThis(mallocSizeOf);
}

namespace {

class ModuleChars
{
  protected:
    uint32_t isFunCtor_;
    Vector<CacheableChars, 0, SystemAllocPolicy> funCtorArgs_;

  public:
    static uint32_t beginOffset(AsmJSParser& parser) {
        return parser.pc->functionBox()->functionNode->pn_pos.begin;
    }

    static uint32_t endOffset(AsmJSParser& parser) {
        TokenPos pos(0, 0);  // initialize to silence GCC warning
        MOZ_ALWAYS_TRUE(parser.tokenStream.peekTokenPos(&pos, TokenStreamShared::Operand));
        return pos.end;
    }
};

class ModuleCharsForStore : ModuleChars
{
    uint32_t uncompressedSize_;
    uint32_t compressedSize_;
    Vector<char, 0, SystemAllocPolicy> compressedBuffer_;

  public:
    bool init(AsmJSParser& parser) {
        MOZ_ASSERT(beginOffset(parser) < endOffset(parser));

        uncompressedSize_ = (endOffset(parser) - beginOffset(parser)) * sizeof(char16_t);
        size_t maxCompressedSize = LZ4::maxCompressedSize(uncompressedSize_);
        if (maxCompressedSize < uncompressedSize_)
            return false;

        if (!compressedBuffer_.resize(maxCompressedSize))
            return false;

        const char16_t* chars = parser.tokenStream.rawCharPtrAt(beginOffset(parser));
        const char* source = reinterpret_cast<const char*>(chars);
        size_t compressedSize = LZ4::compress(source, uncompressedSize_, compressedBuffer_.begin());
        if (!compressedSize || compressedSize > UINT32_MAX)
            return false;

        compressedSize_ = compressedSize;

        // For a function statement or named function expression:
        //   function f(x,y,z) { abc }
        // the range [beginOffset, endOffset) captures the source:
        //   f(x,y,z) { abc }
        // An unnamed function expression captures the same thing, sans 'f'.
        // Since asm.js modules do not contain any free variables, equality of
        // [beginOffset, endOffset) is sufficient to guarantee identical code
        // generation, modulo Assumptions.
        //
        // For functions created with 'new Function', function arguments are
        // not present in the source so we must manually explicitly serialize
        // and match the formals as a Vector of PropertyName.
        isFunCtor_ = parser.pc->isStandaloneFunctionBody();
        if (isFunCtor_) {
            unsigned numArgs;
            ParseNode* functionNode = parser.pc->functionBox()->functionNode;
            ParseNode* arg = FunctionFormalParametersList(functionNode, &numArgs);
            for (unsigned i = 0; i < numArgs; i++, arg = arg->pn_next) {
                UniqueChars name = StringToNewUTF8CharsZ(nullptr, *arg->name());
                if (!name || !funCtorArgs_.append(Move(name)))
                    return false;
            }
        }

        return true;
    }

    size_t serializedSize() const {
        return sizeof(uint32_t) +
               sizeof(uint32_t) +
               compressedSize_ +
               sizeof(uint32_t) +
               (isFunCtor_ ? SerializedVectorSize(funCtorArgs_) : 0);
    }

    uint8_t* serialize(uint8_t* cursor) const {
        cursor = WriteScalar<uint32_t>(cursor, uncompressedSize_);
        cursor = WriteScalar<uint32_t>(cursor, compressedSize_);
        cursor = WriteBytes(cursor, compressedBuffer_.begin(), compressedSize_);
        cursor = WriteScalar<uint32_t>(cursor, isFunCtor_);
        if (isFunCtor_)
            cursor = SerializeVector(cursor, funCtorArgs_);
        return cursor;
    }
};

class ModuleCharsForLookup : ModuleChars
{
    Vector<char16_t, 0, SystemAllocPolicy> chars_;

  public:
    const uint8_t* deserialize(const uint8_t* cursor) {
        uint32_t uncompressedSize;
        cursor = ReadScalar<uint32_t>(cursor, &uncompressedSize);

        uint32_t compressedSize;
        cursor = ReadScalar<uint32_t>(cursor, &compressedSize);

        if (!chars_.resize(uncompressedSize / sizeof(char16_t)))
            return nullptr;

        const char* source = reinterpret_cast<const char*>(cursor);
        char* dest = reinterpret_cast<char*>(chars_.begin());
        if (!LZ4::decompress(source, dest, uncompressedSize))
            return nullptr;

        cursor += compressedSize;

        cursor = ReadScalar<uint32_t>(cursor, &isFunCtor_);
        if (isFunCtor_)
            cursor = DeserializeVector(cursor, &funCtorArgs_);

        return cursor;
    }

    bool match(AsmJSParser& parser) const {
        const char16_t* parseBegin = parser.tokenStream.rawCharPtrAt(beginOffset(parser));
        const char16_t* parseLimit = parser.tokenStream.rawLimit();
        MOZ_ASSERT(parseLimit >= parseBegin);
        if (uint32_t(parseLimit - parseBegin) < chars_.length())
            return false;
        if (!PodEqual(chars_.begin(), parseBegin, chars_.length()))
            return false;
        if (isFunCtor_ != parser.pc->isStandaloneFunctionBody())
            return false;
        if (isFunCtor_) {
            // For function statements, the closing } is included as the last
            // character of the matched source. For Function constructor,
            // parsing terminates with EOF which we must explicitly check. This
            // prevents
            //   new Function('"use asm"; function f() {} return f')
            // from incorrectly matching
            //   new Function('"use asm"; function f() {} return ff')
            if (parseBegin + chars_.length() != parseLimit)
                return false;
            unsigned numArgs;
            ParseNode* functionNode = parser.pc->functionBox()->functionNode;
            ParseNode* arg = FunctionFormalParametersList(functionNode, &numArgs);
            if (funCtorArgs_.length() != numArgs)
                return false;
            for (unsigned i = 0; i < funCtorArgs_.length(); i++, arg = arg->pn_next) {
                UniqueChars name = StringToNewUTF8CharsZ(nullptr, *arg->name());
                if (!name || strcmp(funCtorArgs_[i].get(), name.get()))
                    return false;
            }
        }
        return true;
    }
};

struct ScopedCacheEntryOpenedForWrite
{
    JSContext* cx;
    const size_t serializedSize;
    uint8_t* memory;
    intptr_t handle;

    ScopedCacheEntryOpenedForWrite(JSContext* cx, size_t serializedSize)
      : cx(cx), serializedSize(serializedSize), memory(nullptr), handle(-1)
    {}

    ~ScopedCacheEntryOpenedForWrite() {
        if (memory)
            cx->asmJSCacheOps().closeEntryForWrite(serializedSize, memory, handle);
    }
};

struct ScopedCacheEntryOpenedForRead
{
    JSContext* cx;
    size_t serializedSize;
    const uint8_t* memory;
    intptr_t handle;

    explicit ScopedCacheEntryOpenedForRead(JSContext* cx)
      : cx(cx), serializedSize(0), memory(nullptr), handle(0)
    {}

    ~ScopedCacheEntryOpenedForRead() {
        if (memory)
            cx->asmJSCacheOps().closeEntryForRead(serializedSize, memory, handle);
    }
};

} // unnamed namespace

static JS::AsmJSCacheResult
StoreAsmJSModuleInCache(AsmJSParser& parser, Module& module, JSContext* cx)
{
    ModuleCharsForStore moduleChars;
    if (!moduleChars.init(parser))
        return JS::AsmJSCache_InternalError;

    size_t bytecodeSize = module.bytecodeSerializedSize();
    MOZ_RELEASE_ASSERT(bytecodeSize == 0);

    size_t compiledSize = module.compiledSerializedSize();
    MOZ_RELEASE_ASSERT(compiledSize <= UINT32_MAX);

    size_t serializedSize = sizeof(uint32_t) +
                            compiledSize +
                            moduleChars.serializedSize();

    JS::OpenAsmJSCacheEntryForWriteOp open = cx->asmJSCacheOps().openEntryForWrite;
    if (!open)
        return JS::AsmJSCache_Disabled_Internal;

    const char16_t* begin = parser.tokenStream.rawCharPtrAt(ModuleChars::beginOffset(parser));
    const char16_t* end = parser.tokenStream.rawCharPtrAt(ModuleChars::endOffset(parser));

    ScopedCacheEntryOpenedForWrite entry(cx, serializedSize);
    JS::AsmJSCacheResult openResult =
        open(cx->global(), begin, end, serializedSize, &entry.memory, &entry.handle);
    if (openResult != JS::AsmJSCache_Success)
        return openResult;

    uint8_t* cursor = entry.memory;

    // Everything serialized before the Module must not change incompatibly
    // between any two builds (regardless of platform, architecture, ...).
    // (The Module::assumptionsMatch() guard everything in the Module and
    // afterwards.)
    cursor = WriteScalar<uint32_t>(cursor, compiledSize);

    module.compiledSerialize(cursor, compiledSize);
    cursor += compiledSize;

    cursor = moduleChars.serialize(cursor);

    MOZ_RELEASE_ASSERT(cursor == entry.memory + serializedSize);

    return JS::AsmJSCache_Success;
}

static bool
LookupAsmJSModuleInCache(JSContext* cx, AsmJSParser& parser, bool* loadedFromCache,
                         SharedModule* module, UniqueChars* compilationTimeReport)
{
    int64_t before = PRMJ_Now();

    *loadedFromCache = false;

    JS::OpenAsmJSCacheEntryForReadOp open = cx->asmJSCacheOps().openEntryForRead;
    if (!open)
        return true;

    const char16_t* begin = parser.tokenStream.rawCharPtrAt(ModuleChars::beginOffset(parser));
    const char16_t* limit = parser.tokenStream.rawLimit();

    ScopedCacheEntryOpenedForRead entry(cx);
    if (!open(cx->global(), begin, limit, &entry.serializedSize, &entry.memory, &entry.handle))
        return true;

    size_t remain = entry.serializedSize;
    const uint8_t* cursor = entry.memory;

    uint32_t compiledSize;
    cursor = ReadScalarChecked<uint32_t>(cursor, &remain, &compiledSize);
    if (!cursor)
        return true;

    Assumptions assumptions;
    if (!assumptions.initBuildIdFromContext(cx))
        return false;

    if (!Module::assumptionsMatch(assumptions, cursor, remain))
        return true;

    MutableAsmJSMetadata asmJSMetadata = cx->new_<AsmJSMetadata>();
    if (!asmJSMetadata)
        return false;

    *module = Module::deserialize(/* bytecodeBegin = */ nullptr, /* bytecodeSize = */ 0,
                                  cursor, compiledSize, asmJSMetadata.get());
    if (!*module) {
        ReportOutOfMemory(cx);
        return false;
    }
    cursor += compiledSize;

    // Due to the hash comparison made by openEntryForRead, this should succeed
    // with high probability.
    ModuleCharsForLookup moduleChars;
    cursor = moduleChars.deserialize(cursor);
    if (!moduleChars.match(parser))
        return true;

    // Don't punish release users by crashing if there is a programmer error
    // here, just gracefully return with a cache miss.
#ifdef NIGHTLY_BUILD
    MOZ_RELEASE_ASSERT(cursor == entry.memory + entry.serializedSize);
#endif
    if (cursor != entry.memory + entry.serializedSize)
        return true;

    // See AsmJSMetadata comment as well as ModuleValidator::init().
    asmJSMetadata->toStringStart = parser.pc->functionBox()->toStringStart;
    asmJSMetadata->srcStart = parser.pc->functionBox()->functionNode->pn_body->pn_pos.begin;
    asmJSMetadata->strict = parser.pc->sc()->strict() && !parser.pc->sc()->hasExplicitUseStrict();
    asmJSMetadata->scriptSource.reset(parser.ss);

    if (!parser.tokenStream.advance(asmJSMetadata->srcEndBeforeCurly()))
        return false;

    int64_t after = PRMJ_Now();
    int ms = (after - before) / PRMJ_USEC_PER_MSEC;
    *compilationTimeReport = JS_smprintf("loaded from cache in %dms", ms);
    if (!*compilationTimeReport)
        return false;

    *loadedFromCache = true;
    return true;
}

/*****************************************************************************/
// Top-level js::CompileAsmJS

static bool
NoExceptionPending(JSContext* cx)
{
    return cx->helperThread() || !cx->isExceptionPending();
}

static bool
SuccessfulValidation(AsmJSParser& parser, UniqueChars str)
{
    return parser.warningNoOffset(JSMSG_USE_ASM_TYPE_OK, str.get());
}

static bool
TypeFailureWarning(AsmJSParser& parser, const char* str)
{
    if (parser.options().throwOnAsmJSValidationFailureOption) {
        parser.errorNoOffset(JSMSG_USE_ASM_TYPE_FAIL, str ? str : "");
        return false;
    }

    // Per the asm.js standard convention, whether failure sets a pending
    // exception determines whether to attempt non-asm.js reparsing, so ignore
    // the return value below.
    Unused << parser.warningNoOffset(JSMSG_USE_ASM_TYPE_FAIL, str ? str : "");
    return false;
}

static bool
EstablishPreconditions(JSContext* cx, AsmJSParser& parser)
{
    // asm.js requires Ion.
    if (!HasCompilerSupport(cx) || !IonCanCompile())
        return TypeFailureWarning(parser, "Disabled by lack of compiler support");

    switch (parser.options().asmJSOption) {
      case AsmJSOption::Disabled:
        return TypeFailureWarning(parser, "Disabled by 'asmjs' runtime option");
      case AsmJSOption::DisabledByDebugger:
        return TypeFailureWarning(parser, "Disabled by debugger");
      case AsmJSOption::Enabled:
        break;
    }

    if (parser.pc->isGenerator())
        return TypeFailureWarning(parser, "Disabled by generator context");

    if (parser.pc->isAsync())
        return TypeFailureWarning(parser, "Disabled by async context");

    if (parser.pc->isArrowFunction())
        return TypeFailureWarning(parser, "Disabled by arrow function context");

    // Class constructors are also methods
    if (parser.pc->isMethod() || parser.pc->isGetterOrSetter())
        return TypeFailureWarning(parser, "Disabled by class constructor or method context");

    return true;
}

static UniqueChars
BuildConsoleMessage(unsigned time, JS::AsmJSCacheResult cacheResult)
{
#ifndef JS_MORE_DETERMINISTIC
    const char* cacheString = "";
    switch (cacheResult) {
      case JS::AsmJSCache_Success:
        cacheString = "stored in cache";
        break;
      case JS::AsmJSCache_ModuleTooSmall:
        cacheString = "not stored in cache (too small to benefit)";
        break;
      case JS::AsmJSCache_SynchronousScript:
        cacheString = "unable to cache asm.js in synchronous scripts; try loading "
                      "asm.js via <script async> or createElement('script')";
        break;
      case JS::AsmJSCache_QuotaExceeded:
        cacheString = "not enough temporary storage quota to store in cache";
        break;
      case JS::AsmJSCache_StorageInitFailure:
        cacheString = "storage initialization failed (consider filing a bug)";
        break;
      case JS::AsmJSCache_Disabled_Internal:
        cacheString = "caching disabled by internal configuration (consider filing a bug)";
        break;
      case JS::AsmJSCache_Disabled_ShellFlags:
        cacheString = "caching disabled by missing command-line arguments";
        break;
      case JS::AsmJSCache_Disabled_JitInspector:
        cacheString = "caching disabled by active JIT inspector";
        break;
      case JS::AsmJSCache_InternalError:
        cacheString = "unable to store in cache due to internal error (consider filing a bug)";
        break;
      case JS::AsmJSCache_Disabled_PrivateBrowsing:
        cacheString = "caching disabled by private browsing mode";
        break;
      case JS::AsmJSCache_LIMIT:
        MOZ_CRASH("bad AsmJSCacheResult");
        break;
    }

    return JS_smprintf("total compilation time %dms; %s", time, cacheString);
#else
    return DuplicateString("");
#endif
}

bool
js::CompileAsmJS(JSContext* cx, AsmJSParser& parser, ParseNode* stmtList, bool* validated)
{
    *validated = false;

    // Various conditions disable asm.js optimizations.
    if (!EstablishPreconditions(cx, parser))
        return NoExceptionPending(cx);

    // Before spending any time parsing the module, try to look it up in the
    // embedding's cache using the chars about to be parsed as the key.
    bool loadedFromCache;
    SharedModule module;
    UniqueChars message;
    if (!LookupAsmJSModuleInCache(cx, parser, &loadedFromCache, &module, &message))
        return false;

    // If not present in the cache, parse, validate and generate code in a
    // single linear pass over the chars of the asm.js module.
    if (!loadedFromCache) {
        // "Checking" parses, validates and compiles, producing a fully compiled
        // WasmModuleObject as result.
        unsigned time;
        module = CheckModule(cx, parser, stmtList, &time);
        if (!module)
            return NoExceptionPending(cx);

        // Try to store the AsmJSModule in the embedding's cache. The
        // AsmJSModule must be stored before static linking since static linking
        // specializes the AsmJSModule to the current process's address space
        // and therefore must be executed after a cache hit.
        JS::AsmJSCacheResult cacheResult = StoreAsmJSModuleInCache(parser, *module, cx);

        // Build the string message to display in the developer console.
        message = BuildConsoleMessage(time, cacheResult);
        if (!message)
            return NoExceptionPending(cx);
    }

    // Hand over ownership to a GC object wrapper which can then be referenced
    // from the module function.
    Rooted<WasmModuleObject*> moduleObj(cx, WasmModuleObject::create(cx, *module));
    if (!moduleObj)
        return false;

    // The module function dynamically links the AsmJSModule when called and
    // generates a set of functions wrapping all the exports.
    FunctionBox* funbox = parser.pc->functionBox();
    RootedFunction moduleFun(cx, NewAsmJSModuleFunction(cx, funbox->function(), moduleObj));
    if (!moduleFun)
        return false;

    // Finished! Clobber the default function created by the parser with the new
    // asm.js module function. Special cases in the bytecode emitter avoid
    // generating bytecode for asm.js functions, allowing this asm.js module
    // function to be the finished result.
    MOZ_ASSERT(funbox->function()->isInterpreted());
    funbox->object = moduleFun;

    // Success! Write to the console with a "warning" message.
    *validated = true;
    SuccessfulValidation(parser, Move(message));
    return NoExceptionPending(cx);
}

/*****************************************************************************/
// asm.js testing functions

bool
js::IsAsmJSModuleNative(Native native)
{
    return native == InstantiateAsmJS;
}

bool
js::IsAsmJSModule(JSFunction* fun)
{
    return fun->maybeNative() == InstantiateAsmJS;
}

bool
js::IsAsmJSFunction(JSFunction* fun)
{
    if (IsExportedFunction(fun))
        return ExportedFunctionToInstance(fun).metadata().isAsmJS();
    return false;
}

bool
js::IsAsmJSStrictModeModuleOrFunction(JSFunction* fun)
{
    if (IsAsmJSModule(fun))
        return AsmJSModuleFunctionToModule(fun).metadata().asAsmJS().strict;

    if (IsAsmJSFunction(fun))
        return ExportedFunctionToInstance(fun).metadata().asAsmJS().strict;

    return false;
}

bool
js::IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // See EstablishPreconditions.
    bool available = HasCompilerSupport(cx) && IonCanCompile() && cx->options().asmJS();

    args.rval().set(BooleanValue(available));
    return true;
}

static JSFunction*
MaybeWrappedNativeFunction(const Value& v)
{
    if (!v.isObject())
        return nullptr;

    JSObject* obj = CheckedUnwrap(&v.toObject());
    if (!obj)
        return nullptr;

    if (!obj->is<JSFunction>())
        return nullptr;

    return &obj->as<JSFunction>();
}

bool
js::IsAsmJSModule(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool rval = false;
    if (JSFunction* fun = MaybeWrappedNativeFunction(args.get(0)))
        rval = IsAsmJSModule(fun);

    args.rval().set(BooleanValue(rval));
    return true;
}

bool
js::IsAsmJSFunction(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    bool rval = false;
    if (JSFunction* fun = MaybeWrappedNativeFunction(args.get(0)))
        rval = IsAsmJSFunction(fun);

    args.rval().set(BooleanValue(rval));
    return true;
}

bool
js::IsAsmJSModuleLoadedFromCache(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    JSFunction* fun = MaybeWrappedNativeFunction(args.get(0));
    if (!fun || !IsAsmJSModule(fun)) {
        JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_USE_ASM_TYPE_FAIL,
                                 "argument passed to isAsmJSModuleLoadedFromCache is not a "
                                 "validated asm.js module");
        return false;
    }

    bool loadedFromCache =
        AsmJSModuleFunctionToModule(fun).metadata().asAsmJS().cacheResult == CacheResult::Hit;

    args.rval().set(BooleanValue(loadedFromCache));
    return true;
}

/*****************************************************************************/
// asm.js toString/toSource support

JSString*
js::AsmJSModuleToString(JSContext* cx, HandleFunction fun, bool isToSource)
{
    MOZ_ASSERT(IsAsmJSModule(fun));

    const AsmJSMetadata& metadata = AsmJSModuleFunctionToModule(fun).metadata().asAsmJS();
    uint32_t begin = metadata.toStringStart;
    uint32_t end = metadata.srcEndAfterCurly();
    ScriptSource* source = metadata.scriptSource.get();

    StringBuffer out(cx);

    if (isToSource && fun->isLambda() && !out.append("("))
        return nullptr;

    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return nullptr;

    if (!haveSource) {
        if (!out.append("function "))
            return nullptr;
         if (fun->explicitName() && !out.append(fun->explicitName()))
             return nullptr;
        if (!out.append("() {\n    [sourceless code]\n}"))
            return nullptr;
    } else {
        Rooted<JSFlatString*> src(cx, source->substring(cx, begin, end));
        if (!src)
            return nullptr;

        if (!out.append(src))
            return nullptr;
    }

    if (isToSource && fun->isLambda() && !out.append(")"))
        return nullptr;

    return out.finishString();
}

JSString*
js::AsmJSFunctionToString(JSContext* cx, HandleFunction fun)
{
    MOZ_ASSERT(IsAsmJSFunction(fun));

    const AsmJSMetadata& metadata = ExportedFunctionToInstance(fun).metadata().asAsmJS();
    const AsmJSExport& f = metadata.lookupAsmJSExport(ExportedFunctionToFuncIndex(fun));

    uint32_t begin = metadata.srcStart + f.startOffsetInModule();
    uint32_t end = metadata.srcStart + f.endOffsetInModule();

    ScriptSource* source = metadata.scriptSource.get();
    StringBuffer out(cx);

    if (!out.append("function "))
        return nullptr;

    bool haveSource = source->hasSourceData();
    if (!haveSource && !JSScript::loadSource(cx, source, &haveSource))
        return nullptr;

    if (!haveSource) {
        // asm.js functions can't be anonymous
        MOZ_ASSERT(fun->explicitName());
        if (!out.append(fun->explicitName()))
            return nullptr;
        if (!out.append("() {\n    [sourceless code]\n}"))
            return nullptr;
    } else {
        Rooted<JSFlatString*> src(cx, source->substring(cx, begin, end));
        if (!src)
            return nullptr;
        if (!out.append(src))
            return nullptr;
    }

    return out.finishString();
}

bool
js::IsValidAsmJSHeapLength(uint32_t length)
{
    if (length < MinHeapLength)
        return false;

    return wasm::IsValidARMImmediate(length);
}
