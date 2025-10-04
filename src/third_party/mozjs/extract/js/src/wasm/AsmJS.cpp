/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"  // SprintfLiteral
#include "mozilla/Try.h"      // MOZ_TRY*
#include "mozilla/Utf8.h"     // mozilla::Utf8Unit
#include "mozilla/Variant.h"

#include <algorithm>
#include <new>

#include "jsmath.h"

#include "frontend/BytecodeCompiler.h"    // CompileStandaloneFunction
#include "frontend/FrontendContext.h"     // js::FrontendContext
#include "frontend/FunctionSyntaxKind.h"  // FunctionSyntaxKind
#include "frontend/ParseNode.h"
#include "frontend/Parser-macros.h"  // MOZ_TRY_*
#include "frontend/Parser.h"
#include "frontend/ParserAtom.h"     // ParserAtomsTable, TaggedParserAtomIndex
#include "frontend/SharedContext.h"  // TopLevelFunction
#include "frontend/TaggedParserAtomIndexHasher.h"  // TaggedParserAtomIndexHasher
#include "gc/GC.h"
#include "gc/Policy.h"
#include "jit/InlinableNatives.h"
#include "js/BuildId.h"  // JS::BuildIdCharVector
#include "js/experimental/JitInfo.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/MemoryMetrics.h"
#include "js/Printf.h"
#include "js/ScalarType.h"  // js::Scalar::Type
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "js/Wrapper.h"
#include "util/DifferentialTesting.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/ErrorReporting.h"
#include "vm/FunctionFlags.h"          // js::FunctionFlags
#include "vm/GeneratorAndAsyncKind.h"  // js::GeneratorKind, js::FunctionAsyncKind
#include "vm/Interpreter.h"
#include "vm/SelfHosting.h"
#include "vm/Time.h"
#include "vm/TypedArrayObject.h"
#include "vm/Warnings.h"  // js::WarnNumberASCII
#include "wasm/WasmCompile.h"
#include "wasm/WasmFeatures.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmModuleTypes.h"
#include "wasm/WasmSerialize.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmValidate.h"

#include "frontend/SharedContext-inl.h"
#include "vm/ArrayBufferObject-inl.h"
#include "vm/JSObject-inl.h"
#include "wasm/WasmInstance-inl.h"

using namespace js;
using namespace js::frontend;
using namespace js::jit;
using namespace js::wasm;

using JS::AsmJSOption;
using JS::AutoStableStringChars;
using JS::GenericNaN;
using JS::SourceOwnership;
using JS::SourceText;
using mozilla::Abs;
using mozilla::AsVariant;
using mozilla::CeilingLog2;
using mozilla::HashGeneric;
using mozilla::IsNegativeZero;
using mozilla::IsPositiveZero;
using mozilla::IsPowerOfTwo;
using mozilla::Nothing;
using mozilla::PodZero;
using mozilla::PositiveInfinity;
using mozilla::Some;
using mozilla::Utf8Unit;
using mozilla::Compression::LZ4;

using FunctionVector = JS::GCVector<JSFunction*>;

/*****************************************************************************/

// A wasm module can either use no memory, a unshared memory (ArrayBuffer) or
// shared memory (SharedArrayBuffer).

enum class MemoryUsage { None = false, Unshared = 1, Shared = 2 };

// The asm.js valid heap lengths are precisely the WASM valid heap lengths for
// ARM greater or equal to MinHeapLength
static const size_t MinHeapLength = PageSize;
// An asm.js heap can in principle be up to INT32_MAX bytes but requirements
// on the format restrict it further to the largest pseudo-ARM-immediate.
// See IsValidAsmJSHeapLength().
static const uint64_t MaxHeapLength = 0x7f000000;

static uint64_t RoundUpToNextValidAsmJSHeapLength(uint64_t length) {
  if (length <= MinHeapLength) {
    return MinHeapLength;
  }

  return wasm::RoundUpToNextValidARMImmediate(length);
}

static uint64_t DivideRoundingUp(uint64_t a, uint64_t b) {
  return (a + (b - 1)) / b;
}

/*****************************************************************************/
// asm.js module object

// The asm.js spec recognizes this set of builtin Math functions.
enum AsmJSMathBuiltinFunction {
  AsmJSMathBuiltin_sin,
  AsmJSMathBuiltin_cos,
  AsmJSMathBuiltin_tan,
  AsmJSMathBuiltin_asin,
  AsmJSMathBuiltin_acos,
  AsmJSMathBuiltin_atan,
  AsmJSMathBuiltin_ceil,
  AsmJSMathBuiltin_floor,
  AsmJSMathBuiltin_exp,
  AsmJSMathBuiltin_log,
  AsmJSMathBuiltin_pow,
  AsmJSMathBuiltin_sqrt,
  AsmJSMathBuiltin_abs,
  AsmJSMathBuiltin_atan2,
  AsmJSMathBuiltin_imul,
  AsmJSMathBuiltin_fround,
  AsmJSMathBuiltin_min,
  AsmJSMathBuiltin_max,
  AsmJSMathBuiltin_clz32
};

// LitValPOD is a restricted version of LitVal suitable for asm.js that is
// always POD.

struct LitValPOD {
  PackedTypeCode valType_;
  union U {
    uint32_t u32_;
    uint64_t u64_;
    float f32_;
    double f64_;
  } u;

  LitValPOD() = default;

  explicit LitValPOD(uint32_t u32) : valType_(ValType(ValType::I32).packed()) {
    u.u32_ = u32;
  }
  explicit LitValPOD(uint64_t u64) : valType_(ValType(ValType::I64).packed()) {
    u.u64_ = u64;
  }

  explicit LitValPOD(float f32) : valType_(ValType(ValType::F32).packed()) {
    u.f32_ = f32;
  }
  explicit LitValPOD(double f64) : valType_(ValType(ValType::F64).packed()) {
    u.f64_ = f64;
  }

  LitVal asLitVal() const {
    switch (valType_.typeCode()) {
      case TypeCode::I32:
        return LitVal(u.u32_);
      case TypeCode::I64:
        return LitVal(u.u64_);
      case TypeCode::F32:
        return LitVal(u.f32_);
      case TypeCode::F64:
        return LitVal(u.f64_);
      default:
        MOZ_CRASH("Can't happen");
    }
  }
};

static_assert(std::is_pod_v<LitValPOD>,
              "must be POD to be simply serialized/deserialized");

// An AsmJSGlobal represents a JS global variable in the asm.js module function.
class AsmJSGlobal {
 public:
  enum Which {
    Variable,
    FFI,
    ArrayView,
    ArrayViewCtor,
    MathBuiltinFunction,
    Constant
  };
  enum VarInitKind { InitConstant, InitImport };
  enum ConstantKind { GlobalConstant, MathConstant };

 private:
  struct CacheablePod {
    Which which_;
    union V {
      struct {
        VarInitKind initKind_;
        union U {
          PackedTypeCode importValType_;
          LitValPOD val_;
        } u;
      } var;
      uint32_t ffiIndex_;
      Scalar::Type viewType_;
      AsmJSMathBuiltinFunction mathBuiltinFunc_;
      struct {
        ConstantKind kind_;
        double value_;
      } constant;
    } u;
  } pod;
  CacheableChars field_;

  friend class ModuleValidatorShared;
  template <typename Unit>
  friend class ModuleValidator;

 public:
  AsmJSGlobal() = default;
  AsmJSGlobal(Which which, UniqueChars field) {
    mozilla::PodZero(&pod);  // zero padding for Valgrind
    pod.which_ = which;
    field_ = std::move(field);
  }
  const char* field() const { return field_.get(); }
  Which which() const { return pod.which_; }
  VarInitKind varInitKind() const {
    MOZ_ASSERT(pod.which_ == Variable);
    return pod.u.var.initKind_;
  }
  LitValPOD varInitVal() const {
    MOZ_ASSERT(pod.which_ == Variable);
    MOZ_ASSERT(pod.u.var.initKind_ == InitConstant);
    return pod.u.var.u.val_;
  }
  ValType varInitImportType() const {
    MOZ_ASSERT(pod.which_ == Variable);
    MOZ_ASSERT(pod.u.var.initKind_ == InitImport);
    return ValType(pod.u.var.u.importValType_);
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
  ConstantKind constantKind() const {
    MOZ_ASSERT(pod.which_ == Constant);
    return pod.u.constant.kind_;
  }
  double constantValue() const {
    MOZ_ASSERT(pod.which_ == Constant);
    return pod.u.constant.value_;
  }
};

using AsmJSGlobalVector = Vector<AsmJSGlobal, 0, SystemAllocPolicy>;

// An AsmJSImport is slightly different than an asm.js FFI function: a single
// asm.js FFI function can be called with many different signatures. When
// compiled to wasm, each unique FFI function paired with signature generates a
// wasm import.
class AsmJSImport {
  uint32_t ffiIndex_;

 public:
  AsmJSImport() = default;
  explicit AsmJSImport(uint32_t ffiIndex) : ffiIndex_(ffiIndex) {}
  uint32_t ffiIndex() const { return ffiIndex_; }
};

using AsmJSImportVector = Vector<AsmJSImport, 0, SystemAllocPolicy>;

// An AsmJSExport logically extends Export with the extra information needed for
// an asm.js exported function, viz., the offsets in module's source chars in
// case the function is toString()ed.
class AsmJSExport {
  uint32_t funcIndex_ = 0;

  // All fields are treated as cacheable POD:
  uint32_t startOffsetInModule_ = 0;  // Store module-start-relative offsets
  uint32_t endOffsetInModule_ = 0;    // so preserved by serialization.

 public:
  AsmJSExport() = default;
  AsmJSExport(uint32_t funcIndex, uint32_t startOffsetInModule,
              uint32_t endOffsetInModule)
      : funcIndex_(funcIndex),
        startOffsetInModule_(startOffsetInModule),
        endOffsetInModule_(endOffsetInModule) {}
  uint32_t funcIndex() const { return funcIndex_; }
  uint32_t startOffsetInModule() const { return startOffsetInModule_; }
  uint32_t endOffsetInModule() const { return endOffsetInModule_; }
};

using AsmJSExportVector = Vector<AsmJSExport, 0, SystemAllocPolicy>;

// Holds the immutable guts of an AsmJSModule.
//
// AsmJSMetadata is built incrementally by ModuleValidator and then shared
// immutably between AsmJSModules.

struct AsmJSMetadataCacheablePod {
  uint32_t numFFIs = 0;
  uint32_t srcLength = 0;
  uint32_t srcLengthWithRightBrace = 0;

  AsmJSMetadataCacheablePod() = default;
};

struct js::AsmJSMetadata : Metadata, AsmJSMetadataCacheablePod {
  AsmJSGlobalVector asmJSGlobals;
  AsmJSImportVector asmJSImports;
  AsmJSExportVector asmJSExports;
  CacheableCharsVector asmJSFuncNames;
  CacheableChars globalArgumentName;
  CacheableChars importArgumentName;
  CacheableChars bufferArgumentName;

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
  uint32_t toStringStart;
  uint32_t srcStart;
  bool strict;
  bool alwaysUseFdlibm = false;
  RefPtr<ScriptSource> source;

  uint32_t srcEndBeforeCurly() const { return srcStart + srcLength; }
  uint32_t srcEndAfterCurly() const {
    return srcStart + srcLengthWithRightBrace;
  }

  AsmJSMetadata()
      : Metadata(ModuleKind::AsmJS),
        toStringStart(0),
        srcStart(0),
        strict(false) {}
  ~AsmJSMetadata() override = default;

  const AsmJSExport& lookupAsmJSExport(uint32_t funcIndex) const {
    // The AsmJSExportVector isn't stored in sorted order so do a linear
    // search. This is for the super-cold and already-expensive toString()
    // path and the number of exports is generally small.
    for (const AsmJSExport& exp : asmJSExports) {
      if (exp.funcIndex() == funcIndex) {
        return exp;
      }
    }
    MOZ_CRASH("missing asm.js func export");
  }

  bool mutedErrors() const override { return source->mutedErrors(); }
  const char16_t* displayURL() const override {
    return source->hasDisplayURL() ? source->displayURL() : nullptr;
  }
  ScriptSource* maybeScriptSource() const override { return source.get(); }
  bool getFuncName(NameContext ctx, uint32_t funcIndex,
                   UTF8Bytes* name) const override {
    const char* p = asmJSFuncNames[funcIndex].get();
    if (!p) {
      return true;
    }
    return name->append(p, strlen(p));
  }

  AsmJSMetadataCacheablePod& pod() { return *this; }
  const AsmJSMetadataCacheablePod& pod() const { return *this; }
};

using MutableAsmJSMetadata = RefPtr<AsmJSMetadata>;

/*****************************************************************************/
// ParseNode utilities

static inline ParseNode* NextNode(ParseNode* pn) { return pn->pn_next; }

static inline ParseNode* UnaryKid(ParseNode* pn) {
  return pn->as<UnaryNode>().kid();
}

static inline ParseNode* BinaryRight(ParseNode* pn) {
  return pn->as<BinaryNode>().right();
}

static inline ParseNode* BinaryLeft(ParseNode* pn) {
  return pn->as<BinaryNode>().left();
}

static inline ParseNode* ReturnExpr(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::ReturnStmt));
  return UnaryKid(pn);
}

static inline ParseNode* TernaryKid1(ParseNode* pn) {
  return pn->as<TernaryNode>().kid1();
}

static inline ParseNode* TernaryKid2(ParseNode* pn) {
  return pn->as<TernaryNode>().kid2();
}

static inline ParseNode* TernaryKid3(ParseNode* pn) {
  return pn->as<TernaryNode>().kid3();
}

static inline ParseNode* ListHead(ParseNode* pn) {
  return pn->as<ListNode>().head();
}

static inline unsigned ListLength(ParseNode* pn) {
  return pn->as<ListNode>().count();
}

static inline ParseNode* CallCallee(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::CallExpr));
  return BinaryLeft(pn);
}

static inline unsigned CallArgListLength(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::CallExpr));
  return ListLength(BinaryRight(pn));
}

static inline ParseNode* CallArgList(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::CallExpr));
  return ListHead(BinaryRight(pn));
}

static inline ParseNode* VarListHead(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::VarStmt) ||
             pn->isKind(ParseNodeKind::ConstDecl));
  return ListHead(pn);
}

static inline bool IsDefaultCase(ParseNode* pn) {
  return pn->as<CaseClause>().isDefault();
}

static inline ParseNode* CaseExpr(ParseNode* pn) {
  return pn->as<CaseClause>().caseExpression();
}

static inline ParseNode* CaseBody(ParseNode* pn) {
  return pn->as<CaseClause>().statementList();
}

static inline ParseNode* BinaryOpLeft(ParseNode* pn) {
  MOZ_ASSERT(pn->isBinaryOperation());
  MOZ_ASSERT(pn->as<ListNode>().count() == 2);
  return ListHead(pn);
}

static inline ParseNode* BinaryOpRight(ParseNode* pn) {
  MOZ_ASSERT(pn->isBinaryOperation());
  MOZ_ASSERT(pn->as<ListNode>().count() == 2);
  return NextNode(ListHead(pn));
}

static inline ParseNode* BitwiseLeft(ParseNode* pn) { return BinaryOpLeft(pn); }

static inline ParseNode* BitwiseRight(ParseNode* pn) {
  return BinaryOpRight(pn);
}

static inline ParseNode* MultiplyLeft(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::MulExpr));
  return BinaryOpLeft(pn);
}

static inline ParseNode* MultiplyRight(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::MulExpr));
  return BinaryOpRight(pn);
}

static inline ParseNode* AddSubLeft(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::AddExpr) ||
             pn->isKind(ParseNodeKind::SubExpr));
  return BinaryOpLeft(pn);
}

static inline ParseNode* AddSubRight(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::AddExpr) ||
             pn->isKind(ParseNodeKind::SubExpr));
  return BinaryOpRight(pn);
}

static inline ParseNode* DivOrModLeft(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::DivExpr) ||
             pn->isKind(ParseNodeKind::ModExpr));
  return BinaryOpLeft(pn);
}

static inline ParseNode* DivOrModRight(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::DivExpr) ||
             pn->isKind(ParseNodeKind::ModExpr));
  return BinaryOpRight(pn);
}

static inline ParseNode* ComparisonLeft(ParseNode* pn) {
  return BinaryOpLeft(pn);
}

static inline ParseNode* ComparisonRight(ParseNode* pn) {
  return BinaryOpRight(pn);
}

static inline bool IsExpressionStatement(ParseNode* pn) {
  return pn->isKind(ParseNodeKind::ExpressionStmt);
}

static inline ParseNode* ExpressionStatementExpr(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::ExpressionStmt));
  return UnaryKid(pn);
}

static inline TaggedParserAtomIndex LoopControlMaybeLabel(ParseNode* pn) {
  MOZ_ASSERT(pn->isKind(ParseNodeKind::BreakStmt) ||
             pn->isKind(ParseNodeKind::ContinueStmt));
  return pn->as<LoopControlStatement>().label();
}

static inline TaggedParserAtomIndex LabeledStatementLabel(ParseNode* pn) {
  return pn->as<LabeledStatement>().label();
}

static inline ParseNode* LabeledStatementStatement(ParseNode* pn) {
  return pn->as<LabeledStatement>().statement();
}

static double NumberNodeValue(ParseNode* pn) {
  return pn->as<NumericLiteral>().value();
}

static bool NumberNodeHasFrac(ParseNode* pn) {
  return pn->as<NumericLiteral>().decimalPoint() == HasDecimal;
}

static ParseNode* DotBase(ParseNode* pn) {
  return &pn->as<PropertyAccess>().expression();
}

static TaggedParserAtomIndex DotMember(ParseNode* pn) {
  return pn->as<PropertyAccess>().name();
}

static ParseNode* ElemBase(ParseNode* pn) {
  return &pn->as<PropertyByValue>().expression();
}

static ParseNode* ElemIndex(ParseNode* pn) {
  return &pn->as<PropertyByValue>().key();
}

static inline TaggedParserAtomIndex FunctionName(FunctionNode* funNode) {
  if (auto name = funNode->funbox()->explicitName()) {
    return name;
  }
  return TaggedParserAtomIndex::null();
}

static inline ParseNode* FunctionFormalParametersList(FunctionNode* fn,
                                                      unsigned* numFormals) {
  ParamsBodyNode* argsBody = fn->body();

  // The number of formals is equal to the number of parameters (excluding the
  // trailing lexical scope). There are no destructuring or rest parameters for
  // asm.js functions.
  *numFormals = argsBody->count();

  // If the function has been fully parsed, the trailing function body node is a
  // lexical scope. If we've only parsed the function parameters, the last node
  // is the last parameter.
  if (*numFormals > 0 && argsBody->last()->is<LexicalScopeNode>()) {
    MOZ_ASSERT(argsBody->last()->as<LexicalScopeNode>().scopeBody()->isKind(
        ParseNodeKind::StatementList));
    (*numFormals)--;
  }

  return argsBody->head();
}

static inline ParseNode* FunctionStatementList(FunctionNode* funNode) {
  LexicalScopeNode* last = funNode->body()->body();
  MOZ_ASSERT(last->isEmptyScope());
  ParseNode* body = last->scopeBody();
  MOZ_ASSERT(body->isKind(ParseNodeKind::StatementList));
  return body;
}

static inline bool IsNormalObjectField(ParseNode* pn) {
  return pn->isKind(ParseNodeKind::PropertyDefinition) &&
         pn->as<PropertyDefinition>().accessorType() == AccessorType::None &&
         BinaryLeft(pn)->isKind(ParseNodeKind::ObjectPropertyName);
}

static inline TaggedParserAtomIndex ObjectNormalFieldName(ParseNode* pn) {
  MOZ_ASSERT(IsNormalObjectField(pn));
  MOZ_ASSERT(BinaryLeft(pn)->isKind(ParseNodeKind::ObjectPropertyName));
  return BinaryLeft(pn)->as<NameNode>().atom();
}

static inline ParseNode* ObjectNormalFieldInitializer(ParseNode* pn) {
  MOZ_ASSERT(IsNormalObjectField(pn));
  return BinaryRight(pn);
}

static inline bool IsUseOfName(ParseNode* pn, TaggedParserAtomIndex name) {
  return pn->isName(name);
}

static inline bool IsIgnoredDirectiveName(TaggedParserAtomIndex atom) {
  return atom != TaggedParserAtomIndex::WellKnown::use_strict_();
}

static inline bool IsIgnoredDirective(ParseNode* pn) {
  return pn->isKind(ParseNodeKind::ExpressionStmt) &&
         UnaryKid(pn)->isKind(ParseNodeKind::StringExpr) &&
         IsIgnoredDirectiveName(UnaryKid(pn)->as<NameNode>().atom());
}

static inline bool IsEmptyStatement(ParseNode* pn) {
  return pn->isKind(ParseNodeKind::EmptyStmt);
}

static inline ParseNode* SkipEmptyStatements(ParseNode* pn) {
  while (pn && IsEmptyStatement(pn)) {
    pn = pn->pn_next;
  }
  return pn;
}

static inline ParseNode* NextNonEmptyStatement(ParseNode* pn) {
  return SkipEmptyStatements(pn->pn_next);
}

template <typename Unit>
static bool GetToken(AsmJSParser<Unit>& parser, TokenKind* tkp) {
  auto& ts = parser.tokenStream;
  TokenKind tk;
  while (true) {
    if (!ts.getToken(&tk, TokenStreamShared::SlashIsRegExp)) {
      return false;
    }
    if (tk != TokenKind::Semi) {
      break;
    }
  }
  *tkp = tk;
  return true;
}

template <typename Unit>
static bool PeekToken(AsmJSParser<Unit>& parser, TokenKind* tkp) {
  auto& ts = parser.tokenStream;
  TokenKind tk;
  while (true) {
    if (!ts.peekToken(&tk, TokenStream::SlashIsRegExp)) {
      return false;
    }
    if (tk != TokenKind::Semi) {
      break;
    }
    ts.consumeKnownToken(TokenKind::Semi, TokenStreamShared::SlashIsRegExp);
  }
  *tkp = tk;
  return true;
}

template <typename Unit>
static bool ParseVarOrConstStatement(AsmJSParser<Unit>& parser,
                                     ParseNode** var) {
  TokenKind tk;
  if (!PeekToken(parser, &tk)) {
    return false;
  }
  if (tk != TokenKind::Var && tk != TokenKind::Const) {
    *var = nullptr;
    return true;
  }

  MOZ_TRY_VAR_OR_RETURN(*var, parser.statementListItem(YieldIsName), false);

  MOZ_ASSERT((*var)->isKind(ParseNodeKind::VarStmt) ||
             (*var)->isKind(ParseNodeKind::ConstDecl));
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
class NumLit {
 public:
  enum Which {
    Fixnum,
    NegativeInt,
    BigUnsigned,
    Double,
    Float,
    OutOfRangeInt = -1
  };

 private:
  Which which_;
  JS::Value value_;

 public:
  NumLit() = default;

  NumLit(Which w, const Value& v) : which_(w), value_(v) {}

  Which which() const { return which_; }

  int32_t toInt32() const {
    MOZ_ASSERT(which_ == Fixnum || which_ == NegativeInt ||
               which_ == BigUnsigned);
    return value_.toInt32();
  }

  uint32_t toUint32() const { return (uint32_t)toInt32(); }

  double toDouble() const {
    MOZ_ASSERT(which_ == Double);
    return value_.toDouble();
  }

  float toFloat() const {
    MOZ_ASSERT(which_ == Float);
    return float(value_.toDouble());
  }

  Value scalarValue() const {
    MOZ_ASSERT(which_ != OutOfRangeInt);
    return value_;
  }

  bool valid() const { return which_ != OutOfRangeInt; }

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
      case NumLit::OutOfRangeInt:
        MOZ_CRASH("can't be here because of valid() check above");
    }
    return false;
  }

  LitValPOD value() const {
    switch (which_) {
      case NumLit::Fixnum:
      case NumLit::NegativeInt:
      case NumLit::BigUnsigned:
        return LitValPOD(toUint32());
      case NumLit::Float:
        return LitValPOD(toFloat());
      case NumLit::Double:
        return LitValPOD(toDouble());
      case NumLit::OutOfRangeInt:;
    }
    MOZ_CRASH("bad literal");
  }
};

// Represents the type of a general asm.js expression.
//
// A canonical subset of types representing the coercion targets: Int, Float,
// Double.
//
// Void is also part of the canonical subset.

class Type {
 public:
  enum Which {
    Fixnum = NumLit::Fixnum,
    Signed = NumLit::NegativeInt,
    Unsigned = NumLit::BigUnsigned,
    DoubleLit = NumLit::Double,
    Float = NumLit::Float,
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

  // Map an already canonicalized Type to the return type of a function call.
  static Type ret(Type t) {
    MOZ_ASSERT(t.isCanonical());
    // The 32-bit external type is Signed, not Int.
    return t.isInt() ? Signed : t;
  }

  static Type lit(const NumLit& lit) {
    MOZ_ASSERT(lit.valid());
    Which which = Type::Which(lit.which());
    MOZ_ASSERT(which >= Fixnum && which <= Float);
    Type t;
    t.which_ = which;
    return t;
  }

  // Map |t| to one of the canonical vartype representations of a
  // wasm::ValType.
  static Type canonicalize(Type t) {
    switch (t.which()) {
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

      case MaybeDouble:
      case MaybeFloat:
      case Floatish:
      case Intish:
        // These types need some kind of coercion, they can't be mapped
        // to an VarType.
        break;
    }
    MOZ_CRASH("Invalid vartype");
  }

  Which which() const { return which_; }

  bool operator==(Type rhs) const { return which_ == rhs.which_; }
  bool operator!=(Type rhs) const { return which_ != rhs.which_; }

  bool operator<=(Type rhs) const {
    switch (rhs.which_) {
      case Signed:
        return isSigned();
      case Unsigned:
        return isUnsigned();
      case DoubleLit:
        return isDoubleLit();
      case Double:
        return isDouble();
      case Float:
        return isFloat();
      case MaybeDouble:
        return isMaybeDouble();
      case MaybeFloat:
        return isMaybeFloat();
      case Floatish:
        return isFloatish();
      case Int:
        return isInt();
      case Intish:
        return isIntish();
      case Fixnum:
        return isFixnum();
      case Void:
        return isVoid();
    }
    MOZ_CRASH("unexpected rhs type");
  }

  bool isFixnum() const { return which_ == Fixnum; }

  bool isSigned() const { return which_ == Signed || which_ == Fixnum; }

  bool isUnsigned() const { return which_ == Unsigned || which_ == Fixnum; }

  bool isInt() const { return isSigned() || isUnsigned() || which_ == Int; }

  bool isIntish() const { return isInt() || which_ == Intish; }

  bool isDoubleLit() const { return which_ == DoubleLit; }

  bool isDouble() const { return isDoubleLit() || which_ == Double; }

  bool isMaybeDouble() const { return isDouble() || which_ == MaybeDouble; }

  bool isFloat() const { return which_ == Float; }

  bool isMaybeFloat() const { return isFloat() || which_ == MaybeFloat; }

  bool isFloatish() const { return isMaybeFloat() || which_ == Floatish; }

  bool isVoid() const { return which_ == Void; }

  bool isExtern() const { return isDouble() || isSigned(); }

  // Check if this is one of the valid types for a function argument.
  bool isArgType() const { return isInt() || isFloat() || isDouble(); }

  // Check if this is one of the valid types for a function return value.
  bool isReturnType() const {
    return isSigned() || isFloat() || isDouble() || isVoid();
  }

  // Check if this is one of the valid types for a global variable.
  bool isGlobalVarType() const { return isArgType(); }

  // Check if this is one of the canonical vartype representations of a
  // wasm::ValType, or is void. See Type::canonicalize().
  bool isCanonical() const {
    switch (which()) {
      case Int:
      case Float:
      case Double:
      case Void:
        return true;
      default:
        return false;
    }
  }

  // Check if this is a canonical representation of a wasm::ValType.
  bool isCanonicalValType() const { return !isVoid() && isCanonical(); }

  // Convert this canonical type to a wasm::ValType.
  ValType canonicalToValType() const {
    switch (which()) {
      case Int:
        return ValType::I32;
      case Float:
        return ValType::F32;
      case Double:
        return ValType::F64;
      default:
        MOZ_CRASH("Need canonical type");
    }
  }

  Maybe<ValType> canonicalToReturnType() const {
    return isVoid() ? Nothing() : Some(canonicalToValType());
  }

  // Convert this type to a wasm::TypeCode for use in a wasm
  // block signature. This works for all types, including non-canonical
  // ones. Consequently, the type isn't valid for subsequent asm.js
  // validation; it's only valid for use in producing wasm.
  TypeCode toWasmBlockSignatureType() const {
    switch (which()) {
      case Fixnum:
      case Signed:
      case Unsigned:
      case Int:
      case Intish:
        return TypeCode::I32;

      case Float:
      case MaybeFloat:
      case Floatish:
        return TypeCode::F32;

      case DoubleLit:
      case Double:
      case MaybeDouble:
        return TypeCode::F64;

      case Void:
        return TypeCode::BlockVoid;
    }
    MOZ_CRASH("Invalid Type");
  }

  const char* toChars() const {
    switch (which_) {
      case Double:
        return "double";
      case DoubleLit:
        return "doublelit";
      case MaybeDouble:
        return "double?";
      case Float:
        return "float";
      case Floatish:
        return "floatish";
      case MaybeFloat:
        return "float?";
      case Fixnum:
        return "fixnum";
      case Int:
        return "int";
      case Signed:
        return "signed";
      case Unsigned:
        return "unsigned";
      case Intish:
        return "intish";
      case Void:
        return "void";
    }
    MOZ_CRASH("Invalid Type");
  }
};

static const unsigned VALIDATION_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;

class MOZ_STACK_CLASS ModuleValidatorShared {
 public:
  struct Memory {
    MemoryUsage usage;
    uint64_t minLength;

    uint64_t minPages() const { return DivideRoundingUp(minLength, PageSize); }

    Memory() = default;
  };

  class Func {
    TaggedParserAtomIndex name_;
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
    Func(TaggedParserAtomIndex name, uint32_t sigIndex, uint32_t firstUse,
         uint32_t funcDefIndex)
        : name_(name),
          sigIndex_(sigIndex),
          firstUse_(firstUse),
          funcDefIndex_(funcDefIndex),
          defined_(false),
          srcBegin_(0),
          srcEnd_(0),
          line_(0) {}

    TaggedParserAtomIndex name() const { return name_; }
    uint32_t sigIndex() const { return sigIndex_; }
    uint32_t firstUse() const { return firstUse_; }
    bool defined() const { return defined_; }
    uint32_t funcDefIndex() const { return funcDefIndex_; }

    void define(ParseNode* fn, uint32_t line, Bytes&& bytes,
                Uint32Vector&& callSiteLineNums) {
      MOZ_ASSERT(!defined_);
      defined_ = true;
      srcBegin_ = fn->pn_pos.begin;
      srcEnd_ = fn->pn_pos.end;
      line_ = line;
      bytes_ = std::move(bytes);
      callSiteLineNums_ = std::move(callSiteLineNums);
    }

    uint32_t srcBegin() const {
      MOZ_ASSERT(defined_);
      return srcBegin_;
    }
    uint32_t srcEnd() const {
      MOZ_ASSERT(defined_);
      return srcEnd_;
    }
    uint32_t line() const {
      MOZ_ASSERT(defined_);
      return line_;
    }
    const Bytes& bytes() const {
      MOZ_ASSERT(defined_);
      return bytes_;
    }
    Uint32Vector& callSiteLineNums() {
      MOZ_ASSERT(defined_);
      return callSiteLineNums_;
    }
  };

  using ConstFuncVector = Vector<const Func*>;
  using FuncVector = Vector<Func>;

  class Table {
    uint32_t sigIndex_;
    TaggedParserAtomIndex name_;
    uint32_t firstUse_;
    uint32_t mask_;
    bool defined_;

   public:
    Table(uint32_t sigIndex, TaggedParserAtomIndex name, uint32_t firstUse,
          uint32_t mask)
        : sigIndex_(sigIndex),
          name_(name),
          firstUse_(firstUse),
          mask_(mask),
          defined_(false) {}

    Table(Table&& rhs) = delete;

    uint32_t sigIndex() const { return sigIndex_; }
    TaggedParserAtomIndex name() const { return name_; }
    uint32_t firstUse() const { return firstUse_; }
    unsigned mask() const { return mask_; }
    bool defined() const { return defined_; }
    void define() {
      MOZ_ASSERT(!defined_);
      defined_ = true;
    }
  };

  using TableVector = Vector<Table*>;

  class Global {
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
      MathBuiltinFunction
    };

   private:
    Which which_;
    union U {
      struct VarOrConst {
        Type::Which type_;
        unsigned index_;
        NumLit literalValue_;

        VarOrConst(unsigned index, const NumLit& lit)
            : type_(Type::lit(lit).which()),
              index_(index),
              literalValue_(lit)  // copies |lit|
        {}

        VarOrConst(unsigned index, Type::Which which)
            : type_(which), index_(index) {
          // The |literalValue_| field remains unused and
          // uninitialized for non-constant variables.
        }

        explicit VarOrConst(double constant)
            : type_(Type::Double),
              literalValue_(NumLit::Double, DoubleValue(constant)) {
          // The index_ field is unused and uninitialized for
          // constant doubles.
        }
      } varOrConst;
      uint32_t funcDefIndex_;
      uint32_t tableIndex_;
      uint32_t ffiIndex_;
      Scalar::Type viewType_;
      AsmJSMathBuiltinFunction mathBuiltinFunc_;

      // |varOrConst|, through |varOrConst.literalValue_|, has a
      // non-trivial constructor and therefore MUST be placement-new'd
      // into existence.
      MOZ_PUSH_DISABLE_NONTRIVIAL_UNION_WARNINGS
      U() : funcDefIndex_(0) {}
      MOZ_POP_DISABLE_NONTRIVIAL_UNION_WARNINGS
    } u;

    friend class ModuleValidatorShared;
    template <typename Unit>
    friend class ModuleValidator;
    friend class js::LifoAlloc;

    explicit Global(Which which) : which_(which) {}

   public:
    Which which() const { return which_; }
    Type varOrConstType() const {
      MOZ_ASSERT(which_ == Variable || which_ == ConstantLiteral ||
                 which_ == ConstantImport);
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
    Scalar::Type viewType() const {
      MOZ_ASSERT(which_ == ArrayView || which_ == ArrayViewCtor);
      return u.viewType_;
    }
    bool isMathFunction() const { return which_ == MathBuiltinFunction; }
    AsmJSMathBuiltinFunction mathBuiltinFunction() const {
      MOZ_ASSERT(which_ == MathBuiltinFunction);
      return u.mathBuiltinFunc_;
    }
  };

  struct MathBuiltin {
    enum Kind { Function, Constant };
    Kind kind;

    union {
      double cst;
      AsmJSMathBuiltinFunction func;
    } u;

    MathBuiltin() : kind(Kind(-1)), u{} {}
    explicit MathBuiltin(double cst) : kind(Constant) { u.cst = cst; }
    explicit MathBuiltin(AsmJSMathBuiltinFunction func) : kind(Function) {
      u.func = func;
    }
  };

  struct ArrayView {
    ArrayView(TaggedParserAtomIndex name, Scalar::Type type)
        : name(name), type(type) {}

    TaggedParserAtomIndex name;
    Scalar::Type type;
  };

 protected:
  class HashableSig {
    uint32_t sigIndex_;
    const TypeContext& types_;

   public:
    HashableSig(uint32_t sigIndex, const TypeContext& types)
        : sigIndex_(sigIndex), types_(types) {}
    uint32_t sigIndex() const { return sigIndex_; }
    const FuncType& funcType() const { return types_[sigIndex_].funcType(); }

    // Implement HashPolicy:
    using Lookup = const FuncType&;
    static HashNumber hash(Lookup l) { return l.hash(nullptr); }
    static bool match(HashableSig lhs, Lookup rhs) {
      return FuncType::strictlyEquals(lhs.funcType(), rhs);
    }
  };

  class NamedSig : public HashableSig {
    TaggedParserAtomIndex name_;

   public:
    NamedSig(TaggedParserAtomIndex name, uint32_t sigIndex,
             const TypeContext& types)
        : HashableSig(sigIndex, types), name_(name) {}
    TaggedParserAtomIndex name() const { return name_; }

    // Implement HashPolicy:
    struct Lookup {
      TaggedParserAtomIndex name;
      const FuncType& funcType;
      Lookup(TaggedParserAtomIndex name, const FuncType& funcType)
          : name(name), funcType(funcType) {}
    };
    static HashNumber hash(Lookup l) {
      return HashGeneric(TaggedParserAtomIndexHasher::hash(l.name),
                         l.funcType.hash(nullptr));
    }
    static bool match(NamedSig lhs, Lookup rhs) {
      return lhs.name() == rhs.name &&
             FuncType::strictlyEquals(lhs.funcType(), rhs.funcType);
    }
  };

  using SigSet = HashSet<HashableSig, HashableSig>;
  using FuncImportMap = HashMap<NamedSig, uint32_t, NamedSig>;
  using GlobalMap =
      HashMap<TaggedParserAtomIndex, Global*, TaggedParserAtomIndexHasher>;
  using MathNameMap =
      HashMap<TaggedParserAtomIndex, MathBuiltin, TaggedParserAtomIndexHasher>;
  using ArrayViewVector = Vector<ArrayView>;

 protected:
  FrontendContext* fc_;
  ParserAtomsTable& parserAtoms_;
  FunctionNode* moduleFunctionNode_;
  TaggedParserAtomIndex moduleFunctionName_;
  TaggedParserAtomIndex globalArgumentName_;
  TaggedParserAtomIndex importArgumentName_;
  TaggedParserAtomIndex bufferArgumentName_;
  MathNameMap standardLibraryMathNames_;

  // Validation-internal state:
  LifoAlloc validationLifo_;
  Memory memory_;
  FuncVector funcDefs_;
  TableVector tables_;
  GlobalMap globalMap_;
  SigSet sigSet_;
  FuncImportMap funcImportMap_;
  ArrayViewVector arrayViews_;

  // State used to build the AsmJSModule in finish():
  CompilerEnvironment compilerEnv_;
  ModuleEnvironment moduleEnv_;
  MutableAsmJSMetadata asmJSMetadata_;

  // Error reporting:
  UniqueChars errorString_ = nullptr;
  uint32_t errorOffset_ = UINT32_MAX;
  bool errorOverRecursed_ = false;

 protected:
  ModuleValidatorShared(FrontendContext* fc, ParserAtomsTable& parserAtoms,
                        FunctionNode* moduleFunctionNode)
      : fc_(fc),
        parserAtoms_(parserAtoms),
        moduleFunctionNode_(moduleFunctionNode),
        moduleFunctionName_(FunctionName(moduleFunctionNode)),
        standardLibraryMathNames_(fc),
        validationLifo_(VALIDATION_LIFO_DEFAULT_CHUNK_SIZE),
        funcDefs_(fc),
        tables_(fc),
        globalMap_(fc),
        sigSet_(fc),
        funcImportMap_(fc),
        arrayViews_(fc),
        compilerEnv_(CompileMode::Once, Tier::Optimized, DebugEnabled::False),
        moduleEnv_(FeatureArgs(), ModuleKind::AsmJS) {
    compilerEnv_.computeParameters();
    memory_.minLength = RoundUpToNextValidAsmJSHeapLength(0);
  }

 protected:
  [[nodiscard]] bool initModuleEnvironment() { return moduleEnv_.init(); }

  [[nodiscard]] bool addStandardLibraryMathInfo() {
    static constexpr struct {
      const char* name;
      AsmJSMathBuiltinFunction func;
    } functions[] = {
        {"sin", AsmJSMathBuiltin_sin},       {"cos", AsmJSMathBuiltin_cos},
        {"tan", AsmJSMathBuiltin_tan},       {"asin", AsmJSMathBuiltin_asin},
        {"acos", AsmJSMathBuiltin_acos},     {"atan", AsmJSMathBuiltin_atan},
        {"ceil", AsmJSMathBuiltin_ceil},     {"floor", AsmJSMathBuiltin_floor},
        {"exp", AsmJSMathBuiltin_exp},       {"log", AsmJSMathBuiltin_log},
        {"pow", AsmJSMathBuiltin_pow},       {"sqrt", AsmJSMathBuiltin_sqrt},
        {"abs", AsmJSMathBuiltin_abs},       {"atan2", AsmJSMathBuiltin_atan2},
        {"imul", AsmJSMathBuiltin_imul},     {"clz32", AsmJSMathBuiltin_clz32},
        {"fround", AsmJSMathBuiltin_fround}, {"min", AsmJSMathBuiltin_min},
        {"max", AsmJSMathBuiltin_max},
    };

    auto AddMathFunction = [this](const char* name,
                                  AsmJSMathBuiltinFunction func) {
      auto atom = parserAtoms_.internAscii(fc_, name, strlen(name));
      if (!atom) {
        return false;
      }
      MathBuiltin builtin(func);
      return this->standardLibraryMathNames_.putNew(atom, builtin);
    };

    for (const auto& info : functions) {
      if (!AddMathFunction(info.name, info.func)) {
        return false;
      }
    }

    static constexpr struct {
      const char* name;
      double value;
    } constants[] = {
        {"E", M_E},
        {"LN10", M_LN10},
        {"LN2", M_LN2},
        {"LOG2E", M_LOG2E},
        {"LOG10E", M_LOG10E},
        {"PI", M_PI},
        {"SQRT1_2", M_SQRT1_2},
        {"SQRT2", M_SQRT2},
    };

    auto AddMathConstant = [this](const char* name, double cst) {
      auto atom = parserAtoms_.internAscii(fc_, name, strlen(name));
      if (!atom) {
        return false;
      }
      MathBuiltin builtin(cst);
      return this->standardLibraryMathNames_.putNew(atom, builtin);
    };

    for (const auto& info : constants) {
      if (!AddMathConstant(info.name, info.value)) {
        return false;
      }
    }

    return true;
  }

 public:
  FrontendContext* fc() const { return fc_; }
  TaggedParserAtomIndex moduleFunctionName() const {
    return moduleFunctionName_;
  }
  TaggedParserAtomIndex globalArgumentName() const {
    return globalArgumentName_;
  }
  TaggedParserAtomIndex importArgumentName() const {
    return importArgumentName_;
  }
  TaggedParserAtomIndex bufferArgumentName() const {
    return bufferArgumentName_;
  }
  const ModuleEnvironment& env() { return moduleEnv_; }

  void initModuleFunctionName(TaggedParserAtomIndex name) {
    MOZ_ASSERT(!moduleFunctionName_);
    moduleFunctionName_ = name;
  }
  [[nodiscard]] bool initGlobalArgumentName(TaggedParserAtomIndex n) {
    globalArgumentName_ = n;
    if (n) {
      asmJSMetadata_->globalArgumentName = parserAtoms_.toNewUTF8CharsZ(fc_, n);
      if (!asmJSMetadata_->globalArgumentName) {
        return false;
      }
    }
    return true;
  }
  [[nodiscard]] bool initImportArgumentName(TaggedParserAtomIndex n) {
    importArgumentName_ = n;
    if (n) {
      asmJSMetadata_->importArgumentName = parserAtoms_.toNewUTF8CharsZ(fc_, n);
      if (!asmJSMetadata_->importArgumentName) {
        return false;
      }
    }
    return true;
  }
  [[nodiscard]] bool initBufferArgumentName(TaggedParserAtomIndex n) {
    bufferArgumentName_ = n;
    if (n) {
      asmJSMetadata_->bufferArgumentName = parserAtoms_.toNewUTF8CharsZ(fc_, n);
      if (!asmJSMetadata_->bufferArgumentName) {
        return false;
      }
    }
    return true;
  }
  bool addGlobalVarInit(TaggedParserAtomIndex var, const NumLit& lit, Type type,
                        bool isConst) {
    MOZ_ASSERT(type.isGlobalVarType());
    MOZ_ASSERT(type == Type::canonicalize(Type::lit(lit)));

    uint32_t index = moduleEnv_.globals.length();
    if (!moduleEnv_.globals.emplaceBack(type.canonicalToValType(), !isConst,
                                        index, ModuleKind::AsmJS)) {
      return false;
    }

    Global::Which which = isConst ? Global::ConstantLiteral : Global::Variable;
    Global* global = validationLifo_.new_<Global>(which);
    if (!global) {
      return false;
    }
    if (isConst) {
      new (&global->u.varOrConst) Global::U::VarOrConst(index, lit);
    } else {
      new (&global->u.varOrConst) Global::U::VarOrConst(index, type.which());
    }
    if (!globalMap_.putNew(var, global)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::Variable, nullptr);
    g.pod.u.var.initKind_ = AsmJSGlobal::InitConstant;
    g.pod.u.var.u.val_ = lit.value();
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addGlobalVarImport(TaggedParserAtomIndex var,
                          TaggedParserAtomIndex field, Type type,
                          bool isConst) {
    MOZ_ASSERT(type.isGlobalVarType());

    UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, field);
    if (!fieldChars) {
      return false;
    }

    uint32_t index = moduleEnv_.globals.length();
    ValType valType = type.canonicalToValType();
    if (!moduleEnv_.globals.emplaceBack(valType, !isConst, index,
                                        ModuleKind::AsmJS)) {
      return false;
    }

    Global::Which which = isConst ? Global::ConstantImport : Global::Variable;
    Global* global = validationLifo_.new_<Global>(which);
    if (!global) {
      return false;
    }
    new (&global->u.varOrConst) Global::U::VarOrConst(index, type.which());
    if (!globalMap_.putNew(var, global)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::Variable, std::move(fieldChars));
    g.pod.u.var.initKind_ = AsmJSGlobal::InitImport;
    g.pod.u.var.u.importValType_ = valType.packed();
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addArrayView(TaggedParserAtomIndex var, Scalar::Type vt,
                    TaggedParserAtomIndex maybeField) {
    UniqueChars fieldChars;
    if (maybeField) {
      fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, maybeField);
      if (!fieldChars) {
        return false;
      }
    }

    if (!arrayViews_.append(ArrayView(var, vt))) {
      return false;
    }

    Global* global = validationLifo_.new_<Global>(Global::ArrayView);
    if (!global) {
      return false;
    }
    new (&global->u.viewType_) Scalar::Type(vt);
    if (!globalMap_.putNew(var, global)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::ArrayView, std::move(fieldChars));
    g.pod.u.viewType_ = vt;
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addMathBuiltinFunction(TaggedParserAtomIndex var,
                              AsmJSMathBuiltinFunction func,
                              TaggedParserAtomIndex field) {
    UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, field);
    if (!fieldChars) {
      return false;
    }

    Global* global = validationLifo_.new_<Global>(Global::MathBuiltinFunction);
    if (!global) {
      return false;
    }
    new (&global->u.mathBuiltinFunc_) AsmJSMathBuiltinFunction(func);
    if (!globalMap_.putNew(var, global)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::MathBuiltinFunction, std::move(fieldChars));
    g.pod.u.mathBuiltinFunc_ = func;
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }

 private:
  bool addGlobalDoubleConstant(TaggedParserAtomIndex var, double constant) {
    Global* global = validationLifo_.new_<Global>(Global::ConstantLiteral);
    if (!global) {
      return false;
    }
    new (&global->u.varOrConst) Global::U::VarOrConst(constant);
    return globalMap_.putNew(var, global);
  }

 public:
  bool addMathBuiltinConstant(TaggedParserAtomIndex var, double constant,
                              TaggedParserAtomIndex field) {
    UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, field);
    if (!fieldChars) {
      return false;
    }

    if (!addGlobalDoubleConstant(var, constant)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::Constant, std::move(fieldChars));
    g.pod.u.constant.value_ = constant;
    g.pod.u.constant.kind_ = AsmJSGlobal::MathConstant;
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addGlobalConstant(TaggedParserAtomIndex var, double constant,
                         TaggedParserAtomIndex field) {
    UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, field);
    if (!fieldChars) {
      return false;
    }

    if (!addGlobalDoubleConstant(var, constant)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::Constant, std::move(fieldChars));
    g.pod.u.constant.value_ = constant;
    g.pod.u.constant.kind_ = AsmJSGlobal::GlobalConstant;
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addArrayViewCtor(TaggedParserAtomIndex var, Scalar::Type vt,
                        TaggedParserAtomIndex field) {
    UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, field);
    if (!fieldChars) {
      return false;
    }

    Global* global = validationLifo_.new_<Global>(Global::ArrayViewCtor);
    if (!global) {
      return false;
    }
    new (&global->u.viewType_) Scalar::Type(vt);
    if (!globalMap_.putNew(var, global)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::ArrayViewCtor, std::move(fieldChars));
    g.pod.u.viewType_ = vt;
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addFFI(TaggedParserAtomIndex var, TaggedParserAtomIndex field) {
    UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, field);
    if (!fieldChars) {
      return false;
    }

    if (asmJSMetadata_->numFFIs == UINT32_MAX) {
      return false;
    }
    uint32_t ffiIndex = asmJSMetadata_->numFFIs++;

    Global* global = validationLifo_.new_<Global>(Global::FFI);
    if (!global) {
      return false;
    }
    new (&global->u.ffiIndex_) uint32_t(ffiIndex);
    if (!globalMap_.putNew(var, global)) {
      return false;
    }

    AsmJSGlobal g(AsmJSGlobal::FFI, std::move(fieldChars));
    g.pod.u.ffiIndex_ = ffiIndex;
    return asmJSMetadata_->asmJSGlobals.append(std::move(g));
  }
  bool addExportField(const Func& func, TaggedParserAtomIndex maybeField) {
    // Record the field name of this export.
    CacheableName fieldName;
    if (maybeField) {
      UniqueChars fieldChars = parserAtoms_.toNewUTF8CharsZ(fc_, maybeField);
      if (!fieldChars) {
        return false;
      }
      fieldName = CacheableName::fromUTF8Chars(std::move(fieldChars));
    }

    // Declare which function is exported which gives us an index into the
    // module ExportVector.
    uint32_t funcIndex = funcImportMap_.count() + func.funcDefIndex();
    if (!moduleEnv_.exports.emplaceBack(std::move(fieldName), funcIndex,
                                        DefinitionKind::Function)) {
      return false;
    }

    // The exported function might have already been exported in which case
    // the index will refer into the range of AsmJSExports.
    return asmJSMetadata_->asmJSExports.emplaceBack(
        funcIndex, func.srcBegin() - asmJSMetadata_->srcStart,
        func.srcEnd() - asmJSMetadata_->srcStart);
  }

  bool defineFuncPtrTable(uint32_t tableIndex, Uint32Vector&& elems) {
    Table& table = *tables_[tableIndex];
    if (table.defined()) {
      return false;
    }

    table.define();

    for (uint32_t& index : elems) {
      index += funcImportMap_.count();
    }

    ModuleElemSegment seg = ModuleElemSegment();
    seg.elemType = RefType::func();
    seg.tableIndex = tableIndex;
    seg.offsetIfActive = Some(InitExpr(LitVal(uint32_t(0))));
    seg.encoding = ModuleElemSegment::Encoding::Indices;
    seg.elemIndices = std::move(elems);
    return moduleEnv_.elemSegments.append(std::move(seg));
  }

  bool tryConstantAccess(uint64_t start, uint64_t width) {
    MOZ_ASSERT(UINT64_MAX - start > width);
    uint64_t len = start + width;
    if (len > uint64_t(INT32_MAX) + 1) {
      return false;
    }
    len = RoundUpToNextValidAsmJSHeapLength(len);
    if (len > memory_.minLength) {
      memory_.minLength = len;
    }
    return true;
  }

  // Error handling.
  bool hasAlreadyFailed() const { return !!errorString_; }

  bool failOffset(uint32_t offset, const char* str) {
    MOZ_ASSERT(!hasAlreadyFailed());
    MOZ_ASSERT(errorOffset_ == UINT32_MAX);
    MOZ_ASSERT(str);
    errorOffset_ = offset;
    errorString_ = DuplicateString(str);
    return false;
  }

  bool fail(ParseNode* pn, const char* str) {
    return failOffset(pn->pn_pos.begin, str);
  }

  bool failfVAOffset(uint32_t offset, const char* fmt, va_list ap)
      MOZ_FORMAT_PRINTF(3, 0) {
    MOZ_ASSERT(!hasAlreadyFailed());
    MOZ_ASSERT(errorOffset_ == UINT32_MAX);
    MOZ_ASSERT(fmt);
    errorOffset_ = offset;
    errorString_ = JS_vsmprintf(fmt, ap);
    return false;
  }

  bool failfOffset(uint32_t offset, const char* fmt, ...)
      MOZ_FORMAT_PRINTF(3, 4) {
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

  bool failNameOffset(uint32_t offset, const char* fmt,
                      TaggedParserAtomIndex name) {
    // This function is invoked without the caller properly rooting its locals.
    if (UniqueChars bytes = parserAtoms_.toPrintableString(name)) {
      failfOffset(offset, fmt, bytes.get());
    } else {
      ReportOutOfMemory(fc_);
    }
    return false;
  }

  bool failName(ParseNode* pn, const char* fmt, TaggedParserAtomIndex name) {
    return failNameOffset(pn->pn_pos.begin, fmt, name);
  }

  bool failOverRecursed() {
    errorOverRecursed_ = true;
    return false;
  }

  unsigned numArrayViews() const { return arrayViews_.length(); }
  const ArrayView& arrayView(unsigned i) const { return arrayViews_[i]; }
  unsigned numFuncDefs() const { return funcDefs_.length(); }
  const Func& funcDef(unsigned i) const { return funcDefs_[i]; }
  unsigned numFuncPtrTables() const { return tables_.length(); }
  Table& table(unsigned i) const { return *tables_[i]; }

  const Global* lookupGlobal(TaggedParserAtomIndex name) const {
    if (GlobalMap::Ptr p = globalMap_.lookup(name)) {
      return p->value();
    }
    return nullptr;
  }

  Func* lookupFuncDef(TaggedParserAtomIndex name) {
    if (GlobalMap::Ptr p = globalMap_.lookup(name)) {
      Global* value = p->value();
      if (value->which() == Global::Function) {
        return &funcDefs_[value->funcDefIndex()];
      }
    }
    return nullptr;
  }

  bool lookupStandardLibraryMathName(TaggedParserAtomIndex name,
                                     MathBuiltin* mathBuiltin) const {
    if (MathNameMap::Ptr p = standardLibraryMathNames_.lookup(name)) {
      *mathBuiltin = p->value();
      return true;
    }
    return false;
  }

  bool startFunctionBodies() {
    if (!arrayViews_.empty()) {
      memory_.usage = MemoryUsage::Unshared;
    } else {
      memory_.usage = MemoryUsage::None;
    }
    return true;
  }
};

// The ModuleValidator encapsulates the entire validation of an asm.js module.
// Its lifetime goes from the validation of the top components of an asm.js
// module (all the globals), the emission of bytecode for all the functions in
// the module and the validation of function's pointer tables. It also finishes
// the compilation of all the module's stubs.
template <typename Unit>
class MOZ_STACK_CLASS ModuleValidator : public ModuleValidatorShared {
 private:
  AsmJSParser<Unit>& parser_;

 public:
  ModuleValidator(FrontendContext* fc, ParserAtomsTable& parserAtoms,
                  AsmJSParser<Unit>& parser, FunctionNode* moduleFunctionNode)
      : ModuleValidatorShared(fc, parserAtoms, moduleFunctionNode),
        parser_(parser) {}

  ~ModuleValidator() {
    if (errorString_) {
      MOZ_ASSERT(errorOffset_ != UINT32_MAX);
      typeFailure(errorOffset_, errorString_.get());
    }
    if (errorOverRecursed_) {
      ReportOverRecursed(fc_);
    }
  }

 private:
  // Helpers:
  bool newSig(FuncType&& sig, uint32_t* sigIndex) {
    if (moduleEnv_.types->length() >= MaxTypes) {
      return failCurrentOffset("too many signatures");
    }

    *sigIndex = moduleEnv_.types->length();
    return moduleEnv_.types->addType(std::move(sig));
  }
  bool declareSig(FuncType&& sig, uint32_t* sigIndex) {
    SigSet::AddPtr p = sigSet_.lookupForAdd(sig);
    if (p) {
      *sigIndex = p->sigIndex();
      MOZ_ASSERT(FuncType::strictlyEquals(
          moduleEnv_.types->type(*sigIndex).funcType(), sig));
      return true;
    }

    return newSig(std::move(sig), sigIndex) &&
           sigSet_.add(p, HashableSig(*sigIndex, *moduleEnv_.types));
  }

 private:
  void typeFailure(uint32_t offset, ...) {
    va_list args;
    va_start(args, offset);

    auto& ts = tokenStream();
    ErrorMetadata metadata;
    if (ts.computeErrorMetadata(&metadata, AsVariant(offset))) {
      if (ts.anyCharsAccess().options().throwOnAsmJSValidationFailure()) {
        ReportCompileErrorLatin1VA(fc_, std::move(metadata), nullptr,
                                   JSMSG_USE_ASM_TYPE_FAIL, &args);
      } else {
        // asm.js type failure is indicated by calling one of the fail*
        // functions below.  These functions always return false to
        // halt asm.js parsing.  Whether normal parsing is attempted as
        // fallback, depends whether an exception is also set.
        //
        // If warning succeeds, no exception is set.  If warning fails,
        // an exception is set and execution will halt.  Thus it's safe
        // and correct to ignore the return value here.
        (void)ts.compileWarning(std::move(metadata), nullptr,
                                JSMSG_USE_ASM_TYPE_FAIL, &args);
      }
    }

    va_end(args);
  }

 public:
  bool init() {
    asmJSMetadata_ = js_new<AsmJSMetadata>();
    if (!asmJSMetadata_) {
      ReportOutOfMemory(fc_);
      return false;
    }

    asmJSMetadata_->toStringStart =
        moduleFunctionNode_->funbox()->extent().toStringStart;
    asmJSMetadata_->srcStart = moduleFunctionNode_->body()->pn_pos.begin;
    asmJSMetadata_->strict = parser_.pc_->sc()->strict() &&
                             !parser_.pc_->sc()->hasExplicitUseStrict();
    asmJSMetadata_->alwaysUseFdlibm = parser_.options().alwaysUseFdlibm();
    asmJSMetadata_->source = do_AddRef(parser_.ss);

    if (!initModuleEnvironment()) {
      return false;
    }
    return addStandardLibraryMathInfo();
  }

  AsmJSParser<Unit>& parser() const { return parser_; }

  auto& tokenStream() const { return parser_.tokenStream; }

  bool alwaysUseFdlibm() const { return asmJSMetadata_->alwaysUseFdlibm; }

 public:
  bool addFuncDef(TaggedParserAtomIndex name, uint32_t firstUse, FuncType&& sig,
                  Func** func) {
    uint32_t sigIndex;
    if (!declareSig(std::move(sig), &sigIndex)) {
      return false;
    }

    uint32_t funcDefIndex = funcDefs_.length();
    if (funcDefIndex >= MaxFuncs) {
      return failCurrentOffset("too many functions");
    }

    Global* global = validationLifo_.new_<Global>(Global::Function);
    if (!global) {
      return false;
    }
    new (&global->u.funcDefIndex_) uint32_t(funcDefIndex);
    if (!globalMap_.putNew(name, global)) {
      return false;
    }
    if (!funcDefs_.emplaceBack(name, sigIndex, firstUse, funcDefIndex)) {
      return false;
    }
    *func = &funcDefs_.back();
    return true;
  }
  bool declareFuncPtrTable(FuncType&& sig, TaggedParserAtomIndex name,
                           uint32_t firstUse, uint32_t mask,
                           uint32_t* tableIndex) {
    if (mask > MaxTableLength) {
      return failCurrentOffset("function pointer table too big");
    }

    MOZ_ASSERT(moduleEnv_.tables.length() == tables_.length());
    *tableIndex = moduleEnv_.tables.length();

    uint32_t sigIndex;
    if (!newSig(std::move(sig), &sigIndex)) {
      return false;
    }

    MOZ_ASSERT(sigIndex >= moduleEnv_.asmJSSigToTableIndex.length());
    if (!moduleEnv_.asmJSSigToTableIndex.resize(sigIndex + 1)) {
      return false;
    }

    moduleEnv_.asmJSSigToTableIndex[sigIndex] = moduleEnv_.tables.length();
    if (!moduleEnv_.tables.emplaceBack(RefType::func(), mask + 1, Nothing(),
                                       /* initExpr */ Nothing(),
                                       /*isAsmJS*/ true)) {
      return false;
    }

    Global* global = validationLifo_.new_<Global>(Global::Table);
    if (!global) {
      return false;
    }

    new (&global->u.tableIndex_) uint32_t(*tableIndex);
    if (!globalMap_.putNew(name, global)) {
      return false;
    }

    Table* t = validationLifo_.new_<Table>(sigIndex, name, firstUse, mask);
    return t && tables_.append(t);
  }
  bool declareImport(TaggedParserAtomIndex name, FuncType&& sig,
                     unsigned ffiIndex, uint32_t* importIndex) {
    FuncImportMap::AddPtr p =
        funcImportMap_.lookupForAdd(NamedSig::Lookup(name, sig));
    if (p) {
      *importIndex = p->value();
      return true;
    }

    *importIndex = funcImportMap_.count();
    MOZ_ASSERT(*importIndex == asmJSMetadata_->asmJSImports.length());

    if (*importIndex >= MaxImports) {
      return failCurrentOffset("too many imports");
    }

    if (!asmJSMetadata_->asmJSImports.emplaceBack(ffiIndex)) {
      return false;
    }

    uint32_t sigIndex;
    if (!declareSig(std::move(sig), &sigIndex)) {
      return false;
    }

    return funcImportMap_.add(p, NamedSig(name, sigIndex, *moduleEnv_.types),
                              *importIndex);
  }

  // Error handling.
  bool failCurrentOffset(const char* str) {
    return failOffset(tokenStream().anyCharsAccess().currentToken().pos.begin,
                      str);
  }

  SharedModule finish() {
    MOZ_ASSERT(moduleEnv_.numMemories() == 0);
    if (memory_.usage != MemoryUsage::None) {
      Limits limits;
      limits.shared = memory_.usage == MemoryUsage::Shared ? Shareable::True
                                                           : Shareable::False;
      limits.initial = memory_.minPages();
      limits.maximum = Nothing();
      limits.indexType = IndexType::I32;
      if (!moduleEnv_.memories.append(MemoryDesc(limits))) {
        return nullptr;
      }
    }
    MOZ_ASSERT(moduleEnv_.funcs.empty());
    if (!moduleEnv_.funcs.resize(funcImportMap_.count() + funcDefs_.length())) {
      return nullptr;
    }
    for (FuncImportMap::Range r = funcImportMap_.all(); !r.empty();
         r.popFront()) {
      uint32_t funcIndex = r.front().value();
      uint32_t funcTypeIndex = r.front().key().sigIndex();
      MOZ_ASSERT(!moduleEnv_.funcs[funcIndex].type);
      moduleEnv_.funcs[funcIndex] = FuncDesc(
          &moduleEnv_.types->type(funcTypeIndex).funcType(), funcTypeIndex);
    }
    for (const Func& func : funcDefs_) {
      uint32_t funcIndex = funcImportMap_.count() + func.funcDefIndex();
      uint32_t funcTypeIndex = func.sigIndex();
      MOZ_ASSERT(!moduleEnv_.funcs[funcIndex].type);
      moduleEnv_.funcs[funcIndex] = FuncDesc(
          &moduleEnv_.types->type(funcTypeIndex).funcType(), funcTypeIndex);
    }
    for (const Export& exp : moduleEnv_.exports) {
      if (exp.kind() != DefinitionKind::Function) {
        continue;
      }
      uint32_t funcIndex = exp.funcIndex();
      moduleEnv_.declareFuncExported(funcIndex, /* eager */ true,
                                     /* canRefFunc */ false);
    }

    moduleEnv_.numFuncImports = funcImportMap_.count();

    // All globals (inits and imports) are imports from Wasm point of view.
    moduleEnv_.numGlobalImports = moduleEnv_.globals.length();

    MOZ_ASSERT(asmJSMetadata_->asmJSFuncNames.empty());
    if (!asmJSMetadata_->asmJSFuncNames.resize(funcImportMap_.count())) {
      return nullptr;
    }
    for (const Func& func : funcDefs_) {
      CacheableChars funcName = parserAtoms_.toNewUTF8CharsZ(fc_, func.name());
      if (!funcName ||
          !asmJSMetadata_->asmJSFuncNames.emplaceBack(std::move(funcName))) {
        return nullptr;
      }
    }

    uint32_t endBeforeCurly =
        tokenStream().anyCharsAccess().currentToken().pos.end;
    asmJSMetadata_->srcLength = endBeforeCurly - asmJSMetadata_->srcStart;

    TokenPos pos;
    MOZ_ALWAYS_TRUE(
        tokenStream().peekTokenPos(&pos, TokenStreamShared::SlashIsRegExp));
    uint32_t endAfterCurly = pos.end;
    asmJSMetadata_->srcLengthWithRightBrace =
        endAfterCurly - asmJSMetadata_->srcStart;

    ScriptedCaller scriptedCaller;
    if (parser_.ss->filename()) {
      scriptedCaller.line = 0;  // unused
      scriptedCaller.filename = DuplicateString(parser_.ss->filename());
      if (!scriptedCaller.filename) {
        return nullptr;
      }
    }

    // The default options are fine for asm.js
    SharedCompileArgs args =
        CompileArgs::buildForAsmJS(std::move(scriptedCaller));
    if (!args) {
      ReportOutOfMemory(fc_);
      return nullptr;
    }

    uint32_t codeSectionSize = 0;
    for (const Func& func : funcDefs_) {
      codeSectionSize += func.bytes().length();
    }

    moduleEnv_.codeSection.emplace();
    moduleEnv_.codeSection->start = 0;
    moduleEnv_.codeSection->size = codeSectionSize;

    // asm.js does not have any wasm bytecode to save; view-source is
    // provided through the ScriptSource.
    SharedBytes bytes = js_new<ShareableBytes>();
    if (!bytes) {
      ReportOutOfMemory(fc_);
      return nullptr;
    }

    ModuleGenerator mg(*args, &moduleEnv_, &compilerEnv_, nullptr, nullptr,
                       nullptr);
    if (!mg.init(asmJSMetadata_.get())) {
      return nullptr;
    }

    for (Func& func : funcDefs_) {
      if (!mg.compileFuncDef(funcImportMap_.count() + func.funcDefIndex(),
                             func.line(), func.bytes().begin(),
                             func.bytes().end(),
                             std::move(func.callSiteLineNums()))) {
        return nullptr;
      }
    }

    if (!mg.finishFuncDefs()) {
      return nullptr;
    }

    return mg.finishModule(*bytes);
  }
};

/*****************************************************************************/
// Numeric literal utilities

static bool IsNumericNonFloatLiteral(ParseNode* pn) {
  // Note: '-' is never rolled into the number; numbers are always positive
  // and negations must be applied manually.
  return pn->isKind(ParseNodeKind::NumberExpr) ||
         (pn->isKind(ParseNodeKind::NegExpr) &&
          UnaryKid(pn)->isKind(ParseNodeKind::NumberExpr));
}

static bool IsCallToGlobal(ModuleValidatorShared& m, ParseNode* pn,
                           const ModuleValidatorShared::Global** global) {
  if (!pn->isKind(ParseNodeKind::CallExpr)) {
    return false;
  }

  ParseNode* callee = CallCallee(pn);
  if (!callee->isKind(ParseNodeKind::Name)) {
    return false;
  }

  *global = m.lookupGlobal(callee->as<NameNode>().name());
  return !!*global;
}

static bool IsCoercionCall(ModuleValidatorShared& m, ParseNode* pn,
                           Type* coerceTo, ParseNode** coercedExpr) {
  const ModuleValidatorShared::Global* global;
  if (!IsCallToGlobal(m, pn, &global)) {
    return false;
  }

  if (CallArgListLength(pn) != 1) {
    return false;
  }

  if (coercedExpr) {
    *coercedExpr = CallArgList(pn);
  }

  if (global->isMathFunction() &&
      global->mathBuiltinFunction() == AsmJSMathBuiltin_fround) {
    *coerceTo = Type::Float;
    return true;
  }

  return false;
}

static bool IsFloatLiteral(ModuleValidatorShared& m, ParseNode* pn) {
  ParseNode* coercedExpr;
  Type coerceTo;
  if (!IsCoercionCall(m, pn, &coerceTo, &coercedExpr)) {
    return false;
  }
  // Don't fold into || to avoid clang/memcheck bug (bug 1077031).
  if (!coerceTo.isFloat()) {
    return false;
  }
  return IsNumericNonFloatLiteral(coercedExpr);
}

static bool IsNumericLiteral(ModuleValidatorShared& m, ParseNode* pn) {
  return IsNumericNonFloatLiteral(pn) || IsFloatLiteral(m, pn);
}

// The JS grammar treats -42 as -(42) (i.e., with separate grammar
// productions) for the unary - and literal 42). However, the asm.js spec
// recognizes -42 (modulo parens, so -(42) and -((42))) as a single literal
// so fold the two potential parse nodes into a single double value.
static double ExtractNumericNonFloatValue(ParseNode* pn,
                                          ParseNode** out = nullptr) {
  MOZ_ASSERT(IsNumericNonFloatLiteral(pn));

  if (pn->isKind(ParseNodeKind::NegExpr)) {
    pn = UnaryKid(pn);
    if (out) {
      *out = pn;
    }
    return -NumberNodeValue(pn);
  }

  return NumberNodeValue(pn);
}

static NumLit ExtractNumericLiteral(ModuleValidatorShared& m, ParseNode* pn) {
  MOZ_ASSERT(IsNumericLiteral(m, pn));

  if (pn->isKind(ParseNodeKind::CallExpr)) {
    // Float literals are explicitly coerced and thus the coerced literal may be
    // any valid (non-float) numeric literal.
    MOZ_ASSERT(CallArgListLength(pn) == 1);
    pn = CallArgList(pn);
    double d = ExtractNumericNonFloatValue(pn);
    return NumLit(NumLit::Float, DoubleValue(d));
  }

  double d = ExtractNumericNonFloatValue(pn, &pn);

  // The asm.js spec syntactically distinguishes any literal containing a
  // decimal point or the literal -0 as having double type.
  if (NumberNodeHasFrac(pn) || IsNegativeZero(d)) {
    return NumLit(NumLit::Double, DoubleValue(d));
  }

  // The syntactic checks above rule out these double values.
  MOZ_ASSERT(!IsNegativeZero(d));
  MOZ_ASSERT(!std::isnan(d));

  // Although doubles can only *precisely* represent 53-bit integers, they
  // can *imprecisely* represent integers much bigger than an int64_t.
  // Furthermore, d may be inf or -inf. In both cases, casting to an int64_t
  // is undefined, so test against the integer bounds using doubles.
  if (d < double(INT32_MIN) || d > double(UINT32_MAX)) {
    return NumLit(NumLit::OutOfRangeInt, UndefinedValue());
  }

  // With the above syntactic and range limitations, d is definitely an
  // integer in the range [INT32_MIN, UINT32_MAX] range.
  int64_t i64 = int64_t(d);
  if (i64 >= 0) {
    if (i64 <= INT32_MAX) {
      return NumLit(NumLit::Fixnum, Int32Value(i64));
    }
    MOZ_ASSERT(i64 <= UINT32_MAX);
    return NumLit(NumLit::BigUnsigned, Int32Value(uint32_t(i64)));
  }
  MOZ_ASSERT(i64 >= INT32_MIN);
  return NumLit(NumLit::NegativeInt, Int32Value(i64));
}

static inline bool IsLiteralInt(const NumLit& lit, uint32_t* u32) {
  switch (lit.which()) {
    case NumLit::Fixnum:
    case NumLit::BigUnsigned:
    case NumLit::NegativeInt:
      *u32 = lit.toUint32();
      return true;
    case NumLit::Double:
    case NumLit::Float:
    case NumLit::OutOfRangeInt:
      return false;
  }
  MOZ_CRASH("Bad literal type");
}

static inline bool IsLiteralInt(ModuleValidatorShared& m, ParseNode* pn,
                                uint32_t* u32) {
  return IsNumericLiteral(m, pn) &&
         IsLiteralInt(ExtractNumericLiteral(m, pn), u32);
}

/*****************************************************************************/

namespace {

using LabelVector = Vector<TaggedParserAtomIndex, 4, SystemAllocPolicy>;

class MOZ_STACK_CLASS FunctionValidatorShared {
 public:
  struct Local {
    Type type;
    unsigned slot;
    Local(Type t, unsigned slot) : type(t), slot(slot) {
      MOZ_ASSERT(type.isCanonicalValType());
    }
  };

 protected:
  using LocalMap =
      HashMap<TaggedParserAtomIndex, Local, TaggedParserAtomIndexHasher>;
  using LabelMap =
      HashMap<TaggedParserAtomIndex, uint32_t, TaggedParserAtomIndexHasher>;

  // This is also a ModuleValidator<Unit>& after the appropriate static_cast<>.
  ModuleValidatorShared& m_;

  FunctionNode* fn_;
  Bytes bytes_;
  Encoder encoder_;
  Uint32Vector callSiteLineNums_;
  LocalMap locals_;

  // Labels
  LabelMap breakLabels_;
  LabelMap continueLabels_;
  Uint32Vector breakableStack_;
  Uint32Vector continuableStack_;
  uint32_t blockDepth_;

  bool hasAlreadyReturned_;
  Maybe<ValType> ret_;

 private:
  FunctionValidatorShared(ModuleValidatorShared& m, FunctionNode* fn,
                          FrontendContext* fc)
      : m_(m),
        fn_(fn),
        encoder_(bytes_),
        locals_(fc),
        breakLabels_(fc),
        continueLabels_(fc),
        blockDepth_(0),
        hasAlreadyReturned_(false) {}

 protected:
  template <typename Unit>
  FunctionValidatorShared(ModuleValidator<Unit>& m, FunctionNode* fn,
                          FrontendContext* fc)
      : FunctionValidatorShared(static_cast<ModuleValidatorShared&>(m), fn,
                                fc) {}

 public:
  ModuleValidatorShared& m() const { return m_; }

  FrontendContext* fc() const { return m_.fc(); }
  FunctionNode* fn() const { return fn_; }

  void define(ModuleValidatorShared::Func* func, unsigned line) {
    MOZ_ASSERT(!blockDepth_);
    MOZ_ASSERT(breakableStack_.empty());
    MOZ_ASSERT(continuableStack_.empty());
    MOZ_ASSERT(breakLabels_.empty());
    MOZ_ASSERT(continueLabels_.empty());
    func->define(fn_, line, std::move(bytes_), std::move(callSiteLineNums_));
  }

  bool fail(ParseNode* pn, const char* str) { return m_.fail(pn, str); }

  bool failf(ParseNode* pn, const char* fmt, ...) MOZ_FORMAT_PRINTF(3, 4) {
    va_list ap;
    va_start(ap, fmt);
    m_.failfVAOffset(pn->pn_pos.begin, fmt, ap);
    va_end(ap);
    return false;
  }

  bool failName(ParseNode* pn, const char* fmt, TaggedParserAtomIndex name) {
    return m_.failName(pn, fmt, name);
  }

  /***************************************************** Local scope setup */

  bool addLocal(ParseNode* pn, TaggedParserAtomIndex name, Type type) {
    LocalMap::AddPtr p = locals_.lookupForAdd(name);
    if (p) {
      return failName(pn, "duplicate local name '%s' not allowed", name);
    }
    return locals_.add(p, name, Local(type, locals_.count()));
  }

  /****************************** For consistency of returns in a function */

  bool hasAlreadyReturned() const { return hasAlreadyReturned_; }

  Maybe<ValType> returnedType() const { return ret_; }

  void setReturnedType(const Maybe<ValType>& ret) {
    MOZ_ASSERT(!hasAlreadyReturned_);
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
  void removeLabel(TaggedParserAtomIndex label, LabelMap* map) {
    LabelMap::Ptr p = map->lookup(label);
    MOZ_ASSERT(p);
    map->remove(p);
  }

 public:
  bool pushBreakableBlock() {
    return encoder().writeOp(Op::Block) &&
           encoder().writeFixedU8(uint8_t(TypeCode::BlockVoid)) &&
           breakableStack_.append(blockDepth_++);
  }
  bool popBreakableBlock() {
    MOZ_ALWAYS_TRUE(breakableStack_.popCopy() == --blockDepth_);
    return encoder().writeOp(Op::End);
  }

  bool pushUnbreakableBlock(const LabelVector* labels = nullptr) {
    if (labels) {
      for (TaggedParserAtomIndex label : *labels) {
        if (!breakLabels_.putNew(label, blockDepth_)) {
          return false;
        }
      }
    }
    blockDepth_++;
    return encoder().writeOp(Op::Block) &&
           encoder().writeFixedU8(uint8_t(TypeCode::BlockVoid));
  }
  bool popUnbreakableBlock(const LabelVector* labels = nullptr) {
    if (labels) {
      for (TaggedParserAtomIndex label : *labels) {
        removeLabel(label, &breakLabels_);
      }
    }
    --blockDepth_;
    return encoder().writeOp(Op::End);
  }

  bool pushContinuableBlock() {
    return encoder().writeOp(Op::Block) &&
           encoder().writeFixedU8(uint8_t(TypeCode::BlockVoid)) &&
           continuableStack_.append(blockDepth_++);
  }
  bool popContinuableBlock() {
    MOZ_ALWAYS_TRUE(continuableStack_.popCopy() == --blockDepth_);
    return encoder().writeOp(Op::End);
  }

  bool pushLoop() {
    return encoder().writeOp(Op::Block) &&
           encoder().writeFixedU8(uint8_t(TypeCode::BlockVoid)) &&
           encoder().writeOp(Op::Loop) &&
           encoder().writeFixedU8(uint8_t(TypeCode::BlockVoid)) &&
           breakableStack_.append(blockDepth_++) &&
           continuableStack_.append(blockDepth_++);
  }
  bool popLoop() {
    MOZ_ALWAYS_TRUE(continuableStack_.popCopy() == --blockDepth_);
    MOZ_ALWAYS_TRUE(breakableStack_.popCopy() == --blockDepth_);
    return encoder().writeOp(Op::End) && encoder().writeOp(Op::End);
  }

  bool pushIf(size_t* typeAt) {
    ++blockDepth_;
    return encoder().writeOp(Op::If) && encoder().writePatchableFixedU7(typeAt);
  }
  bool switchToElse() {
    MOZ_ASSERT(blockDepth_ > 0);
    return encoder().writeOp(Op::Else);
  }
  void setIfType(size_t typeAt, TypeCode type) {
    encoder().patchFixedU7(typeAt, uint8_t(type));
  }
  bool popIf() {
    MOZ_ASSERT(blockDepth_ > 0);
    --blockDepth_;
    return encoder().writeOp(Op::End);
  }
  bool popIf(size_t typeAt, TypeCode type) {
    MOZ_ASSERT(blockDepth_ > 0);
    --blockDepth_;
    if (!encoder().writeOp(Op::End)) {
      return false;
    }

    setIfType(typeAt, type);
    return true;
  }

  bool writeBreakIf() { return writeBr(breakableStack_.back(), Op::BrIf); }
  bool writeContinueIf() { return writeBr(continuableStack_.back(), Op::BrIf); }
  bool writeUnlabeledBreakOrContinue(bool isBreak) {
    return writeBr(isBreak ? breakableStack_.back() : continuableStack_.back());
  }
  bool writeContinue() { return writeBr(continuableStack_.back()); }

  bool addLabels(const LabelVector& labels, uint32_t relativeBreakDepth,
                 uint32_t relativeContinueDepth) {
    for (TaggedParserAtomIndex label : labels) {
      if (!breakLabels_.putNew(label, blockDepth_ + relativeBreakDepth)) {
        return false;
      }
      if (!continueLabels_.putNew(label, blockDepth_ + relativeContinueDepth)) {
        return false;
      }
    }
    return true;
  }
  void removeLabels(const LabelVector& labels) {
    for (TaggedParserAtomIndex label : labels) {
      removeLabel(label, &breakLabels_);
      removeLabel(label, &continueLabels_);
    }
  }
  bool writeLabeledBreakOrContinue(TaggedParserAtomIndex label, bool isBreak) {
    LabelMap& map = isBreak ? breakLabels_ : continueLabels_;
    if (LabelMap::Ptr p = map.lookup(label)) {
      return writeBr(p->value());
    }
    MOZ_CRASH("nonexistent label");
  }

  /*************************************************** Read-only interface */

  const Local* lookupLocal(TaggedParserAtomIndex name) const {
    if (auto p = locals_.lookup(name)) {
      return &p->value();
    }
    return nullptr;
  }

  const ModuleValidatorShared::Global* lookupGlobal(
      TaggedParserAtomIndex name) const {
    if (locals_.has(name)) {
      return nullptr;
    }
    return m_.lookupGlobal(name);
  }

  size_t numLocals() const { return locals_.count(); }

  /**************************************************** Encoding interface */

  Encoder& encoder() { return encoder_; }

  [[nodiscard]] bool writeInt32Lit(int32_t i32) {
    return encoder().writeOp(Op::I32Const) && encoder().writeVarS32(i32);
  }
  [[nodiscard]] bool writeConstExpr(const NumLit& lit) {
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
      case NumLit::OutOfRangeInt:
        break;
    }
    MOZ_CRASH("unexpected literal type");
  }
};

// Encapsulates the building of an asm bytecode function from an asm.js function
// source code, packing the asm.js code into the asm bytecode form that can
// be decoded and compiled with a FunctionCompiler.
template <typename Unit>
class MOZ_STACK_CLASS FunctionValidator : public FunctionValidatorShared {
 public:
  FunctionValidator(ModuleValidator<Unit>& m, FunctionNode* fn)
      : FunctionValidatorShared(m, fn, m.fc()) {}

 public:
  ModuleValidator<Unit>& m() const {
    return static_cast<ModuleValidator<Unit>&>(FunctionValidatorShared::m());
  }

  [[nodiscard]] bool writeCall(ParseNode* pn, Op op) {
    MOZ_ASSERT(op == Op::Call);
    if (!encoder().writeOp(op)) {
      return false;
    }

    return appendCallSiteLineNumber(pn);
  }
  [[nodiscard]] bool writeCall(ParseNode* pn, MozOp op) {
    MOZ_ASSERT(op == MozOp::OldCallDirect || op == MozOp::OldCallIndirect);
    if (!encoder().writeOp(op)) {
      return false;
    }

    return appendCallSiteLineNumber(pn);
  }
  [[nodiscard]] bool prepareCall(ParseNode* pn) {
    return appendCallSiteLineNumber(pn);
  }

 private:
  [[nodiscard]] bool appendCallSiteLineNumber(ParseNode* node) {
    const TokenStreamAnyChars& anyChars = m().tokenStream().anyCharsAccess();
    auto lineToken = anyChars.lineToken(node->pn_pos.begin);
    uint32_t lineNumber = anyChars.lineNumber(lineToken);
    if (lineNumber > CallSiteDesc::MAX_LINE_OR_BYTECODE_VALUE) {
      return fail(node, "line number exceeding implementation limits");
    }
    return callSiteLineNums_.append(lineNumber);
  }
};

} /* anonymous namespace */

/*****************************************************************************/
// asm.js type-checking and code-generation algorithm

static bool CheckIdentifier(ModuleValidatorShared& m, ParseNode* usepn,
                            TaggedParserAtomIndex name) {
  if (name == TaggedParserAtomIndex::WellKnown::arguments() ||
      name == TaggedParserAtomIndex::WellKnown::eval()) {
    return m.failName(usepn, "'%s' is not an allowed identifier", name);
  }
  return true;
}

static bool CheckModuleLevelName(ModuleValidatorShared& m, ParseNode* usepn,
                                 TaggedParserAtomIndex name) {
  if (!CheckIdentifier(m, usepn, name)) {
    return false;
  }

  if (name == m.moduleFunctionName() || name == m.globalArgumentName() ||
      name == m.importArgumentName() || name == m.bufferArgumentName() ||
      m.lookupGlobal(name)) {
    return m.failName(usepn, "duplicate name '%s' not allowed", name);
  }

  return true;
}

static bool CheckFunctionHead(ModuleValidatorShared& m, FunctionNode* funNode) {
  FunctionBox* funbox = funNode->funbox();
  MOZ_ASSERT(!funbox->hasExprBody());

  if (funbox->hasRest()) {
    return m.fail(funNode, "rest args not allowed");
  }
  if (funbox->hasDestructuringArgs) {
    return m.fail(funNode, "destructuring args not allowed");
  }
  return true;
}

static bool CheckArgument(ModuleValidatorShared& m, ParseNode* arg,
                          TaggedParserAtomIndex* name) {
  *name = TaggedParserAtomIndex::null();

  if (!arg->isKind(ParseNodeKind::Name)) {
    return m.fail(arg, "argument is not a plain name");
  }

  TaggedParserAtomIndex argName = arg->as<NameNode>().name();
  if (!CheckIdentifier(m, arg, argName)) {
    return false;
  }

  *name = argName;
  return true;
}

static bool CheckModuleArgument(ModuleValidatorShared& m, ParseNode* arg,
                                TaggedParserAtomIndex* name) {
  if (!CheckArgument(m, arg, name)) {
    return false;
  }

  return CheckModuleLevelName(m, arg, *name);
}

static bool CheckModuleArguments(ModuleValidatorShared& m,
                                 FunctionNode* funNode) {
  unsigned numFormals;
  ParseNode* arg1 = FunctionFormalParametersList(funNode, &numFormals);
  ParseNode* arg2 = arg1 ? NextNode(arg1) : nullptr;
  ParseNode* arg3 = arg2 ? NextNode(arg2) : nullptr;

  if (numFormals > 3) {
    return m.fail(funNode, "asm.js modules takes at most 3 argument");
  }

  TaggedParserAtomIndex arg1Name;
  if (arg1 && !CheckModuleArgument(m, arg1, &arg1Name)) {
    return false;
  }
  if (!m.initGlobalArgumentName(arg1Name)) {
    return false;
  }

  TaggedParserAtomIndex arg2Name;
  if (arg2 && !CheckModuleArgument(m, arg2, &arg2Name)) {
    return false;
  }
  if (!m.initImportArgumentName(arg2Name)) {
    return false;
  }

  TaggedParserAtomIndex arg3Name;
  if (arg3 && !CheckModuleArgument(m, arg3, &arg3Name)) {
    return false;
  }
  return m.initBufferArgumentName(arg3Name);
}

static bool CheckPrecedingStatements(ModuleValidatorShared& m,
                                     ParseNode* stmtList) {
  MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));

  ParseNode* stmt = ListHead(stmtList);
  for (unsigned i = 0, n = ListLength(stmtList); i < n; i++) {
    if (!IsIgnoredDirective(stmt)) {
      return m.fail(stmt, "invalid asm.js statement");
    }
  }

  return true;
}

static bool CheckGlobalVariableInitConstant(ModuleValidatorShared& m,
                                            TaggedParserAtomIndex varName,
                                            ParseNode* initNode, bool isConst) {
  NumLit lit = ExtractNumericLiteral(m, initNode);
  if (!lit.valid()) {
    return m.fail(initNode,
                  "global initializer is out of representable integer range");
  }

  Type canonicalType = Type::canonicalize(Type::lit(lit));
  if (!canonicalType.isGlobalVarType()) {
    return m.fail(initNode, "global variable type not allowed");
  }

  return m.addGlobalVarInit(varName, lit, canonicalType, isConst);
}

static bool CheckTypeAnnotation(ModuleValidatorShared& m,
                                ParseNode* coercionNode, Type* coerceTo,
                                ParseNode** coercedExpr = nullptr) {
  switch (coercionNode->getKind()) {
    case ParseNodeKind::BitOrExpr: {
      ParseNode* rhs = BitwiseRight(coercionNode);
      uint32_t i;
      if (!IsLiteralInt(m, rhs, &i) || i != 0) {
        return m.fail(rhs, "must use |0 for argument/return coercion");
      }
      *coerceTo = Type::Int;
      if (coercedExpr) {
        *coercedExpr = BitwiseLeft(coercionNode);
      }
      return true;
    }
    case ParseNodeKind::PosExpr: {
      *coerceTo = Type::Double;
      if (coercedExpr) {
        *coercedExpr = UnaryKid(coercionNode);
      }
      return true;
    }
    case ParseNodeKind::CallExpr: {
      if (IsCoercionCall(m, coercionNode, coerceTo, coercedExpr)) {
        return true;
      }
      break;
    }
    default:;
  }

  return m.fail(coercionNode, "must be of the form +x, x|0 or fround(x)");
}

static bool CheckGlobalVariableInitImport(ModuleValidatorShared& m,
                                          TaggedParserAtomIndex varName,
                                          ParseNode* initNode, bool isConst) {
  Type coerceTo;
  ParseNode* coercedExpr;
  if (!CheckTypeAnnotation(m, initNode, &coerceTo, &coercedExpr)) {
    return false;
  }

  if (!coercedExpr->isKind(ParseNodeKind::DotExpr)) {
    return m.failName(coercedExpr, "invalid import expression for global '%s'",
                      varName);
  }

  if (!coerceTo.isGlobalVarType()) {
    return m.fail(initNode, "global variable type not allowed");
  }

  ParseNode* base = DotBase(coercedExpr);
  TaggedParserAtomIndex field = DotMember(coercedExpr);

  TaggedParserAtomIndex importName = m.importArgumentName();
  if (!importName) {
    return m.fail(coercedExpr,
                  "cannot import without an asm.js foreign parameter");
  }
  if (!IsUseOfName(base, importName)) {
    return m.failName(coercedExpr, "base of import expression must be '%s'",
                      importName);
  }

  return m.addGlobalVarImport(varName, field, coerceTo, isConst);
}

static bool IsArrayViewCtorName(ModuleValidatorShared& m,
                                TaggedParserAtomIndex name,
                                Scalar::Type* type) {
  if (name == TaggedParserAtomIndex::WellKnown::Int8Array()) {
    *type = Scalar::Int8;
  } else if (name == TaggedParserAtomIndex::WellKnown::Uint8Array()) {
    *type = Scalar::Uint8;
  } else if (name == TaggedParserAtomIndex::WellKnown::Int16Array()) {
    *type = Scalar::Int16;
  } else if (name == TaggedParserAtomIndex::WellKnown::Uint16Array()) {
    *type = Scalar::Uint16;
  } else if (name == TaggedParserAtomIndex::WellKnown::Int32Array()) {
    *type = Scalar::Int32;
  } else if (name == TaggedParserAtomIndex::WellKnown::Uint32Array()) {
    *type = Scalar::Uint32;
  } else if (name == TaggedParserAtomIndex::WellKnown::Float32Array()) {
    *type = Scalar::Float32;
  } else if (name == TaggedParserAtomIndex::WellKnown::Float64Array()) {
    *type = Scalar::Float64;
  } else {
    return false;
  }
  return true;
}

static bool CheckNewArrayViewArgs(ModuleValidatorShared& m, ParseNode* newExpr,
                                  TaggedParserAtomIndex bufferName) {
  ParseNode* ctorExpr = BinaryLeft(newExpr);
  ParseNode* ctorArgs = BinaryRight(newExpr);
  ParseNode* bufArg = ListHead(ctorArgs);
  if (!bufArg || NextNode(bufArg) != nullptr) {
    return m.fail(ctorExpr,
                  "array view constructor takes exactly one argument");
  }

  if (!IsUseOfName(bufArg, bufferName)) {
    return m.failName(bufArg, "argument to array view constructor must be '%s'",
                      bufferName);
  }

  return true;
}

static bool CheckNewArrayView(ModuleValidatorShared& m,
                              TaggedParserAtomIndex varName,
                              ParseNode* newExpr) {
  TaggedParserAtomIndex globalName = m.globalArgumentName();
  if (!globalName) {
    return m.fail(
        newExpr, "cannot create array view without an asm.js global parameter");
  }

  TaggedParserAtomIndex bufferName = m.bufferArgumentName();
  if (!bufferName) {
    return m.fail(newExpr,
                  "cannot create array view without an asm.js heap parameter");
  }

  ParseNode* ctorExpr = BinaryLeft(newExpr);

  TaggedParserAtomIndex field;
  Scalar::Type type;
  if (ctorExpr->isKind(ParseNodeKind::DotExpr)) {
    ParseNode* base = DotBase(ctorExpr);

    if (!IsUseOfName(base, globalName)) {
      return m.failName(base, "expecting '%s.*Array", globalName);
    }

    field = DotMember(ctorExpr);
    if (!IsArrayViewCtorName(m, field, &type)) {
      return m.fail(ctorExpr, "could not match typed array name");
    }
  } else {
    if (!ctorExpr->isKind(ParseNodeKind::Name)) {
      return m.fail(ctorExpr,
                    "expecting name of imported array view constructor");
    }

    TaggedParserAtomIndex globalName = ctorExpr->as<NameNode>().name();
    const ModuleValidatorShared::Global* global = m.lookupGlobal(globalName);
    if (!global) {
      return m.failName(ctorExpr, "%s not found in module global scope",
                        globalName);
    }

    if (global->which() != ModuleValidatorShared::Global::ArrayViewCtor) {
      return m.failName(ctorExpr,
                        "%s must be an imported array view constructor",
                        globalName);
    }

    type = global->viewType();
  }

  if (!CheckNewArrayViewArgs(m, newExpr, bufferName)) {
    return false;
  }

  return m.addArrayView(varName, type, field);
}

static bool CheckGlobalMathImport(ModuleValidatorShared& m, ParseNode* initNode,
                                  TaggedParserAtomIndex varName,
                                  TaggedParserAtomIndex field) {
  // Math builtin, with the form glob.Math.[[builtin]]
  ModuleValidatorShared::MathBuiltin mathBuiltin;
  if (!m.lookupStandardLibraryMathName(field, &mathBuiltin)) {
    return m.failName(initNode, "'%s' is not a standard Math builtin", field);
  }

  switch (mathBuiltin.kind) {
    case ModuleValidatorShared::MathBuiltin::Function:
      return m.addMathBuiltinFunction(varName, mathBuiltin.u.func, field);
    case ModuleValidatorShared::MathBuiltin::Constant:
      return m.addMathBuiltinConstant(varName, mathBuiltin.u.cst, field);
    default:
      break;
  }
  MOZ_CRASH("unexpected or uninitialized math builtin type");
}

static bool CheckGlobalDotImport(ModuleValidatorShared& m,
                                 TaggedParserAtomIndex varName,
                                 ParseNode* initNode) {
  ParseNode* base = DotBase(initNode);
  TaggedParserAtomIndex field = DotMember(initNode);

  if (base->isKind(ParseNodeKind::DotExpr)) {
    ParseNode* global = DotBase(base);
    TaggedParserAtomIndex math = DotMember(base);

    TaggedParserAtomIndex globalName = m.globalArgumentName();
    if (!globalName) {
      return m.fail(
          base, "import statement requires the module have a stdlib parameter");
    }

    if (!IsUseOfName(global, globalName)) {
      if (global->isKind(ParseNodeKind::DotExpr)) {
        return m.failName(base,
                          "imports can have at most two dot accesses "
                          "(e.g. %s.Math.sin)",
                          globalName);
      }
      return m.failName(base, "expecting %s.*", globalName);
    }

    if (math == TaggedParserAtomIndex::WellKnown::Math()) {
      return CheckGlobalMathImport(m, initNode, varName, field);
    }
    return m.failName(base, "expecting %s.Math", globalName);
  }

  if (!base->isKind(ParseNodeKind::Name)) {
    return m.fail(base, "expected name of variable or parameter");
  }

  auto baseName = base->as<NameNode>().name();
  if (baseName == m.globalArgumentName()) {
    if (field == TaggedParserAtomIndex::WellKnown::NaN()) {
      return m.addGlobalConstant(varName, GenericNaN(), field);
    }
    if (field == TaggedParserAtomIndex::WellKnown::Infinity()) {
      return m.addGlobalConstant(varName, PositiveInfinity<double>(), field);
    }

    Scalar::Type type;
    if (IsArrayViewCtorName(m, field, &type)) {
      return m.addArrayViewCtor(varName, type, field);
    }

    return m.failName(
        initNode, "'%s' is not a standard constant or typed array name", field);
  }

  if (baseName != m.importArgumentName()) {
    return m.fail(base, "expected global or import name");
  }

  return m.addFFI(varName, field);
}

static bool CheckModuleGlobal(ModuleValidatorShared& m, ParseNode* decl,
                              bool isConst) {
  if (!decl->isKind(ParseNodeKind::AssignExpr)) {
    return m.fail(decl, "module import needs initializer");
  }
  AssignmentNode* assignNode = &decl->as<AssignmentNode>();

  ParseNode* var = assignNode->left();

  if (!var->isKind(ParseNodeKind::Name)) {
    return m.fail(var, "import variable is not a plain name");
  }

  TaggedParserAtomIndex varName = var->as<NameNode>().name();
  if (!CheckModuleLevelName(m, var, varName)) {
    return false;
  }

  ParseNode* initNode = assignNode->right();

  if (IsNumericLiteral(m, initNode)) {
    return CheckGlobalVariableInitConstant(m, varName, initNode, isConst);
  }

  if (initNode->isKind(ParseNodeKind::BitOrExpr) ||
      initNode->isKind(ParseNodeKind::PosExpr) ||
      initNode->isKind(ParseNodeKind::CallExpr)) {
    return CheckGlobalVariableInitImport(m, varName, initNode, isConst);
  }

  if (initNode->isKind(ParseNodeKind::NewExpr)) {
    return CheckNewArrayView(m, varName, initNode);
  }

  if (initNode->isKind(ParseNodeKind::DotExpr)) {
    return CheckGlobalDotImport(m, varName, initNode);
  }

  return m.fail(initNode, "unsupported import expression");
}

template <typename Unit>
static bool CheckModuleProcessingDirectives(ModuleValidator<Unit>& m) {
  auto& ts = m.parser().tokenStream;
  while (true) {
    bool matched;
    if (!ts.matchToken(&matched, TokenKind::String,
                       TokenStreamShared::SlashIsRegExp)) {
      return false;
    }
    if (!matched) {
      return true;
    }

    if (!IsIgnoredDirectiveName(ts.anyCharsAccess().currentToken().atom())) {
      return m.failCurrentOffset("unsupported processing directive");
    }

    TokenKind tt;
    if (!ts.getToken(&tt)) {
      return false;
    }
    if (tt != TokenKind::Semi) {
      return m.failCurrentOffset("expected semicolon after string literal");
    }
  }
}

template <typename Unit>
static bool CheckModuleGlobals(ModuleValidator<Unit>& m) {
  while (true) {
    ParseNode* varStmt;
    if (!ParseVarOrConstStatement(m.parser(), &varStmt)) {
      return false;
    }
    if (!varStmt) {
      break;
    }
    for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
      if (!CheckModuleGlobal(m, var,
                             varStmt->isKind(ParseNodeKind::ConstDecl))) {
        return false;
      }
    }
  }

  return true;
}

static bool ArgFail(FunctionValidatorShared& f, TaggedParserAtomIndex argName,
                    ParseNode* stmt) {
  return f.failName(stmt,
                    "expecting argument type declaration for '%s' of the "
                    "form 'arg = arg|0' or 'arg = +arg' or 'arg = fround(arg)'",
                    argName);
}

static bool CheckArgumentType(FunctionValidatorShared& f, ParseNode* stmt,
                              TaggedParserAtomIndex name, Type* type) {
  if (!stmt || !IsExpressionStatement(stmt)) {
    return ArgFail(f, name, stmt ? stmt : f.fn());
  }

  ParseNode* initNode = ExpressionStatementExpr(stmt);
  if (!initNode->isKind(ParseNodeKind::AssignExpr)) {
    return ArgFail(f, name, stmt);
  }

  ParseNode* argNode = BinaryLeft(initNode);
  ParseNode* coercionNode = BinaryRight(initNode);

  if (!IsUseOfName(argNode, name)) {
    return ArgFail(f, name, stmt);
  }

  ParseNode* coercedExpr;
  if (!CheckTypeAnnotation(f.m(), coercionNode, type, &coercedExpr)) {
    return false;
  }

  if (!type->isArgType()) {
    return f.failName(stmt, "invalid type for argument '%s'", name);
  }

  if (!IsUseOfName(coercedExpr, name)) {
    return ArgFail(f, name, stmt);
  }

  return true;
}

static bool CheckProcessingDirectives(ModuleValidatorShared& m,
                                      ParseNode** stmtIter) {
  ParseNode* stmt = *stmtIter;

  while (stmt && IsIgnoredDirective(stmt)) {
    stmt = NextNode(stmt);
  }

  *stmtIter = stmt;
  return true;
}

static bool CheckArguments(FunctionValidatorShared& f, ParseNode** stmtIter,
                           ValTypeVector* argTypes) {
  ParseNode* stmt = *stmtIter;

  unsigned numFormals;
  ParseNode* argpn = FunctionFormalParametersList(f.fn(), &numFormals);

  for (unsigned i = 0; i < numFormals;
       i++, argpn = NextNode(argpn), stmt = NextNode(stmt)) {
    TaggedParserAtomIndex name;
    if (!CheckArgument(f.m(), argpn, &name)) {
      return false;
    }

    Type type;
    if (!CheckArgumentType(f, stmt, name, &type)) {
      return false;
    }

    if (!argTypes->append(type.canonicalToValType())) {
      return false;
    }

    if (!f.addLocal(argpn, name, type)) {
      return false;
    }
  }

  *stmtIter = stmt;
  return true;
}

static bool IsLiteralOrConst(FunctionValidatorShared& f, ParseNode* pn,
                             NumLit* lit) {
  if (pn->isKind(ParseNodeKind::Name)) {
    const ModuleValidatorShared::Global* global =
        f.lookupGlobal(pn->as<NameNode>().name());
    if (!global ||
        global->which() != ModuleValidatorShared::Global::ConstantLiteral) {
      return false;
    }

    *lit = global->constLiteralValue();
    return true;
  }

  if (!IsNumericLiteral(f.m(), pn)) {
    return false;
  }

  *lit = ExtractNumericLiteral(f.m(), pn);
  return true;
}

static bool CheckFinalReturn(FunctionValidatorShared& f,
                             ParseNode* lastNonEmptyStmt) {
  if (!f.encoder().writeOp(Op::End)) {
    return false;
  }

  if (!f.hasAlreadyReturned()) {
    f.setReturnedType(Nothing());
    return true;
  }

  if (!lastNonEmptyStmt->isKind(ParseNodeKind::ReturnStmt) &&
      f.returnedType()) {
    return f.fail(lastNonEmptyStmt,
                  "void incompatible with previous return type");
  }

  return true;
}

static bool CheckVariable(FunctionValidatorShared& f, ParseNode* decl,
                          ValTypeVector* types, Vector<NumLit>* inits) {
  if (!decl->isKind(ParseNodeKind::AssignExpr)) {
    return f.failName(
        decl, "var '%s' needs explicit type declaration via an initial value",
        decl->as<NameNode>().name());
  }
  AssignmentNode* assignNode = &decl->as<AssignmentNode>();

  ParseNode* var = assignNode->left();

  if (!var->isKind(ParseNodeKind::Name)) {
    return f.fail(var, "local variable is not a plain name");
  }

  TaggedParserAtomIndex name = var->as<NameNode>().name();

  if (!CheckIdentifier(f.m(), var, name)) {
    return false;
  }

  ParseNode* initNode = assignNode->right();

  NumLit lit;
  if (!IsLiteralOrConst(f, initNode, &lit)) {
    return f.failName(
        var, "var '%s' initializer must be literal or const literal", name);
  }

  if (!lit.valid()) {
    return f.failName(var, "var '%s' initializer out of range", name);
  }

  Type type = Type::canonicalize(Type::lit(lit));

  return f.addLocal(var, name, type) &&
         types->append(type.canonicalToValType()) && inits->append(lit);
}

static bool CheckVariables(FunctionValidatorShared& f, ParseNode** stmtIter) {
  ParseNode* stmt = *stmtIter;

  uint32_t firstVar = f.numLocals();

  ValTypeVector types;
  Vector<NumLit> inits(f.fc());

  for (; stmt && stmt->isKind(ParseNodeKind::VarStmt);
       stmt = NextNonEmptyStatement(stmt)) {
    for (ParseNode* var = VarListHead(stmt); var; var = NextNode(var)) {
      if (!CheckVariable(f, var, &types, &inits)) {
        return false;
      }
    }
  }

  MOZ_ASSERT(f.encoder().empty());

  if (!EncodeLocalEntries(f.encoder(), types)) {
    return false;
  }

  for (uint32_t i = 0; i < inits.length(); i++) {
    NumLit lit = inits[i];
    if (lit.isZeroBits()) {
      continue;
    }
    if (!f.writeConstExpr(lit)) {
      return false;
    }
    if (!f.encoder().writeOp(Op::LocalSet)) {
      return false;
    }
    if (!f.encoder().writeVarU32(firstVar + i)) {
      return false;
    }
  }

  *stmtIter = stmt;
  return true;
}

template <typename Unit>
static bool CheckExpr(FunctionValidator<Unit>& f, ParseNode* expr, Type* type);

template <typename Unit>
static bool CheckNumericLiteral(FunctionValidator<Unit>& f, ParseNode* num,
                                Type* type) {
  NumLit lit = ExtractNumericLiteral(f.m(), num);
  if (!lit.valid()) {
    return f.fail(num, "numeric literal out of representable integer range");
  }
  *type = Type::lit(lit);
  return f.writeConstExpr(lit);
}

static bool CheckVarRef(FunctionValidatorShared& f, ParseNode* varRef,
                        Type* type) {
  TaggedParserAtomIndex name = varRef->as<NameNode>().name();

  if (const FunctionValidatorShared::Local* local = f.lookupLocal(name)) {
    if (!f.encoder().writeOp(Op::LocalGet)) {
      return false;
    }
    if (!f.encoder().writeVarU32(local->slot)) {
      return false;
    }
    *type = local->type;
    return true;
  }

  if (const ModuleValidatorShared::Global* global = f.lookupGlobal(name)) {
    switch (global->which()) {
      case ModuleValidatorShared::Global::ConstantLiteral:
        *type = global->varOrConstType();
        return f.writeConstExpr(global->constLiteralValue());
      case ModuleValidatorShared::Global::ConstantImport:
      case ModuleValidatorShared::Global::Variable: {
        *type = global->varOrConstType();
        return f.encoder().writeOp(Op::GlobalGet) &&
               f.encoder().writeVarU32(global->varOrConstIndex());
      }
      case ModuleValidatorShared::Global::Function:
      case ModuleValidatorShared::Global::FFI:
      case ModuleValidatorShared::Global::MathBuiltinFunction:
      case ModuleValidatorShared::Global::Table:
      case ModuleValidatorShared::Global::ArrayView:
      case ModuleValidatorShared::Global::ArrayViewCtor:
        break;
    }
    return f.failName(varRef,
                      "'%s' may not be accessed by ordinary expressions", name);
  }

  return f.failName(varRef, "'%s' not found in local or asm.js module scope",
                    name);
}

static inline bool IsLiteralOrConstInt(FunctionValidatorShared& f,
                                       ParseNode* pn, uint32_t* u32) {
  NumLit lit;
  if (!IsLiteralOrConst(f, pn, &lit)) {
    return false;
  }

  return IsLiteralInt(lit, u32);
}

static const int32_t NoMask = -1;

template <typename Unit>
static bool CheckArrayAccess(FunctionValidator<Unit>& f, ParseNode* viewName,
                             ParseNode* indexExpr, Scalar::Type* viewType) {
  if (!viewName->isKind(ParseNodeKind::Name)) {
    return f.fail(viewName,
                  "base of array access must be a typed array view name");
  }

  const ModuleValidatorShared::Global* global =
      f.lookupGlobal(viewName->as<NameNode>().name());
  if (!global || global->which() != ModuleValidatorShared::Global::ArrayView) {
    return f.fail(viewName,
                  "base of array access must be a typed array view name");
  }

  *viewType = global->viewType();

  uint32_t index;
  if (IsLiteralOrConstInt(f, indexExpr, &index)) {
    uint64_t byteOffset = uint64_t(index) << TypedArrayShift(*viewType);
    uint64_t width = TypedArrayElemSize(*viewType);
    if (!f.m().tryConstantAccess(byteOffset, width)) {
      return f.fail(indexExpr, "constant index out of range");
    }

    return f.writeInt32Lit(byteOffset);
  }

  // Mask off the low bits to account for the clearing effect of a right shift
  // followed by the left shift implicit in the array access. E.g., H32[i>>2]
  // loses the low two bits.
  int32_t mask = ~(TypedArrayElemSize(*viewType) - 1);

  if (indexExpr->isKind(ParseNodeKind::RshExpr)) {
    ParseNode* shiftAmountNode = BitwiseRight(indexExpr);

    uint32_t shift;
    if (!IsLiteralInt(f.m(), shiftAmountNode, &shift)) {
      return f.failf(shiftAmountNode, "shift amount must be constant");
    }

    unsigned requiredShift = TypedArrayShift(*viewType);
    if (shift != requiredShift) {
      return f.failf(shiftAmountNode, "shift amount must be %u", requiredShift);
    }

    ParseNode* pointerNode = BitwiseLeft(indexExpr);

    Type pointerType;
    if (!CheckExpr(f, pointerNode, &pointerType)) {
      return false;
    }

    if (!pointerType.isIntish()) {
      return f.failf(pointerNode, "%s is not a subtype of int",
                     pointerType.toChars());
    }
  } else {
    // For legacy scalar access compatibility, accept Int8/Uint8 accesses
    // with no shift.
    if (TypedArrayShift(*viewType) != 0) {
      return f.fail(
          indexExpr,
          "index expression isn't shifted; must be an Int8/Uint8 access");
    }

    MOZ_ASSERT(mask == NoMask);

    ParseNode* pointerNode = indexExpr;

    Type pointerType;
    if (!CheckExpr(f, pointerNode, &pointerType)) {
      return false;
    }
    if (!pointerType.isInt()) {
      return f.failf(pointerNode, "%s is not a subtype of int",
                     pointerType.toChars());
    }
  }

  // Don't generate the mask op if there is no need for it which could happen
  // for a shift of zero.
  if (mask != NoMask) {
    return f.writeInt32Lit(mask) && f.encoder().writeOp(Op::I32And);
  }

  return true;
}

static bool WriteArrayAccessFlags(FunctionValidatorShared& f,
                                  Scalar::Type viewType) {
  // asm.js only has naturally-aligned accesses.
  size_t align = TypedArrayElemSize(viewType);
  MOZ_ASSERT(IsPowerOfTwo(align));
  if (!f.encoder().writeFixedU8(CeilingLog2(align))) {
    return false;
  }

  // asm.js doesn't have constant offsets, so just encode a 0.
  return f.encoder().writeVarU32(0);
}

template <typename Unit>
static bool CheckLoadArray(FunctionValidator<Unit>& f, ParseNode* elem,
                           Type* type) {
  Scalar::Type viewType;

  if (!CheckArrayAccess(f, ElemBase(elem), ElemIndex(elem), &viewType)) {
    return false;
  }

  switch (viewType) {
    case Scalar::Int8:
      if (!f.encoder().writeOp(Op::I32Load8S)) return false;
      break;
    case Scalar::Uint8:
      if (!f.encoder().writeOp(Op::I32Load8U)) return false;
      break;
    case Scalar::Int16:
      if (!f.encoder().writeOp(Op::I32Load16S)) return false;
      break;
    case Scalar::Uint16:
      if (!f.encoder().writeOp(Op::I32Load16U)) return false;
      break;
    case Scalar::Uint32:
    case Scalar::Int32:
      if (!f.encoder().writeOp(Op::I32Load)) return false;
      break;
    case Scalar::Float32:
      if (!f.encoder().writeOp(Op::F32Load)) return false;
      break;
    case Scalar::Float64:
      if (!f.encoder().writeOp(Op::F64Load)) return false;
      break;
    default:
      MOZ_CRASH("unexpected scalar type");
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
    default:
      MOZ_CRASH("Unexpected array type");
  }

  return WriteArrayAccessFlags(f, viewType);
}

template <typename Unit>
static bool CheckStoreArray(FunctionValidator<Unit>& f, ParseNode* lhs,
                            ParseNode* rhs, Type* type) {
  Scalar::Type viewType;
  if (!CheckArrayAccess(f, ElemBase(lhs), ElemIndex(lhs), &viewType)) {
    return false;
  }

  Type rhsType;
  if (!CheckExpr(f, rhs, &rhsType)) {
    return false;
  }

  switch (viewType) {
    case Scalar::Int8:
    case Scalar::Int16:
    case Scalar::Int32:
    case Scalar::Uint8:
    case Scalar::Uint16:
    case Scalar::Uint32:
      if (!rhsType.isIntish()) {
        return f.failf(lhs, "%s is not a subtype of intish", rhsType.toChars());
      }
      break;
    case Scalar::Float32:
      if (!rhsType.isMaybeDouble() && !rhsType.isFloatish()) {
        return f.failf(lhs, "%s is not a subtype of double? or floatish",
                       rhsType.toChars());
      }
      break;
    case Scalar::Float64:
      if (!rhsType.isMaybeFloat() && !rhsType.isMaybeDouble()) {
        return f.failf(lhs, "%s is not a subtype of float? or double?",
                       rhsType.toChars());
      }
      break;
    default:
      MOZ_CRASH("Unexpected view type");
  }

  switch (viewType) {
    case Scalar::Int8:
    case Scalar::Uint8:
      if (!f.encoder().writeOp(MozOp::I32TeeStore8)) {
        return false;
      }
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
      if (!f.encoder().writeOp(MozOp::I32TeeStore16)) {
        return false;
      }
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      if (!f.encoder().writeOp(MozOp::I32TeeStore)) {
        return false;
      }
      break;
    case Scalar::Float32:
      if (rhsType.isFloatish()) {
        if (!f.encoder().writeOp(MozOp::F32TeeStore)) {
          return false;
        }
      } else {
        if (!f.encoder().writeOp(MozOp::F64TeeStoreF32)) {
          return false;
        }
      }
      break;
    case Scalar::Float64:
      if (rhsType.isFloatish()) {
        if (!f.encoder().writeOp(MozOp::F32TeeStoreF64)) {
          return false;
        }
      } else {
        if (!f.encoder().writeOp(MozOp::F64TeeStore)) {
          return false;
        }
      }
      break;
    default:
      MOZ_CRASH("unexpected scalar type");
  }

  if (!WriteArrayAccessFlags(f, viewType)) {
    return false;
  }

  *type = rhsType;
  return true;
}

template <typename Unit>
static bool CheckAssignName(FunctionValidator<Unit>& f, ParseNode* lhs,
                            ParseNode* rhs, Type* type) {
  TaggedParserAtomIndex name = lhs->as<NameNode>().name();

  if (const FunctionValidatorShared::Local* lhsVar = f.lookupLocal(name)) {
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType)) {
      return false;
    }

    if (!f.encoder().writeOp(Op::LocalTee)) {
      return false;
    }
    if (!f.encoder().writeVarU32(lhsVar->slot)) {
      return false;
    }

    if (!(rhsType <= lhsVar->type)) {
      return f.failf(lhs, "%s is not a subtype of %s", rhsType.toChars(),
                     lhsVar->type.toChars());
    }
    *type = rhsType;
    return true;
  }

  if (const ModuleValidatorShared::Global* global = f.lookupGlobal(name)) {
    if (global->which() != ModuleValidatorShared::Global::Variable) {
      return f.failName(lhs, "'%s' is not a mutable variable", name);
    }

    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType)) {
      return false;
    }

    Type globType = global->varOrConstType();
    if (!(rhsType <= globType)) {
      return f.failf(lhs, "%s is not a subtype of %s", rhsType.toChars(),
                     globType.toChars());
    }
    if (!f.encoder().writeOp(MozOp::TeeGlobal)) {
      return false;
    }
    if (!f.encoder().writeVarU32(global->varOrConstIndex())) {
      return false;
    }

    *type = rhsType;
    return true;
  }

  return f.failName(lhs, "'%s' not found in local or asm.js module scope",
                    name);
}

template <typename Unit>
static bool CheckAssign(FunctionValidator<Unit>& f, ParseNode* assign,
                        Type* type) {
  MOZ_ASSERT(assign->isKind(ParseNodeKind::AssignExpr));

  ParseNode* lhs = BinaryLeft(assign);
  ParseNode* rhs = BinaryRight(assign);

  if (lhs->getKind() == ParseNodeKind::ElemExpr) {
    return CheckStoreArray(f, lhs, rhs, type);
  }

  if (lhs->getKind() == ParseNodeKind::Name) {
    return CheckAssignName(f, lhs, rhs, type);
  }

  return f.fail(
      assign,
      "left-hand side of assignment must be a variable or array access");
}

template <typename Unit>
static bool CheckMathIMul(FunctionValidator<Unit>& f, ParseNode* call,
                          Type* type) {
  if (CallArgListLength(call) != 2) {
    return f.fail(call, "Math.imul must be passed 2 arguments");
  }

  ParseNode* lhs = CallArgList(call);
  ParseNode* rhs = NextNode(lhs);

  Type lhsType;
  if (!CheckExpr(f, lhs, &lhsType)) {
    return false;
  }

  Type rhsType;
  if (!CheckExpr(f, rhs, &rhsType)) {
    return false;
  }

  if (!lhsType.isIntish()) {
    return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
  }
  if (!rhsType.isIntish()) {
    return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());
  }

  *type = Type::Signed;
  return f.encoder().writeOp(Op::I32Mul);
}

template <typename Unit>
static bool CheckMathClz32(FunctionValidator<Unit>& f, ParseNode* call,
                           Type* type) {
  if (CallArgListLength(call) != 1) {
    return f.fail(call, "Math.clz32 must be passed 1 argument");
  }

  ParseNode* arg = CallArgList(call);

  Type argType;
  if (!CheckExpr(f, arg, &argType)) {
    return false;
  }

  if (!argType.isIntish()) {
    return f.failf(arg, "%s is not a subtype of intish", argType.toChars());
  }

  *type = Type::Fixnum;
  return f.encoder().writeOp(Op::I32Clz);
}

template <typename Unit>
static bool CheckMathAbs(FunctionValidator<Unit>& f, ParseNode* call,
                         Type* type) {
  if (CallArgListLength(call) != 1) {
    return f.fail(call, "Math.abs must be passed 1 argument");
  }

  ParseNode* arg = CallArgList(call);

  Type argType;
  if (!CheckExpr(f, arg, &argType)) {
    return false;
  }

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

  return f.failf(call, "%s is not a subtype of signed, float? or double?",
                 argType.toChars());
}

template <typename Unit>
static bool CheckMathSqrt(FunctionValidator<Unit>& f, ParseNode* call,
                          Type* type) {
  if (CallArgListLength(call) != 1) {
    return f.fail(call, "Math.sqrt must be passed 1 argument");
  }

  ParseNode* arg = CallArgList(call);

  Type argType;
  if (!CheckExpr(f, arg, &argType)) {
    return false;
  }

  if (argType.isMaybeDouble()) {
    *type = Type::Double;
    return f.encoder().writeOp(Op::F64Sqrt);
  }

  if (argType.isMaybeFloat()) {
    *type = Type::Floatish;
    return f.encoder().writeOp(Op::F32Sqrt);
  }

  return f.failf(call, "%s is neither a subtype of double? nor float?",
                 argType.toChars());
}

template <typename Unit>
static bool CheckMathMinMax(FunctionValidator<Unit>& f, ParseNode* callNode,
                            bool isMax, Type* type) {
  if (CallArgListLength(callNode) < 2) {
    return f.fail(callNode, "Math.min/max must be passed at least 2 arguments");
  }

  ParseNode* firstArg = CallArgList(callNode);
  Type firstType;
  if (!CheckExpr(f, firstArg, &firstType)) {
    return false;
  }

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
    if (!CheckExpr(f, nextArg, &nextType)) {
      return false;
    }
    if (!(nextType <= firstType)) {
      return f.failf(nextArg, "%s is not a subtype of %s", nextType.toChars(),
                     firstType.toChars());
    }

    if (op != Op::Limit) {
      if (!f.encoder().writeOp(op)) {
        return false;
      }
    } else {
      if (!f.encoder().writeOp(mozOp)) {
        return false;
      }
    }
  }

  return true;
}

using CheckArgType = bool (*)(FunctionValidatorShared& f, ParseNode* argNode,
                              Type type);

template <CheckArgType checkArg, typename Unit>
static bool CheckCallArgs(FunctionValidator<Unit>& f, ParseNode* callNode,
                          ValTypeVector* args) {
  ParseNode* argNode = CallArgList(callNode);
  for (unsigned i = 0; i < CallArgListLength(callNode);
       i++, argNode = NextNode(argNode)) {
    Type type;
    if (!CheckExpr(f, argNode, &type)) {
      return false;
    }

    if (!checkArg(f, argNode, type)) {
      return false;
    }

    if (!args->append(Type::canonicalize(type).canonicalToValType())) {
      return false;
    }
  }
  return true;
}

static bool CheckSignatureAgainstExisting(ModuleValidatorShared& m,
                                          ParseNode* usepn, const FuncType& sig,
                                          const FuncType& existing) {
  if (!FuncType::strictlyEquals(sig, existing)) {
    return m.failf(usepn, "incompatible argument types to function");
  }
  return true;
}

template <typename Unit>
static bool CheckFunctionSignature(ModuleValidator<Unit>& m, ParseNode* usepn,
                                   FuncType&& sig, TaggedParserAtomIndex name,
                                   ModuleValidatorShared::Func** func) {
  if (sig.args().length() > MaxParams) {
    return m.failf(usepn, "too many parameters");
  }

  ModuleValidatorShared::Func* existing = m.lookupFuncDef(name);
  if (!existing) {
    if (!CheckModuleLevelName(m, usepn, name)) {
      return false;
    }
    return m.addFuncDef(name, usepn->pn_pos.begin, std::move(sig), func);
  }

  const FuncType& existingSig =
      m.env().types->type(existing->sigIndex()).funcType();

  if (!CheckSignatureAgainstExisting(m, usepn, sig, existingSig)) {
    return false;
  }

  *func = existing;
  return true;
}

static bool CheckIsArgType(FunctionValidatorShared& f, ParseNode* argNode,
                           Type type) {
  if (!type.isArgType()) {
    return f.failf(argNode, "%s is not a subtype of int, float, or double",
                   type.toChars());
  }
  return true;
}

template <typename Unit>
static bool CheckInternalCall(FunctionValidator<Unit>& f, ParseNode* callNode,
                              TaggedParserAtomIndex calleeName, Type ret,
                              Type* type) {
  MOZ_ASSERT(ret.isCanonical());

  ValTypeVector args;
  if (!CheckCallArgs<CheckIsArgType>(f, callNode, &args)) {
    return false;
  }

  ValTypeVector results;
  Maybe<ValType> retType = ret.canonicalToReturnType();
  if (retType && !results.append(retType.ref())) {
    return false;
  }

  FuncType sig(std::move(args), std::move(results));

  ModuleValidatorShared::Func* callee;
  if (!CheckFunctionSignature(f.m(), callNode, std::move(sig), calleeName,
                              &callee)) {
    return false;
  }

  if (!f.writeCall(callNode, MozOp::OldCallDirect)) {
    return false;
  }

  if (!f.encoder().writeVarU32(callee->funcDefIndex())) {
    return false;
  }

  *type = Type::ret(ret);
  return true;
}

template <typename Unit>
static bool CheckFuncPtrTableAgainstExisting(ModuleValidator<Unit>& m,
                                             ParseNode* usepn,
                                             TaggedParserAtomIndex name,
                                             FuncType&& sig, unsigned mask,
                                             uint32_t* tableIndex) {
  if (const ModuleValidatorShared::Global* existing = m.lookupGlobal(name)) {
    if (existing->which() != ModuleValidatorShared::Global::Table) {
      return m.failName(usepn, "'%s' is not a function-pointer table", name);
    }

    ModuleValidatorShared::Table& table = m.table(existing->tableIndex());
    if (mask != table.mask()) {
      return m.failf(usepn, "mask does not match previous value (%u)",
                     table.mask());
    }

    if (!CheckSignatureAgainstExisting(
            m, usepn, sig, m.env().types->type(table.sigIndex()).funcType())) {
      return false;
    }

    *tableIndex = existing->tableIndex();
    return true;
  }

  if (!CheckModuleLevelName(m, usepn, name)) {
    return false;
  }

  return m.declareFuncPtrTable(std::move(sig), name, usepn->pn_pos.begin, mask,
                               tableIndex);
}

template <typename Unit>
static bool CheckFuncPtrCall(FunctionValidator<Unit>& f, ParseNode* callNode,
                             Type ret, Type* type) {
  MOZ_ASSERT(ret.isCanonical());

  ParseNode* callee = CallCallee(callNode);
  ParseNode* tableNode = ElemBase(callee);
  ParseNode* indexExpr = ElemIndex(callee);

  if (!tableNode->isKind(ParseNodeKind::Name)) {
    return f.fail(tableNode, "expecting name of function-pointer array");
  }

  TaggedParserAtomIndex name = tableNode->as<NameNode>().name();
  if (const ModuleValidatorShared::Global* existing = f.lookupGlobal(name)) {
    if (existing->which() != ModuleValidatorShared::Global::Table) {
      return f.failName(
          tableNode, "'%s' is not the name of a function-pointer array", name);
    }
  }

  if (!indexExpr->isKind(ParseNodeKind::BitAndExpr)) {
    return f.fail(indexExpr,
                  "function-pointer table index expression needs & mask");
  }

  ParseNode* indexNode = BitwiseLeft(indexExpr);
  ParseNode* maskNode = BitwiseRight(indexExpr);

  uint32_t mask;
  if (!IsLiteralInt(f.m(), maskNode, &mask) || mask == UINT32_MAX ||
      !IsPowerOfTwo(mask + 1)) {
    return f.fail(maskNode,
                  "function-pointer table index mask value must be a power of "
                  "two minus 1");
  }

  Type indexType;
  if (!CheckExpr(f, indexNode, &indexType)) {
    return false;
  }

  if (!indexType.isIntish()) {
    return f.failf(indexNode, "%s is not a subtype of intish",
                   indexType.toChars());
  }

  ValTypeVector args;
  if (!CheckCallArgs<CheckIsArgType>(f, callNode, &args)) {
    return false;
  }

  ValTypeVector results;
  Maybe<ValType> retType = ret.canonicalToReturnType();
  if (retType && !results.append(retType.ref())) {
    return false;
  }

  FuncType sig(std::move(args), std::move(results));

  uint32_t tableIndex;
  if (!CheckFuncPtrTableAgainstExisting(f.m(), tableNode, name, std::move(sig),
                                        mask, &tableIndex)) {
    return false;
  }

  if (!f.writeCall(callNode, MozOp::OldCallIndirect)) {
    return false;
  }

  // Call signature
  if (!f.encoder().writeVarU32(f.m().table(tableIndex).sigIndex())) {
    return false;
  }

  *type = Type::ret(ret);
  return true;
}

static bool CheckIsExternType(FunctionValidatorShared& f, ParseNode* argNode,
                              Type type) {
  if (!type.isExtern()) {
    return f.failf(argNode, "%s is not a subtype of extern", type.toChars());
  }
  return true;
}

template <typename Unit>
static bool CheckFFICall(FunctionValidator<Unit>& f, ParseNode* callNode,
                         unsigned ffiIndex, Type ret, Type* type) {
  MOZ_ASSERT(ret.isCanonical());

  TaggedParserAtomIndex calleeName =
      CallCallee(callNode)->as<NameNode>().name();

  if (ret.isFloat()) {
    return f.fail(callNode, "FFI calls can't return float");
  }

  ValTypeVector args;
  if (!CheckCallArgs<CheckIsExternType>(f, callNode, &args)) {
    return false;
  }

  ValTypeVector results;
  Maybe<ValType> retType = ret.canonicalToReturnType();
  if (retType && !results.append(retType.ref())) {
    return false;
  }

  FuncType sig(std::move(args), std::move(results));

  uint32_t importIndex;
  if (!f.m().declareImport(calleeName, std::move(sig), ffiIndex,
                           &importIndex)) {
    return false;
  }

  if (!f.writeCall(callNode, Op::Call)) {
    return false;
  }

  if (!f.encoder().writeVarU32(importIndex)) {
    return false;
  }

  *type = Type::ret(ret);
  return true;
}

static bool CheckFloatCoercionArg(FunctionValidatorShared& f,
                                  ParseNode* inputNode, Type inputType) {
  if (inputType.isMaybeDouble()) {
    return f.encoder().writeOp(Op::F32DemoteF64);
  }
  if (inputType.isSigned()) {
    return f.encoder().writeOp(Op::F32ConvertI32S);
  }
  if (inputType.isUnsigned()) {
    return f.encoder().writeOp(Op::F32ConvertI32U);
  }
  if (inputType.isFloatish()) {
    return true;
  }

  return f.failf(inputNode,
                 "%s is not a subtype of signed, unsigned, double? or floatish",
                 inputType.toChars());
}

template <typename Unit>
static bool CheckCoercedCall(FunctionValidator<Unit>& f, ParseNode* call,
                             Type ret, Type* type);

template <typename Unit>
static bool CheckCoercionArg(FunctionValidator<Unit>& f, ParseNode* arg,
                             Type expected, Type* type) {
  MOZ_ASSERT(expected.isCanonicalValType());

  if (arg->isKind(ParseNodeKind::CallExpr)) {
    return CheckCoercedCall(f, arg, expected, type);
  }

  Type argType;
  if (!CheckExpr(f, arg, &argType)) {
    return false;
  }

  if (expected.isFloat()) {
    if (!CheckFloatCoercionArg(f, arg, argType)) {
      return false;
    }
  } else {
    MOZ_CRASH("not call coercions");
  }

  *type = Type::ret(expected);
  return true;
}

template <typename Unit>
static bool CheckMathFRound(FunctionValidator<Unit>& f, ParseNode* callNode,
                            Type* type) {
  if (CallArgListLength(callNode) != 1) {
    return f.fail(callNode, "Math.fround must be passed 1 argument");
  }

  ParseNode* argNode = CallArgList(callNode);
  Type argType;
  if (!CheckCoercionArg(f, argNode, Type::Float, &argType)) {
    return false;
  }

  MOZ_ASSERT(argType == Type::Float);
  *type = Type::Float;
  return true;
}

template <typename Unit>
static bool CheckMathBuiltinCall(FunctionValidator<Unit>& f,
                                 ParseNode* callNode,
                                 AsmJSMathBuiltinFunction func, Type* type) {
  unsigned arity = 0;
  Op f32 = Op::Limit;
  Op f64 = Op::Limit;
  MozOp mozf64 = MozOp::Limit;
  switch (func) {
    case AsmJSMathBuiltin_imul:
      return CheckMathIMul(f, callNode, type);
    case AsmJSMathBuiltin_clz32:
      return CheckMathClz32(f, callNode, type);
    case AsmJSMathBuiltin_abs:
      return CheckMathAbs(f, callNode, type);
    case AsmJSMathBuiltin_sqrt:
      return CheckMathSqrt(f, callNode, type);
    case AsmJSMathBuiltin_fround:
      return CheckMathFRound(f, callNode, type);
    case AsmJSMathBuiltin_min:
      return CheckMathMinMax(f, callNode, /* isMax = */ false, type);
    case AsmJSMathBuiltin_max:
      return CheckMathMinMax(f, callNode, /* isMax = */ true, type);
    case AsmJSMathBuiltin_ceil:
      arity = 1;
      f64 = Op::F64Ceil;
      f32 = Op::F32Ceil;
      break;
    case AsmJSMathBuiltin_floor:
      arity = 1;
      f64 = Op::F64Floor;
      f32 = Op::F32Floor;
      break;
    case AsmJSMathBuiltin_sin:
      arity = 1;
      if (!f.m().alwaysUseFdlibm()) {
        mozf64 = MozOp::F64SinNative;
      } else {
        mozf64 = MozOp::F64SinFdlibm;
      }
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_cos:
      arity = 1;
      if (!f.m().alwaysUseFdlibm()) {
        mozf64 = MozOp::F64CosNative;
      } else {
        mozf64 = MozOp::F64CosFdlibm;
      }
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_tan:
      arity = 1;
      if (!f.m().alwaysUseFdlibm()) {
        mozf64 = MozOp::F64TanNative;
      } else {
        mozf64 = MozOp::F64TanFdlibm;
      }
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_asin:
      arity = 1;
      mozf64 = MozOp::F64Asin;
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_acos:
      arity = 1;
      mozf64 = MozOp::F64Acos;
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_atan:
      arity = 1;
      mozf64 = MozOp::F64Atan;
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_exp:
      arity = 1;
      mozf64 = MozOp::F64Exp;
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_log:
      arity = 1;
      mozf64 = MozOp::F64Log;
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_pow:
      arity = 2;
      mozf64 = MozOp::F64Pow;
      f32 = Op::Unreachable;
      break;
    case AsmJSMathBuiltin_atan2:
      arity = 2;
      mozf64 = MozOp::F64Atan2;
      f32 = Op::Unreachable;
      break;
    default:
      MOZ_CRASH("unexpected mathBuiltin function");
  }

  unsigned actualArity = CallArgListLength(callNode);
  if (actualArity != arity) {
    return f.failf(callNode, "call passed %u arguments, expected %u",
                   actualArity, arity);
  }

  if (!f.prepareCall(callNode)) {
    return false;
  }

  Type firstType;
  ParseNode* argNode = CallArgList(callNode);
  if (!CheckExpr(f, argNode, &firstType)) {
    return false;
  }

  if (!firstType.isMaybeFloat() && !firstType.isMaybeDouble()) {
    return f.fail(
        argNode,
        "arguments to math call should be a subtype of double? or float?");
  }

  bool opIsDouble = firstType.isMaybeDouble();
  if (!opIsDouble && f32 == Op::Unreachable) {
    return f.fail(callNode, "math builtin cannot be used as float");
  }

  if (arity == 2) {
    Type secondType;
    argNode = NextNode(argNode);
    if (!CheckExpr(f, argNode, &secondType)) {
      return false;
    }

    if (firstType.isMaybeDouble() && !secondType.isMaybeDouble()) {
      return f.fail(
          argNode,
          "both arguments to math builtin call should be the same type");
    }
    if (firstType.isMaybeFloat() && !secondType.isMaybeFloat()) {
      return f.fail(
          argNode,
          "both arguments to math builtin call should be the same type");
    }
  }

  if (opIsDouble) {
    if (f64 != Op::Limit) {
      if (!f.encoder().writeOp(f64)) {
        return false;
      }
    } else {
      if (!f.encoder().writeOp(mozf64)) {
        return false;
      }
    }
  } else {
    if (!f.encoder().writeOp(f32)) {
      return false;
    }
  }

  *type = opIsDouble ? Type::Double : Type::Floatish;
  return true;
}

template <typename Unit>
static bool CheckUncoercedCall(FunctionValidator<Unit>& f, ParseNode* expr,
                               Type* type) {
  MOZ_ASSERT(expr->isKind(ParseNodeKind::CallExpr));

  const ModuleValidatorShared::Global* global;
  if (IsCallToGlobal(f.m(), expr, &global) && global->isMathFunction()) {
    return CheckMathBuiltinCall(f, expr, global->mathBuiltinFunction(), type);
  }

  return f.fail(
      expr,
      "all function calls must be calls to standard lib math functions,"
      " ignored (via f(); or comma-expression), coerced to signed (via f()|0),"
      " coerced to float (via fround(f())), or coerced to double (via +f())");
}

static bool CoerceResult(FunctionValidatorShared& f, ParseNode* expr,
                         Type expected, Type actual, Type* type) {
  MOZ_ASSERT(expected.isCanonical());

  // At this point, the bytecode resembles this:
  //      | the thing we wanted to coerce | current position |>
  switch (expected.which()) {
    case Type::Void:
      if (!actual.isVoid()) {
        if (!f.encoder().writeOp(Op::Drop)) {
          return false;
        }
      }
      break;
    case Type::Int:
      if (!actual.isIntish()) {
        return f.failf(expr, "%s is not a subtype of intish", actual.toChars());
      }
      break;
    case Type::Float:
      if (!CheckFloatCoercionArg(f, expr, actual)) {
        return false;
      }
      break;
    case Type::Double:
      if (actual.isMaybeDouble()) {
        // No conversion necessary.
      } else if (actual.isMaybeFloat()) {
        if (!f.encoder().writeOp(Op::F64PromoteF32)) {
          return false;
        }
      } else if (actual.isSigned()) {
        if (!f.encoder().writeOp(Op::F64ConvertI32S)) {
          return false;
        }
      } else if (actual.isUnsigned()) {
        if (!f.encoder().writeOp(Op::F64ConvertI32U)) {
          return false;
        }
      } else {
        return f.failf(
            expr, "%s is not a subtype of double?, float?, signed or unsigned",
            actual.toChars());
      }
      break;
    default:
      MOZ_CRASH("unexpected uncoerced result type");
  }

  *type = Type::ret(expected);
  return true;
}

template <typename Unit>
static bool CheckCoercedMathBuiltinCall(FunctionValidator<Unit>& f,
                                        ParseNode* callNode,
                                        AsmJSMathBuiltinFunction func, Type ret,
                                        Type* type) {
  Type actual;
  if (!CheckMathBuiltinCall(f, callNode, func, &actual)) {
    return false;
  }
  return CoerceResult(f, callNode, ret, actual, type);
}

template <typename Unit>
static bool CheckCoercedCall(FunctionValidator<Unit>& f, ParseNode* call,
                             Type ret, Type* type) {
  MOZ_ASSERT(ret.isCanonical());

  AutoCheckRecursionLimit recursion(f.fc());
  if (!recursion.checkDontReport(f.fc())) {
    return f.m().failOverRecursed();
  }

  if (IsNumericLiteral(f.m(), call)) {
    NumLit lit = ExtractNumericLiteral(f.m(), call);
    if (!f.writeConstExpr(lit)) {
      return false;
    }
    return CoerceResult(f, call, ret, Type::lit(lit), type);
  }

  ParseNode* callee = CallCallee(call);

  if (callee->isKind(ParseNodeKind::ElemExpr)) {
    return CheckFuncPtrCall(f, call, ret, type);
  }

  if (!callee->isKind(ParseNodeKind::Name)) {
    return f.fail(callee, "unexpected callee expression type");
  }

  TaggedParserAtomIndex calleeName = callee->as<NameNode>().name();

  if (const ModuleValidatorShared::Global* global =
          f.lookupGlobal(calleeName)) {
    switch (global->which()) {
      case ModuleValidatorShared::Global::FFI:
        return CheckFFICall(f, call, global->ffiIndex(), ret, type);
      case ModuleValidatorShared::Global::MathBuiltinFunction:
        return CheckCoercedMathBuiltinCall(
            f, call, global->mathBuiltinFunction(), ret, type);
      case ModuleValidatorShared::Global::ConstantLiteral:
      case ModuleValidatorShared::Global::ConstantImport:
      case ModuleValidatorShared::Global::Variable:
      case ModuleValidatorShared::Global::Table:
      case ModuleValidatorShared::Global::ArrayView:
      case ModuleValidatorShared::Global::ArrayViewCtor:
        return f.failName(callee, "'%s' is not callable function", calleeName);
      case ModuleValidatorShared::Global::Function:
        break;
    }
  }

  return CheckInternalCall(f, call, calleeName, ret, type);
}

template <typename Unit>
static bool CheckPos(FunctionValidator<Unit>& f, ParseNode* pos, Type* type) {
  MOZ_ASSERT(pos->isKind(ParseNodeKind::PosExpr));
  ParseNode* operand = UnaryKid(pos);

  if (operand->isKind(ParseNodeKind::CallExpr)) {
    return CheckCoercedCall(f, operand, Type::Double, type);
  }

  Type actual;
  if (!CheckExpr(f, operand, &actual)) {
    return false;
  }

  return CoerceResult(f, operand, Type::Double, actual, type);
}

template <typename Unit>
static bool CheckNot(FunctionValidator<Unit>& f, ParseNode* expr, Type* type) {
  MOZ_ASSERT(expr->isKind(ParseNodeKind::NotExpr));
  ParseNode* operand = UnaryKid(expr);

  Type operandType;
  if (!CheckExpr(f, operand, &operandType)) {
    return false;
  }

  if (!operandType.isInt()) {
    return f.failf(operand, "%s is not a subtype of int",
                   operandType.toChars());
  }

  *type = Type::Int;
  return f.encoder().writeOp(Op::I32Eqz);
}

template <typename Unit>
static bool CheckNeg(FunctionValidator<Unit>& f, ParseNode* expr, Type* type) {
  MOZ_ASSERT(expr->isKind(ParseNodeKind::NegExpr));
  ParseNode* operand = UnaryKid(expr);

  Type operandType;
  if (!CheckExpr(f, operand, &operandType)) {
    return false;
  }

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

  return f.failf(operand, "%s is not a subtype of int, float? or double?",
                 operandType.toChars());
}

template <typename Unit>
static bool CheckCoerceToInt(FunctionValidator<Unit>& f, ParseNode* expr,
                             Type* type) {
  MOZ_ASSERT(expr->isKind(ParseNodeKind::BitNotExpr));
  ParseNode* operand = UnaryKid(expr);

  Type operandType;
  if (!CheckExpr(f, operand, &operandType)) {
    return false;
  }

  if (operandType.isMaybeDouble() || operandType.isMaybeFloat()) {
    *type = Type::Signed;
    Op opcode =
        operandType.isMaybeDouble() ? Op::I32TruncF64S : Op::I32TruncF32S;
    return f.encoder().writeOp(opcode);
  }

  if (!operandType.isIntish()) {
    return f.failf(operand, "%s is not a subtype of double?, float? or intish",
                   operandType.toChars());
  }

  *type = Type::Signed;
  return true;
}

template <typename Unit>
static bool CheckBitNot(FunctionValidator<Unit>& f, ParseNode* neg,
                        Type* type) {
  MOZ_ASSERT(neg->isKind(ParseNodeKind::BitNotExpr));
  ParseNode* operand = UnaryKid(neg);

  if (operand->isKind(ParseNodeKind::BitNotExpr)) {
    return CheckCoerceToInt(f, operand, type);
  }

  Type operandType;
  if (!CheckExpr(f, operand, &operandType)) {
    return false;
  }

  if (!operandType.isIntish()) {
    return f.failf(operand, "%s is not a subtype of intish",
                   operandType.toChars());
  }

  if (!f.encoder().writeOp(MozOp::I32BitNot)) {
    return false;
  }

  *type = Type::Signed;
  return true;
}

template <typename Unit>
static bool CheckAsExprStatement(FunctionValidator<Unit>& f,
                                 ParseNode* exprStmt);

template <typename Unit>
static bool CheckComma(FunctionValidator<Unit>& f, ParseNode* comma,
                       Type* type) {
  MOZ_ASSERT(comma->isKind(ParseNodeKind::CommaExpr));
  ParseNode* operands = ListHead(comma);

  // The block depth isn't taken into account here, because a comma list can't
  // contain breaks and continues and nested control flow structures.
  if (!f.encoder().writeOp(Op::Block)) {
    return false;
  }

  size_t typeAt;
  if (!f.encoder().writePatchableFixedU7(&typeAt)) {
    return false;
  }

  ParseNode* pn = operands;
  for (; NextNode(pn); pn = NextNode(pn)) {
    if (!CheckAsExprStatement(f, pn)) {
      return false;
    }
  }

  if (!CheckExpr(f, pn, type)) {
    return false;
  }

  f.encoder().patchFixedU7(typeAt, uint8_t(type->toWasmBlockSignatureType()));

  return f.encoder().writeOp(Op::End);
}

template <typename Unit>
static bool CheckConditional(FunctionValidator<Unit>& f, ParseNode* ternary,
                             Type* type) {
  MOZ_ASSERT(ternary->isKind(ParseNodeKind::ConditionalExpr));

  ParseNode* cond = TernaryKid1(ternary);
  ParseNode* thenExpr = TernaryKid2(ternary);
  ParseNode* elseExpr = TernaryKid3(ternary);

  Type condType;
  if (!CheckExpr(f, cond, &condType)) {
    return false;
  }

  if (!condType.isInt()) {
    return f.failf(cond, "%s is not a subtype of int", condType.toChars());
  }

  size_t typeAt;
  if (!f.pushIf(&typeAt)) {
    return false;
  }

  Type thenType;
  if (!CheckExpr(f, thenExpr, &thenType)) {
    return false;
  }

  if (!f.switchToElse()) {
    return false;
  }

  Type elseType;
  if (!CheckExpr(f, elseExpr, &elseType)) {
    return false;
  }

  if (thenType.isInt() && elseType.isInt()) {
    *type = Type::Int;
  } else if (thenType.isDouble() && elseType.isDouble()) {
    *type = Type::Double;
  } else if (thenType.isFloat() && elseType.isFloat()) {
    *type = Type::Float;
  } else {
    return f.failf(
        ternary,
        "then/else branches of conditional must both produce int, float, "
        "double, current types are %s and %s",
        thenType.toChars(), elseType.toChars());
  }

  return f.popIf(typeAt, type->toWasmBlockSignatureType());
}

template <typename Unit>
static bool IsValidIntMultiplyConstant(ModuleValidator<Unit>& m,
                                       ParseNode* expr) {
  if (!IsNumericLiteral(m, expr)) {
    return false;
  }

  NumLit lit = ExtractNumericLiteral(m, expr);
  switch (lit.which()) {
    case NumLit::Fixnum:
    case NumLit::NegativeInt:
      if (Abs(lit.toInt32()) < (uint32_t(1) << 20)) {
        return true;
      }
      return false;
    case NumLit::BigUnsigned:
    case NumLit::Double:
    case NumLit::Float:
    case NumLit::OutOfRangeInt:
      return false;
  }

  MOZ_CRASH("Bad literal");
}

template <typename Unit>
static bool CheckMultiply(FunctionValidator<Unit>& f, ParseNode* star,
                          Type* type) {
  MOZ_ASSERT(star->isKind(ParseNodeKind::MulExpr));
  ParseNode* lhs = MultiplyLeft(star);
  ParseNode* rhs = MultiplyRight(star);

  Type lhsType;
  if (!CheckExpr(f, lhs, &lhsType)) {
    return false;
  }

  Type rhsType;
  if (!CheckExpr(f, rhs, &rhsType)) {
    return false;
  }

  if (lhsType.isInt() && rhsType.isInt()) {
    if (!IsValidIntMultiplyConstant(f.m(), lhs) &&
        !IsValidIntMultiplyConstant(f.m(), rhs)) {
      return f.fail(
          star,
          "one arg to int multiply must be a small (-2^20, 2^20) int literal");
    }
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

  return f.fail(
      star, "multiply operands must be both int, both double? or both float?");
}

template <typename Unit>
static bool CheckAddOrSub(FunctionValidator<Unit>& f, ParseNode* expr,
                          Type* type, unsigned* numAddOrSubOut = nullptr) {
  AutoCheckRecursionLimit recursion(f.fc());
  if (!recursion.checkDontReport(f.fc())) {
    return f.m().failOverRecursed();
  }

  MOZ_ASSERT(expr->isKind(ParseNodeKind::AddExpr) ||
             expr->isKind(ParseNodeKind::SubExpr));
  ParseNode* lhs = AddSubLeft(expr);
  ParseNode* rhs = AddSubRight(expr);

  Type lhsType, rhsType;
  unsigned lhsNumAddOrSub, rhsNumAddOrSub;

  if (lhs->isKind(ParseNodeKind::AddExpr) ||
      lhs->isKind(ParseNodeKind::SubExpr)) {
    if (!CheckAddOrSub(f, lhs, &lhsType, &lhsNumAddOrSub)) {
      return false;
    }
    if (lhsType == Type::Intish) {
      lhsType = Type::Int;
    }
  } else {
    if (!CheckExpr(f, lhs, &lhsType)) {
      return false;
    }
    lhsNumAddOrSub = 0;
  }

  if (rhs->isKind(ParseNodeKind::AddExpr) ||
      rhs->isKind(ParseNodeKind::SubExpr)) {
    if (!CheckAddOrSub(f, rhs, &rhsType, &rhsNumAddOrSub)) {
      return false;
    }
    if (rhsType == Type::Intish) {
      rhsType = Type::Int;
    }
  } else {
    if (!CheckExpr(f, rhs, &rhsType)) {
      return false;
    }
    rhsNumAddOrSub = 0;
  }

  unsigned numAddOrSub = lhsNumAddOrSub + rhsNumAddOrSub + 1;
  if (numAddOrSub > (1 << 20)) {
    return f.fail(expr, "too many + or - without intervening coercion");
  }

  if (lhsType.isInt() && rhsType.isInt()) {
    if (!f.encoder().writeOp(
            expr->isKind(ParseNodeKind::AddExpr) ? Op::I32Add : Op::I32Sub)) {
      return false;
    }
    *type = Type::Intish;
  } else if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
    if (!f.encoder().writeOp(
            expr->isKind(ParseNodeKind::AddExpr) ? Op::F64Add : Op::F64Sub)) {
      return false;
    }
    *type = Type::Double;
  } else if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
    if (!f.encoder().writeOp(
            expr->isKind(ParseNodeKind::AddExpr) ? Op::F32Add : Op::F32Sub)) {
      return false;
    }
    *type = Type::Floatish;
  } else {
    return f.failf(
        expr,
        "operands to + or - must both be int, float? or double?, got %s and %s",
        lhsType.toChars(), rhsType.toChars());
  }

  if (numAddOrSubOut) {
    *numAddOrSubOut = numAddOrSub;
  }
  return true;
}

template <typename Unit>
static bool CheckDivOrMod(FunctionValidator<Unit>& f, ParseNode* expr,
                          Type* type) {
  MOZ_ASSERT(expr->isKind(ParseNodeKind::DivExpr) ||
             expr->isKind(ParseNodeKind::ModExpr));

  ParseNode* lhs = DivOrModLeft(expr);
  ParseNode* rhs = DivOrModRight(expr);

  Type lhsType, rhsType;
  if (!CheckExpr(f, lhs, &lhsType)) {
    return false;
  }
  if (!CheckExpr(f, rhs, &rhsType)) {
    return false;
  }

  if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
    *type = Type::Double;
    if (expr->isKind(ParseNodeKind::DivExpr)) {
      return f.encoder().writeOp(Op::F64Div);
    }
    return f.encoder().writeOp(MozOp::F64Mod);
  }

  if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
    *type = Type::Floatish;
    if (expr->isKind(ParseNodeKind::DivExpr)) {
      return f.encoder().writeOp(Op::F32Div);
    }
    return f.fail(expr, "modulo cannot receive float arguments");
  }

  if (lhsType.isSigned() && rhsType.isSigned()) {
    *type = Type::Intish;
    return f.encoder().writeOp(
        expr->isKind(ParseNodeKind::DivExpr) ? Op::I32DivS : Op::I32RemS);
  }

  if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
    *type = Type::Intish;
    return f.encoder().writeOp(
        expr->isKind(ParseNodeKind::DivExpr) ? Op::I32DivU : Op::I32RemU);
  }

  return f.failf(
      expr,
      "arguments to / or %% must both be double?, float?, signed, or unsigned; "
      "%s and %s are given",
      lhsType.toChars(), rhsType.toChars());
}

template <typename Unit>
static bool CheckComparison(FunctionValidator<Unit>& f, ParseNode* comp,
                            Type* type) {
  MOZ_ASSERT(comp->isKind(ParseNodeKind::LtExpr) ||
             comp->isKind(ParseNodeKind::LeExpr) ||
             comp->isKind(ParseNodeKind::GtExpr) ||
             comp->isKind(ParseNodeKind::GeExpr) ||
             comp->isKind(ParseNodeKind::EqExpr) ||
             comp->isKind(ParseNodeKind::NeExpr));

  ParseNode* lhs = ComparisonLeft(comp);
  ParseNode* rhs = ComparisonRight(comp);

  Type lhsType, rhsType;
  if (!CheckExpr(f, lhs, &lhsType)) {
    return false;
  }
  if (!CheckExpr(f, rhs, &rhsType)) {
    return false;
  }

  if (!(lhsType.isSigned() && rhsType.isSigned()) &&
      !(lhsType.isUnsigned() && rhsType.isUnsigned()) &&
      !(lhsType.isDouble() && rhsType.isDouble()) &&
      !(lhsType.isFloat() && rhsType.isFloat())) {
    return f.failf(comp,
                   "arguments to a comparison must both be signed, unsigned, "
                   "floats or doubles; "
                   "%s and %s are given",
                   lhsType.toChars(), rhsType.toChars());
  }

  Op stmt;
  if (lhsType.isSigned() && rhsType.isSigned()) {
    switch (comp->getKind()) {
      case ParseNodeKind::EqExpr:
        stmt = Op::I32Eq;
        break;
      case ParseNodeKind::NeExpr:
        stmt = Op::I32Ne;
        break;
      case ParseNodeKind::LtExpr:
        stmt = Op::I32LtS;
        break;
      case ParseNodeKind::LeExpr:
        stmt = Op::I32LeS;
        break;
      case ParseNodeKind::GtExpr:
        stmt = Op::I32GtS;
        break;
      case ParseNodeKind::GeExpr:
        stmt = Op::I32GeS;
        break;
      default:
        MOZ_CRASH("unexpected comparison op");
    }
  } else if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
    switch (comp->getKind()) {
      case ParseNodeKind::EqExpr:
        stmt = Op::I32Eq;
        break;
      case ParseNodeKind::NeExpr:
        stmt = Op::I32Ne;
        break;
      case ParseNodeKind::LtExpr:
        stmt = Op::I32LtU;
        break;
      case ParseNodeKind::LeExpr:
        stmt = Op::I32LeU;
        break;
      case ParseNodeKind::GtExpr:
        stmt = Op::I32GtU;
        break;
      case ParseNodeKind::GeExpr:
        stmt = Op::I32GeU;
        break;
      default:
        MOZ_CRASH("unexpected comparison op");
    }
  } else if (lhsType.isDouble()) {
    switch (comp->getKind()) {
      case ParseNodeKind::EqExpr:
        stmt = Op::F64Eq;
        break;
      case ParseNodeKind::NeExpr:
        stmt = Op::F64Ne;
        break;
      case ParseNodeKind::LtExpr:
        stmt = Op::F64Lt;
        break;
      case ParseNodeKind::LeExpr:
        stmt = Op::F64Le;
        break;
      case ParseNodeKind::GtExpr:
        stmt = Op::F64Gt;
        break;
      case ParseNodeKind::GeExpr:
        stmt = Op::F64Ge;
        break;
      default:
        MOZ_CRASH("unexpected comparison op");
    }
  } else if (lhsType.isFloat()) {
    switch (comp->getKind()) {
      case ParseNodeKind::EqExpr:
        stmt = Op::F32Eq;
        break;
      case ParseNodeKind::NeExpr:
        stmt = Op::F32Ne;
        break;
      case ParseNodeKind::LtExpr:
        stmt = Op::F32Lt;
        break;
      case ParseNodeKind::LeExpr:
        stmt = Op::F32Le;
        break;
      case ParseNodeKind::GtExpr:
        stmt = Op::F32Gt;
        break;
      case ParseNodeKind::GeExpr:
        stmt = Op::F32Ge;
        break;
      default:
        MOZ_CRASH("unexpected comparison op");
    }
  } else {
    MOZ_CRASH("unexpected type");
  }

  *type = Type::Int;
  return f.encoder().writeOp(stmt);
}

template <typename Unit>
static bool CheckBitwise(FunctionValidator<Unit>& f, ParseNode* bitwise,
                         Type* type) {
  ParseNode* lhs = BitwiseLeft(bitwise);
  ParseNode* rhs = BitwiseRight(bitwise);

  int32_t identityElement;
  bool onlyOnRight;
  switch (bitwise->getKind()) {
    case ParseNodeKind::BitOrExpr:
      identityElement = 0;
      onlyOnRight = false;
      *type = Type::Signed;
      break;
    case ParseNodeKind::BitAndExpr:
      identityElement = -1;
      onlyOnRight = false;
      *type = Type::Signed;
      break;
    case ParseNodeKind::BitXorExpr:
      identityElement = 0;
      onlyOnRight = false;
      *type = Type::Signed;
      break;
    case ParseNodeKind::LshExpr:
      identityElement = 0;
      onlyOnRight = true;
      *type = Type::Signed;
      break;
    case ParseNodeKind::RshExpr:
      identityElement = 0;
      onlyOnRight = true;
      *type = Type::Signed;
      break;
    case ParseNodeKind::UrshExpr:
      identityElement = 0;
      onlyOnRight = true;
      *type = Type::Unsigned;
      break;
    default:
      MOZ_CRASH("not a bitwise op");
  }

  uint32_t i;
  if (!onlyOnRight && IsLiteralInt(f.m(), lhs, &i) &&
      i == uint32_t(identityElement)) {
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsType)) {
      return false;
    }
    if (!rhsType.isIntish()) {
      return f.failf(bitwise, "%s is not a subtype of intish",
                     rhsType.toChars());
    }
    return true;
  }

  if (IsLiteralInt(f.m(), rhs, &i) && i == uint32_t(identityElement)) {
    if (bitwise->isKind(ParseNodeKind::BitOrExpr) &&
        lhs->isKind(ParseNodeKind::CallExpr)) {
      return CheckCoercedCall(f, lhs, Type::Int, type);
    }

    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsType)) {
      return false;
    }
    if (!lhsType.isIntish()) {
      return f.failf(bitwise, "%s is not a subtype of intish",
                     lhsType.toChars());
    }
    return true;
  }

  Type lhsType;
  if (!CheckExpr(f, lhs, &lhsType)) {
    return false;
  }

  Type rhsType;
  if (!CheckExpr(f, rhs, &rhsType)) {
    return false;
  }

  if (!lhsType.isIntish()) {
    return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
  }
  if (!rhsType.isIntish()) {
    return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());
  }

  switch (bitwise->getKind()) {
    case ParseNodeKind::BitOrExpr:
      if (!f.encoder().writeOp(Op::I32Or)) return false;
      break;
    case ParseNodeKind::BitAndExpr:
      if (!f.encoder().writeOp(Op::I32And)) return false;
      break;
    case ParseNodeKind::BitXorExpr:
      if (!f.encoder().writeOp(Op::I32Xor)) return false;
      break;
    case ParseNodeKind::LshExpr:
      if (!f.encoder().writeOp(Op::I32Shl)) return false;
      break;
    case ParseNodeKind::RshExpr:
      if (!f.encoder().writeOp(Op::I32ShrS)) return false;
      break;
    case ParseNodeKind::UrshExpr:
      if (!f.encoder().writeOp(Op::I32ShrU)) return false;
      break;
    default:
      MOZ_CRASH("not a bitwise op");
  }

  return true;
}

template <typename Unit>
static bool CheckExpr(FunctionValidator<Unit>& f, ParseNode* expr, Type* type) {
  AutoCheckRecursionLimit recursion(f.fc());
  if (!recursion.checkDontReport(f.fc())) {
    return f.m().failOverRecursed();
  }

  if (IsNumericLiteral(f.m(), expr)) {
    return CheckNumericLiteral(f, expr, type);
  }

  switch (expr->getKind()) {
    case ParseNodeKind::Name:
      return CheckVarRef(f, expr, type);
    case ParseNodeKind::ElemExpr:
      return CheckLoadArray(f, expr, type);
    case ParseNodeKind::AssignExpr:
      return CheckAssign(f, expr, type);
    case ParseNodeKind::PosExpr:
      return CheckPos(f, expr, type);
    case ParseNodeKind::NotExpr:
      return CheckNot(f, expr, type);
    case ParseNodeKind::NegExpr:
      return CheckNeg(f, expr, type);
    case ParseNodeKind::BitNotExpr:
      return CheckBitNot(f, expr, type);
    case ParseNodeKind::CommaExpr:
      return CheckComma(f, expr, type);
    case ParseNodeKind::ConditionalExpr:
      return CheckConditional(f, expr, type);
    case ParseNodeKind::MulExpr:
      return CheckMultiply(f, expr, type);
    case ParseNodeKind::CallExpr:
      return CheckUncoercedCall(f, expr, type);

    case ParseNodeKind::AddExpr:
    case ParseNodeKind::SubExpr:
      return CheckAddOrSub(f, expr, type);

    case ParseNodeKind::DivExpr:
    case ParseNodeKind::ModExpr:
      return CheckDivOrMod(f, expr, type);

    case ParseNodeKind::LtExpr:
    case ParseNodeKind::LeExpr:
    case ParseNodeKind::GtExpr:
    case ParseNodeKind::GeExpr:
    case ParseNodeKind::EqExpr:
    case ParseNodeKind::NeExpr:
      return CheckComparison(f, expr, type);

    case ParseNodeKind::BitOrExpr:
    case ParseNodeKind::BitAndExpr:
    case ParseNodeKind::BitXorExpr:
    case ParseNodeKind::LshExpr:
    case ParseNodeKind::RshExpr:
    case ParseNodeKind::UrshExpr:
      return CheckBitwise(f, expr, type);

    default:;
  }

  return f.fail(expr, "unsupported expression");
}

template <typename Unit>
static bool CheckStatement(FunctionValidator<Unit>& f, ParseNode* stmt);

template <typename Unit>
static bool CheckAsExprStatement(FunctionValidator<Unit>& f, ParseNode* expr) {
  if (expr->isKind(ParseNodeKind::CallExpr)) {
    Type ignored;
    return CheckCoercedCall(f, expr, Type::Void, &ignored);
  }

  Type resultType;
  if (!CheckExpr(f, expr, &resultType)) {
    return false;
  }

  if (!resultType.isVoid()) {
    if (!f.encoder().writeOp(Op::Drop)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
static bool CheckExprStatement(FunctionValidator<Unit>& f,
                               ParseNode* exprStmt) {
  MOZ_ASSERT(exprStmt->isKind(ParseNodeKind::ExpressionStmt));
  return CheckAsExprStatement(f, UnaryKid(exprStmt));
}

template <typename Unit>
static bool CheckLoopConditionOnEntry(FunctionValidator<Unit>& f,
                                      ParseNode* cond) {
  uint32_t maybeLit;
  if (IsLiteralInt(f.m(), cond, &maybeLit) && maybeLit) {
    return true;
  }

  Type condType;
  if (!CheckExpr(f, cond, &condType)) {
    return false;
  }
  if (!condType.isInt()) {
    return f.failf(cond, "%s is not a subtype of int", condType.toChars());
  }

  if (!f.encoder().writeOp(Op::I32Eqz)) {
    return false;
  }

  // brIf (i32.eqz $f) $out
  return f.writeBreakIf();
}

template <typename Unit>
static bool CheckWhile(FunctionValidator<Unit>& f, ParseNode* whileStmt,
                       const LabelVector* labels = nullptr) {
  MOZ_ASSERT(whileStmt->isKind(ParseNodeKind::WhileStmt));
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
  if (labels && !f.addLabels(*labels, 0, 1)) {
    return false;
  }

  if (!f.pushLoop()) {
    return false;
  }

  if (!CheckLoopConditionOnEntry(f, cond)) {
    return false;
  }
  if (!CheckStatement(f, body)) {
    return false;
  }
  if (!f.writeContinue()) {
    return false;
  }

  if (!f.popLoop()) {
    return false;
  }
  if (labels) {
    f.removeLabels(*labels);
  }
  return true;
}

template <typename Unit>
static bool CheckFor(FunctionValidator<Unit>& f, ParseNode* forStmt,
                     const LabelVector* labels = nullptr) {
  MOZ_ASSERT(forStmt->isKind(ParseNodeKind::ForStmt));
  ParseNode* forHead = BinaryLeft(forStmt);
  ParseNode* body = BinaryRight(forStmt);

  if (!forHead->isKind(ParseNodeKind::ForHead)) {
    return f.fail(forHead, "unsupported for-loop statement");
  }

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
  if (labels && !f.addLabels(*labels, 1, 3)) {
    return false;
  }

  if (!f.pushUnbreakableBlock()) {
    return false;
  }

  if (maybeInit && !CheckAsExprStatement(f, maybeInit)) {
    return false;
  }

  {
    if (!f.pushLoop()) {
      return false;
    }

    if (maybeCond && !CheckLoopConditionOnEntry(f, maybeCond)) {
      return false;
    }

    {
      // Continuing in the body should just break out to the increment.
      if (!f.pushContinuableBlock()) {
        return false;
      }
      if (!CheckStatement(f, body)) {
        return false;
      }
      if (!f.popContinuableBlock()) {
        return false;
      }
    }

    if (maybeInc && !CheckAsExprStatement(f, maybeInc)) {
      return false;
    }

    if (!f.writeContinue()) {
      return false;
    }
    if (!f.popLoop()) {
      return false;
    }
  }

  if (!f.popUnbreakableBlock()) {
    return false;
  }

  if (labels) {
    f.removeLabels(*labels);
  }

  return true;
}

template <typename Unit>
static bool CheckDoWhile(FunctionValidator<Unit>& f, ParseNode* whileStmt,
                         const LabelVector* labels = nullptr) {
  MOZ_ASSERT(whileStmt->isKind(ParseNodeKind::DoWhileStmt));
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
  if (labels && !f.addLabels(*labels, 0, 2)) {
    return false;
  }

  if (!f.pushLoop()) {
    return false;
  }

  {
    // An unlabeled continue in the body should break out to the condition.
    if (!f.pushContinuableBlock()) {
      return false;
    }
    if (!CheckStatement(f, body)) {
      return false;
    }
    if (!f.popContinuableBlock()) {
      return false;
    }
  }

  Type condType;
  if (!CheckExpr(f, cond, &condType)) {
    return false;
  }
  if (!condType.isInt()) {
    return f.failf(cond, "%s is not a subtype of int", condType.toChars());
  }

  if (!f.writeContinueIf()) {
    return false;
  }

  if (!f.popLoop()) {
    return false;
  }
  if (labels) {
    f.removeLabels(*labels);
  }
  return true;
}

template <typename Unit>
static bool CheckStatementList(FunctionValidator<Unit>& f, ParseNode*,
                               const LabelVector* = nullptr);

template <typename Unit>
static bool CheckLabel(FunctionValidator<Unit>& f, ParseNode* labeledStmt) {
  MOZ_ASSERT(labeledStmt->isKind(ParseNodeKind::LabelStmt));

  LabelVector labels;
  ParseNode* innermost = labeledStmt;
  do {
    if (!labels.append(LabeledStatementLabel(innermost))) {
      return false;
    }
    innermost = LabeledStatementStatement(innermost);
  } while (innermost->getKind() == ParseNodeKind::LabelStmt);

  switch (innermost->getKind()) {
    case ParseNodeKind::ForStmt:
      return CheckFor(f, innermost, &labels);
    case ParseNodeKind::DoWhileStmt:
      return CheckDoWhile(f, innermost, &labels);
    case ParseNodeKind::WhileStmt:
      return CheckWhile(f, innermost, &labels);
    case ParseNodeKind::StatementList:
      return CheckStatementList(f, innermost, &labels);
    default:
      break;
  }

  if (!f.pushUnbreakableBlock(&labels)) {
    return false;
  }

  if (!CheckStatement(f, innermost)) {
    return false;
  }

  return f.popUnbreakableBlock(&labels);
}

template <typename Unit>
static bool CheckIf(FunctionValidator<Unit>& f, ParseNode* ifStmt) {
  uint32_t numIfEnd = 1;

recurse:
  MOZ_ASSERT(ifStmt->isKind(ParseNodeKind::IfStmt));
  ParseNode* cond = TernaryKid1(ifStmt);
  ParseNode* thenStmt = TernaryKid2(ifStmt);
  ParseNode* elseStmt = TernaryKid3(ifStmt);

  Type condType;
  if (!CheckExpr(f, cond, &condType)) {
    return false;
  }
  if (!condType.isInt()) {
    return f.failf(cond, "%s is not a subtype of int", condType.toChars());
  }

  size_t typeAt;
  if (!f.pushIf(&typeAt)) {
    return false;
  }

  f.setIfType(typeAt, TypeCode::BlockVoid);

  if (!CheckStatement(f, thenStmt)) {
    return false;
  }

  if (elseStmt) {
    if (!f.switchToElse()) {
      return false;
    }

    if (elseStmt->isKind(ParseNodeKind::IfStmt)) {
      ifStmt = elseStmt;
      if (numIfEnd++ == UINT32_MAX) {
        return false;
      }
      goto recurse;
    }

    if (!CheckStatement(f, elseStmt)) {
      return false;
    }
  }

  for (uint32_t i = 0; i != numIfEnd; ++i) {
    if (!f.popIf()) {
      return false;
    }
  }

  return true;
}

static bool CheckCaseExpr(FunctionValidatorShared& f, ParseNode* caseExpr,
                          int32_t* value) {
  if (!IsNumericLiteral(f.m(), caseExpr)) {
    return f.fail(caseExpr,
                  "switch case expression must be an integer literal");
  }

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
      return f.fail(caseExpr,
                    "switch case expression must be an integer literal");
  }

  return true;
}

static bool CheckDefaultAtEnd(FunctionValidatorShared& f, ParseNode* stmt) {
  for (; stmt; stmt = NextNode(stmt)) {
    if (IsDefaultCase(stmt) && NextNode(stmt) != nullptr) {
      return f.fail(stmt, "default label must be at the end");
    }
  }

  return true;
}

static bool CheckSwitchRange(FunctionValidatorShared& f, ParseNode* stmt,
                             int32_t* low, int32_t* high,
                             uint32_t* tableLength) {
  if (IsDefaultCase(stmt)) {
    *low = 0;
    *high = -1;
    *tableLength = 0;
    return true;
  }

  int32_t i = 0;
  if (!CheckCaseExpr(f, CaseExpr(stmt), &i)) {
    return false;
  }

  *low = *high = i;

  ParseNode* initialStmt = stmt;
  for (stmt = NextNode(stmt); stmt && !IsDefaultCase(stmt);
       stmt = NextNode(stmt)) {
    int32_t i = 0;
    if (!CheckCaseExpr(f, CaseExpr(stmt), &i)) {
      return false;
    }

    *low = std::min(*low, i);
    *high = std::max(*high, i);
  }

  int64_t i64 = (int64_t(*high) - int64_t(*low)) + 1;
  if (i64 > MaxBrTableElems) {
    return f.fail(
        initialStmt,
        "all switch statements generate tables; this table would be too big");
  }

  *tableLength = uint32_t(i64);
  return true;
}

template <typename Unit>
static bool CheckSwitchExpr(FunctionValidator<Unit>& f, ParseNode* switchExpr) {
  Type exprType;
  if (!CheckExpr(f, switchExpr, &exprType)) {
    return false;
  }
  if (!exprType.isSigned()) {
    return f.failf(switchExpr, "%s is not a subtype of signed",
                   exprType.toChars());
  }
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
template <typename Unit>
static bool CheckSwitch(FunctionValidator<Unit>& f, ParseNode* switchStmt) {
  MOZ_ASSERT(switchStmt->isKind(ParseNodeKind::SwitchStmt));

  ParseNode* switchExpr = BinaryLeft(switchStmt);
  ParseNode* switchBody = BinaryRight(switchStmt);

  if (switchBody->is<LexicalScopeNode>()) {
    LexicalScopeNode* scope = &switchBody->as<LexicalScopeNode>();
    if (!scope->isEmptyScope()) {
      return f.fail(scope, "switch body may not contain lexical declarations");
    }
    switchBody = scope->scopeBody();
  }

  ParseNode* stmt = ListHead(switchBody);
  if (!stmt) {
    if (!CheckSwitchExpr(f, switchExpr)) {
      return false;
    }
    return f.encoder().writeOp(Op::Drop);
  }

  if (!CheckDefaultAtEnd(f, stmt)) {
    return false;
  }

  int32_t low = 0, high = 0;
  uint32_t tableLength = 0;
  if (!CheckSwitchRange(f, stmt, &low, &high, &tableLength)) {
    return false;
  }

  static const uint32_t CASE_NOT_DEFINED = UINT32_MAX;

  Uint32Vector caseDepths;
  if (!caseDepths.appendN(CASE_NOT_DEFINED, tableLength)) {
    return false;
  }

  uint32_t numCases = 0;
  for (ParseNode* s = stmt; s && !IsDefaultCase(s); s = NextNode(s)) {
    int32_t caseValue = ExtractNumericLiteral(f.m(), CaseExpr(s)).toInt32();

    MOZ_ASSERT(caseValue >= low);
    unsigned i = caseValue - low;
    if (caseDepths[i] != CASE_NOT_DEFINED) {
      return f.fail(s, "no duplicate case labels");
    }

    MOZ_ASSERT(numCases != CASE_NOT_DEFINED);
    caseDepths[i] = numCases++;
  }

  // Open the wrapping breakable default block.
  if (!f.pushBreakableBlock()) {
    return false;
  }

  // Open all the case blocks.
  for (uint32_t i = 0; i < numCases; i++) {
    if (!f.pushUnbreakableBlock()) {
      return false;
    }
  }

  // Open the br_table block.
  if (!f.pushUnbreakableBlock()) {
    return false;
  }

  // The default block is the last one.
  uint32_t defaultDepth = numCases;

  // Subtract lowest case value, so that all the cases start from 0.
  if (low) {
    if (!CheckSwitchExpr(f, switchExpr)) {
      return false;
    }
    if (!f.writeInt32Lit(low)) {
      return false;
    }
    if (!f.encoder().writeOp(Op::I32Sub)) {
      return false;
    }
  } else {
    if (!CheckSwitchExpr(f, switchExpr)) {
      return false;
    }
  }

  // Start the br_table block.
  if (!f.encoder().writeOp(Op::BrTable)) {
    return false;
  }

  // Write the number of cases (tableLength - 1 + 1 (default)).
  // Write the number of cases (tableLength - 1 + 1 (default)).
  if (!f.encoder().writeVarU32(tableLength)) {
    return false;
  }

  // Each case value describes the relative depth to the actual block. When
  // a case is not explicitly defined, it goes to the default.
  for (size_t i = 0; i < tableLength; i++) {
    uint32_t target =
        caseDepths[i] == CASE_NOT_DEFINED ? defaultDepth : caseDepths[i];
    if (!f.encoder().writeVarU32(target)) {
      return false;
    }
  }

  // Write the default depth.
  if (!f.encoder().writeVarU32(defaultDepth)) {
    return false;
  }

  // Our br_table is done. Close its block, write the cases down in order.
  if (!f.popUnbreakableBlock()) {
    return false;
  }

  for (; stmt && !IsDefaultCase(stmt); stmt = NextNode(stmt)) {
    if (!CheckStatement(f, CaseBody(stmt))) {
      return false;
    }
    if (!f.popUnbreakableBlock()) {
      return false;
    }
  }

  // Write the default block.
  if (stmt && IsDefaultCase(stmt)) {
    if (!CheckStatement(f, CaseBody(stmt))) {
      return false;
    }
  }

  // Close the wrapping block.
  return f.popBreakableBlock();
}

static bool CheckReturnType(FunctionValidatorShared& f, ParseNode* usepn,
                            Type ret) {
  Maybe<ValType> type = ret.canonicalToReturnType();

  if (!f.hasAlreadyReturned()) {
    f.setReturnedType(type);
    return true;
  }

  if (f.returnedType() != type) {
    return f.failf(usepn, "%s incompatible with previous return of type %s",
                   ToString(type, nullptr).get(),
                   ToString(f.returnedType(), nullptr).get());
  }

  return true;
}

template <typename Unit>
static bool CheckReturn(FunctionValidator<Unit>& f, ParseNode* returnStmt) {
  ParseNode* expr = ReturnExpr(returnStmt);

  if (!expr) {
    if (!CheckReturnType(f, returnStmt, Type::Void)) {
      return false;
    }
  } else {
    Type type;
    if (!CheckExpr(f, expr, &type)) {
      return false;
    }

    if (!type.isReturnType()) {
      return f.failf(expr, "%s is not a valid return type", type.toChars());
    }

    if (!CheckReturnType(f, expr, Type::canonicalize(type))) {
      return false;
    }
  }

  return f.encoder().writeOp(Op::Return);
}

template <typename Unit>
static bool CheckStatementList(FunctionValidator<Unit>& f, ParseNode* stmtList,
                               const LabelVector* labels /*= nullptr */) {
  MOZ_ASSERT(stmtList->isKind(ParseNodeKind::StatementList));

  if (!f.pushUnbreakableBlock(labels)) {
    return false;
  }

  for (ParseNode* stmt = ListHead(stmtList); stmt; stmt = NextNode(stmt)) {
    if (!CheckStatement(f, stmt)) {
      return false;
    }
  }

  return f.popUnbreakableBlock(labels);
}

template <typename Unit>
static bool CheckLexicalScope(FunctionValidator<Unit>& f, ParseNode* node) {
  LexicalScopeNode* lexicalScope = &node->as<LexicalScopeNode>();
  if (!lexicalScope->isEmptyScope()) {
    return f.fail(lexicalScope, "cannot have 'let' or 'const' declarations");
  }

  return CheckStatement(f, lexicalScope->scopeBody());
}

static bool CheckBreakOrContinue(FunctionValidatorShared& f, bool isBreak,
                                 ParseNode* stmt) {
  if (TaggedParserAtomIndex maybeLabel = LoopControlMaybeLabel(stmt)) {
    return f.writeLabeledBreakOrContinue(maybeLabel, isBreak);
  }
  return f.writeUnlabeledBreakOrContinue(isBreak);
}

template <typename Unit>
static bool CheckStatement(FunctionValidator<Unit>& f, ParseNode* stmt) {
  AutoCheckRecursionLimit recursion(f.fc());
  if (!recursion.checkDontReport(f.fc())) {
    return f.m().failOverRecursed();
  }

  switch (stmt->getKind()) {
    case ParseNodeKind::EmptyStmt:
      return true;
    case ParseNodeKind::ExpressionStmt:
      return CheckExprStatement(f, stmt);
    case ParseNodeKind::WhileStmt:
      return CheckWhile(f, stmt);
    case ParseNodeKind::ForStmt:
      return CheckFor(f, stmt);
    case ParseNodeKind::DoWhileStmt:
      return CheckDoWhile(f, stmt);
    case ParseNodeKind::LabelStmt:
      return CheckLabel(f, stmt);
    case ParseNodeKind::IfStmt:
      return CheckIf(f, stmt);
    case ParseNodeKind::SwitchStmt:
      return CheckSwitch(f, stmt);
    case ParseNodeKind::ReturnStmt:
      return CheckReturn(f, stmt);
    case ParseNodeKind::StatementList:
      return CheckStatementList(f, stmt);
    case ParseNodeKind::BreakStmt:
      return CheckBreakOrContinue(f, true, stmt);
    case ParseNodeKind::ContinueStmt:
      return CheckBreakOrContinue(f, false, stmt);
    case ParseNodeKind::LexicalScope:
      return CheckLexicalScope(f, stmt);
    default:;
  }

  return f.fail(stmt, "unexpected statement kind");
}

template <typename Unit>
static bool ParseFunction(ModuleValidator<Unit>& m, FunctionNode** funNodeOut,
                          unsigned* line) {
  auto& tokenStream = m.tokenStream();

  tokenStream.consumeKnownToken(TokenKind::Function,
                                TokenStreamShared::SlashIsRegExp);

  auto& anyChars = tokenStream.anyCharsAccess();
  uint32_t toStringStart = anyChars.currentToken().pos.begin;
  *line = anyChars.lineNumber(anyChars.lineToken(toStringStart));

  TokenKind tk;
  if (!tokenStream.getToken(&tk, TokenStreamShared::SlashIsRegExp)) {
    return false;
  }
  if (tk == TokenKind::Mul) {
    return m.failCurrentOffset("unexpected generator function");
  }
  if (!TokenKindIsPossibleIdentifier(tk)) {
    return false;  // The regular parser will throw a SyntaxError, no need to
                   // m.fail.
  }

  TaggedParserAtomIndex name = m.parser().bindingIdentifier(YieldIsName);
  if (!name) {
    return false;
  }

  FunctionNode* funNode;
  MOZ_TRY_VAR_OR_RETURN(funNode,
                        m.parser().handler_.newFunction(
                            FunctionSyntaxKind::Statement, m.parser().pos()),
                        false);

  ParseContext* outerpc = m.parser().pc_;
  Directives directives(outerpc);
  FunctionFlags flags(FunctionFlags::INTERPRETED_NORMAL);
  FunctionBox* funbox = m.parser().newFunctionBox(
      funNode, name, flags, toStringStart, directives,
      GeneratorKind::NotGenerator, FunctionAsyncKind::SyncFunction);
  if (!funbox) {
    return false;
  }
  funbox->initWithEnclosingParseContext(outerpc, FunctionSyntaxKind::Statement);

  Directives newDirectives = directives;
  SourceParseContext funpc(&m.parser(), funbox, &newDirectives);
  if (!funpc.init()) {
    return false;
  }

  if (!m.parser().functionFormalParametersAndBody(
          InAllowed, YieldIsName, &funNode, FunctionSyntaxKind::Statement)) {
    if (anyChars.hadError() || directives == newDirectives) {
      return false;
    }

    return m.fail(funNode, "encountered new directive in function");
  }

  MOZ_ASSERT(!anyChars.hadError());
  MOZ_ASSERT(directives == newDirectives);

  *funNodeOut = funNode;
  return true;
}

template <typename Unit>
static bool CheckFunction(ModuleValidator<Unit>& m) {
  // asm.js modules can be quite large when represented as parse trees so pop
  // the backing LifoAlloc after parsing/compiling each function. Release the
  // parser's lifo memory after the last use of a parse node.
  frontend::ParserBase::Mark mark = m.parser().mark();
  auto releaseMark =
      mozilla::MakeScopeExit([&m, &mark] { m.parser().release(mark); });

  FunctionNode* funNode = nullptr;
  unsigned line = 0;
  if (!ParseFunction(m, &funNode, &line)) {
    return false;
  }

  if (!CheckFunctionHead(m, funNode)) {
    return false;
  }

  FunctionValidator<Unit> f(m, funNode);

  ParseNode* stmtIter = ListHead(FunctionStatementList(funNode));

  if (!CheckProcessingDirectives(m, &stmtIter)) {
    return false;
  }

  ValTypeVector args;
  if (!CheckArguments(f, &stmtIter, &args)) {
    return false;
  }

  if (!CheckVariables(f, &stmtIter)) {
    return false;
  }

  ParseNode* lastNonEmptyStmt = nullptr;
  for (; stmtIter; stmtIter = NextNonEmptyStatement(stmtIter)) {
    lastNonEmptyStmt = stmtIter;
    if (!CheckStatement(f, stmtIter)) {
      return false;
    }
  }

  if (!CheckFinalReturn(f, lastNonEmptyStmt)) {
    return false;
  }

  ValTypeVector results;
  if (f.returnedType()) {
    if (!results.append(f.returnedType().ref())) {
      return false;
    }
  }

  FuncType sig(std::move(args), std::move(results));

  ModuleValidatorShared::Func* func = nullptr;
  if (!CheckFunctionSignature(m, funNode, std::move(sig), FunctionName(funNode),
                              &func)) {
    return false;
  }

  if (func->defined()) {
    return m.failName(funNode, "function '%s' already defined",
                      FunctionName(funNode));
  }

  f.define(func, line);

  return true;
}

static bool CheckAllFunctionsDefined(ModuleValidatorShared& m) {
  for (unsigned i = 0; i < m.numFuncDefs(); i++) {
    const ModuleValidatorShared::Func& f = m.funcDef(i);
    if (!f.defined()) {
      return m.failNameOffset(f.firstUse(), "missing definition of function %s",
                              f.name());
    }
  }

  return true;
}

template <typename Unit>
static bool CheckFunctions(ModuleValidator<Unit>& m) {
  while (true) {
    TokenKind tk;
    if (!PeekToken(m.parser(), &tk)) {
      return false;
    }

    if (tk != TokenKind::Function) {
      break;
    }

    if (!CheckFunction(m)) {
      return false;
    }
  }

  return CheckAllFunctionsDefined(m);
}

template <typename Unit>
static bool CheckFuncPtrTable(ModuleValidator<Unit>& m, ParseNode* decl) {
  if (!decl->isKind(ParseNodeKind::AssignExpr)) {
    return m.fail(decl, "function-pointer table must have initializer");
  }
  AssignmentNode* assignNode = &decl->as<AssignmentNode>();

  ParseNode* var = assignNode->left();

  if (!var->isKind(ParseNodeKind::Name)) {
    return m.fail(var, "function-pointer table name is not a plain name");
  }

  ParseNode* arrayLiteral = assignNode->right();

  if (!arrayLiteral->isKind(ParseNodeKind::ArrayExpr)) {
    return m.fail(
        var, "function-pointer table's initializer must be an array literal");
  }

  unsigned length = ListLength(arrayLiteral);

  if (!IsPowerOfTwo(length)) {
    return m.failf(arrayLiteral,
                   "function-pointer table length must be a power of 2 (is %u)",
                   length);
  }

  unsigned mask = length - 1;

  Uint32Vector elemFuncDefIndices;
  const FuncType* sig = nullptr;
  for (ParseNode* elem = ListHead(arrayLiteral); elem; elem = NextNode(elem)) {
    if (!elem->isKind(ParseNodeKind::Name)) {
      return m.fail(
          elem, "function-pointer table's elements must be names of functions");
    }

    TaggedParserAtomIndex funcName = elem->as<NameNode>().name();
    const ModuleValidatorShared::Func* func = m.lookupFuncDef(funcName);
    if (!func) {
      return m.fail(
          elem, "function-pointer table's elements must be names of functions");
    }

    const FuncType& funcSig = m.env().types->type(func->sigIndex()).funcType();
    if (sig) {
      if (!FuncType::strictlyEquals(*sig, funcSig)) {
        return m.fail(elem, "all functions in table must have same signature");
      }
    } else {
      sig = &funcSig;
    }

    if (!elemFuncDefIndices.append(func->funcDefIndex())) {
      return false;
    }
  }

  FuncType copy;
  if (!copy.clone(*sig)) {
    return false;
  }

  uint32_t tableIndex;
  if (!CheckFuncPtrTableAgainstExisting(m, var, var->as<NameNode>().name(),
                                        std::move(copy), mask, &tableIndex)) {
    return false;
  }

  if (!m.defineFuncPtrTable(tableIndex, std::move(elemFuncDefIndices))) {
    return m.fail(var, "duplicate function-pointer definition");
  }

  return true;
}

template <typename Unit>
static bool CheckFuncPtrTables(ModuleValidator<Unit>& m) {
  while (true) {
    ParseNode* varStmt;
    if (!ParseVarOrConstStatement(m.parser(), &varStmt)) {
      return false;
    }
    if (!varStmt) {
      break;
    }
    for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
      if (!CheckFuncPtrTable(m, var)) {
        return false;
      }
    }
  }

  for (unsigned i = 0; i < m.numFuncPtrTables(); i++) {
    ModuleValidatorShared::Table& table = m.table(i);
    if (!table.defined()) {
      return m.failNameOffset(table.firstUse(),
                              "function-pointer table %s wasn't defined",
                              table.name());
    }
  }

  return true;
}

static bool CheckModuleExportFunction(
    ModuleValidatorShared& m, ParseNode* pn,
    TaggedParserAtomIndex maybeFieldName = TaggedParserAtomIndex::null()) {
  if (!pn->isKind(ParseNodeKind::Name)) {
    return m.fail(pn, "expected name of exported function");
  }

  TaggedParserAtomIndex funcName = pn->as<NameNode>().name();
  const ModuleValidatorShared::Func* func = m.lookupFuncDef(funcName);
  if (!func) {
    return m.failName(pn, "function '%s' not found", funcName);
  }

  return m.addExportField(*func, maybeFieldName);
}

static bool CheckModuleExportObject(ModuleValidatorShared& m,
                                    ParseNode* object) {
  MOZ_ASSERT(object->isKind(ParseNodeKind::ObjectExpr));

  for (ParseNode* pn = ListHead(object); pn; pn = NextNode(pn)) {
    if (!IsNormalObjectField(pn)) {
      return m.fail(pn,
                    "only normal object properties may be used in the export "
                    "object literal");
    }

    TaggedParserAtomIndex fieldName = ObjectNormalFieldName(pn);

    ParseNode* initNode = ObjectNormalFieldInitializer(pn);
    if (!initNode->isKind(ParseNodeKind::Name)) {
      return m.fail(
          initNode,
          "initializer of exported object literal must be name of function");
    }

    if (!CheckModuleExportFunction(m, initNode, fieldName)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
static bool CheckModuleReturn(ModuleValidator<Unit>& m) {
  TokenKind tk;
  if (!GetToken(m.parser(), &tk)) {
    return false;
  }
  auto& ts = m.parser().tokenStream;
  if (tk != TokenKind::Return) {
    return m.failCurrentOffset(
        (tk == TokenKind::RightCurly || tk == TokenKind::Eof)
            ? "expecting return statement"
            : "invalid asm.js. statement");
  }
  ts.anyCharsAccess().ungetToken();

  ParseNode* returnStmt;
  MOZ_TRY_VAR_OR_RETURN(returnStmt, m.parser().statementListItem(YieldIsName),
                        false);

  ParseNode* returnExpr = ReturnExpr(returnStmt);
  if (!returnExpr) {
    return m.fail(returnStmt, "export statement must return something");
  }

  if (returnExpr->isKind(ParseNodeKind::ObjectExpr)) {
    if (!CheckModuleExportObject(m, returnExpr)) {
      return false;
    }
  } else {
    if (!CheckModuleExportFunction(m, returnExpr)) {
      return false;
    }
  }

  return true;
}

template <typename Unit>
static bool CheckModuleEnd(ModuleValidator<Unit>& m) {
  TokenKind tk;
  if (!GetToken(m.parser(), &tk)) {
    return false;
  }

  if (tk != TokenKind::Eof && tk != TokenKind::RightCurly) {
    return m.failCurrentOffset(
        "top-level export (return) must be the last statement");
  }

  m.parser().tokenStream.anyCharsAccess().ungetToken();
  return true;
}

template <typename Unit>
static SharedModule CheckModule(FrontendContext* fc,
                                ParserAtomsTable& parserAtoms,
                                AsmJSParser<Unit>& parser, ParseNode* stmtList,
                                unsigned* time) {
  int64_t before = PRMJ_Now();

  FunctionNode* moduleFunctionNode = parser.pc_->functionBox()->functionNode;

  ModuleValidator<Unit> m(fc, parserAtoms, parser, moduleFunctionNode);
  if (!m.init()) {
    return nullptr;
  }

  if (!CheckFunctionHead(m, moduleFunctionNode)) {
    return nullptr;
  }

  if (!CheckModuleArguments(m, moduleFunctionNode)) {
    return nullptr;
  }

  if (!CheckPrecedingStatements(m, stmtList)) {
    return nullptr;
  }

  if (!CheckModuleProcessingDirectives(m)) {
    return nullptr;
  }

  if (!CheckModuleGlobals(m)) {
    return nullptr;
  }

  if (!m.startFunctionBodies()) {
    return nullptr;
  }

  if (!CheckFunctions(m)) {
    return nullptr;
  }

  if (!CheckFuncPtrTables(m)) {
    return nullptr;
  }

  if (!CheckModuleReturn(m)) {
    return nullptr;
  }

  if (!CheckModuleEnd(m)) {
    return nullptr;
  }

  SharedModule module = m.finish();
  if (!module) {
    return nullptr;
  }

  *time = (PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC;
  return module;
}

/*****************************************************************************/
// Link-time validation

static bool LinkFail(JSContext* cx, const char* str) {
  WarnNumberASCII(cx, JSMSG_USE_ASM_LINK_FAIL, str);
  return false;
}

static bool IsMaybeWrappedScriptedProxy(JSObject* obj) {
  JSObject* unwrapped = UncheckedUnwrap(obj);
  return unwrapped && IsScriptedProxy(unwrapped);
}

static bool GetDataProperty(JSContext* cx, HandleValue objVal,
                            Handle<JSAtom*> field, MutableHandleValue v) {
  if (!objVal.isObject()) {
    return LinkFail(cx, "accessing property of non-object");
  }

  RootedObject obj(cx, &objVal.toObject());
  if (IsMaybeWrappedScriptedProxy(obj)) {
    return LinkFail(cx, "accessing property of a Proxy");
  }

  RootedId id(cx, AtomToId(field));
  Rooted<mozilla::Maybe<PropertyDescriptor>> desc(cx);
  RootedObject holder(cx);
  if (!GetPropertyDescriptor(cx, obj, id, &desc, &holder)) {
    return false;
  }

  if (!desc.isSome()) {
    return LinkFail(cx, "property not present on object");
  }

  if (!desc->isDataDescriptor()) {
    return LinkFail(cx, "property is not a data property");
  }

  v.set(desc->value());
  return true;
}

static bool GetDataProperty(JSContext* cx, HandleValue objVal,
                            const char* fieldChars, MutableHandleValue v) {
  Rooted<JSAtom*> field(cx,
                        AtomizeUTF8Chars(cx, fieldChars, strlen(fieldChars)));
  if (!field) {
    return false;
  }

  return GetDataProperty(cx, objVal, field, v);
}

static bool GetDataProperty(JSContext* cx, HandleValue objVal,
                            const ImmutableTenuredPtr<PropertyName*>& field,
                            MutableHandleValue v) {
  Handle<PropertyName*> fieldHandle = field;
  return GetDataProperty(cx, objVal, fieldHandle, v);
}

static bool HasObjectValueOfMethodPure(JSObject* obj, JSContext* cx) {
  Value v;
  if (!GetPropertyPure(cx, obj, NameToId(cx->names().valueOf), &v)) {
    return false;
  }

  JSFunction* fun;
  if (!IsFunctionObject(v, &fun)) {
    return false;
  }

  return IsSelfHostedFunctionWithName(fun, cx->names().Object_valueOf);
}

static bool HasPureCoercion(JSContext* cx, HandleValue v) {
  // Ideally, we'd reject all non-primitives, but Emscripten has a bug that
  // generates code that passes functions for some imports. To avoid breaking
  // all the code that contains this bug, we make an exception for functions
  // that don't have user-defined valueOf or toString, for their coercions
  // are not observable and coercion via ToNumber/ToInt32 definitely produces
  // NaN/0. We should remove this special case later once most apps have been
  // built with newer Emscripten.
  return v.toObject().is<JSFunction>() &&
         HasNoToPrimitiveMethodPure(&v.toObject(), cx) &&
         HasObjectValueOfMethodPure(&v.toObject(), cx) &&
         HasNativeMethodPure(&v.toObject(), cx->names().toString, fun_toString,
                             cx);
}

static bool ValidateGlobalVariable(JSContext* cx, const AsmJSGlobal& global,
                                   HandleValue importVal,
                                   Maybe<LitValPOD>* val) {
  switch (global.varInitKind()) {
    case AsmJSGlobal::InitConstant:
      val->emplace(global.varInitVal());
      return true;

    case AsmJSGlobal::InitImport: {
      RootedValue v(cx);
      if (!GetDataProperty(cx, importVal, global.field(), &v)) {
        return false;
      }

      if (!v.isPrimitive() && !HasPureCoercion(cx, v)) {
        return LinkFail(cx, "Imported values must be primitives");
      }

      switch (global.varInitImportType().kind()) {
        case ValType::I32: {
          int32_t i32;
          if (!ToInt32(cx, v, &i32)) {
            return false;
          }
          val->emplace(uint32_t(i32));
          return true;
        }
        case ValType::I64:
          MOZ_CRASH("int64");
        case ValType::V128:
          MOZ_CRASH("v128");
        case ValType::F32: {
          float f;
          if (!RoundFloat32(cx, v, &f)) {
            return false;
          }
          val->emplace(f);
          return true;
        }
        case ValType::F64: {
          double d;
          if (!ToNumber(cx, v, &d)) {
            return false;
          }
          val->emplace(d);
          return true;
        }
        case ValType::Ref: {
          MOZ_CRASH("not available in asm.js");
        }
      }
    }
  }

  MOZ_CRASH("unreachable");
}

static bool ValidateFFI(JSContext* cx, const AsmJSGlobal& global,
                        HandleValue importVal,
                        MutableHandle<FunctionVector> ffis) {
  RootedValue v(cx);
  if (!GetDataProperty(cx, importVal, global.field(), &v)) {
    return false;
  }

  if (!IsFunctionObject(v)) {
    return LinkFail(cx, "FFI imports must be functions");
  }

  ffis[global.ffiIndex()].set(&v.toObject().as<JSFunction>());
  return true;
}

static bool ValidateArrayView(JSContext* cx, const AsmJSGlobal& global,
                              HandleValue globalVal) {
  if (!global.field()) {
    return true;
  }

  if (Scalar::isBigIntType(global.viewType())) {
    return LinkFail(cx, "bad typed array constructor");
  }

  RootedValue v(cx);
  if (!GetDataProperty(cx, globalVal, global.field(), &v)) {
    return false;
  }

  bool tac = IsTypedArrayConstructor(v, global.viewType());
  if (!tac) {
    return LinkFail(cx, "bad typed array constructor");
  }

  return true;
}

static InlinableNative ToInlinableNative(AsmJSMathBuiltinFunction func) {
  switch (func) {
    case AsmJSMathBuiltin_sin:
      return InlinableNative::MathSin;
    case AsmJSMathBuiltin_cos:
      return InlinableNative::MathCos;
    case AsmJSMathBuiltin_tan:
      return InlinableNative::MathTan;
    case AsmJSMathBuiltin_asin:
      return InlinableNative::MathASin;
    case AsmJSMathBuiltin_acos:
      return InlinableNative::MathACos;
    case AsmJSMathBuiltin_atan:
      return InlinableNative::MathATan;
    case AsmJSMathBuiltin_ceil:
      return InlinableNative::MathCeil;
    case AsmJSMathBuiltin_floor:
      return InlinableNative::MathFloor;
    case AsmJSMathBuiltin_exp:
      return InlinableNative::MathExp;
    case AsmJSMathBuiltin_log:
      return InlinableNative::MathLog;
    case AsmJSMathBuiltin_pow:
      return InlinableNative::MathPow;
    case AsmJSMathBuiltin_sqrt:
      return InlinableNative::MathSqrt;
    case AsmJSMathBuiltin_abs:
      return InlinableNative::MathAbs;
    case AsmJSMathBuiltin_atan2:
      return InlinableNative::MathATan2;
    case AsmJSMathBuiltin_imul:
      return InlinableNative::MathImul;
    case AsmJSMathBuiltin_fround:
      return InlinableNative::MathFRound;
    case AsmJSMathBuiltin_min:
      return InlinableNative::MathMin;
    case AsmJSMathBuiltin_max:
      return InlinableNative::MathMax;
    case AsmJSMathBuiltin_clz32:
      return InlinableNative::MathClz32;
  }
  MOZ_CRASH("Invalid asm.js math builtin function");
}

static bool ValidateMathBuiltinFunction(JSContext* cx,
                                        const AsmJSMetadata& metadata,
                                        const AsmJSGlobal& global,
                                        HandleValue globalVal) {
  RootedValue v(cx);
  if (!GetDataProperty(cx, globalVal, cx->names().Math, &v)) {
    return false;
  }

  if (!GetDataProperty(cx, v, global.field(), &v)) {
    return false;
  }

  InlinableNative native = ToInlinableNative(global.mathBuiltinFunction());

  JSFunction* fun;
  if (!IsFunctionObject(v, &fun) || !fun->hasJitInfo() ||
      fun->jitInfo()->type() != JSJitInfo::InlinableNative ||
      fun->jitInfo()->inlinableNative != native) {
    return LinkFail(cx, "bad Math.* builtin function");
  }
  if (fun->realm()->creationOptions().alwaysUseFdlibm() !=
      metadata.alwaysUseFdlibm) {
    return LinkFail(cx,
                    "Math.* builtin function and asm.js use different native"
                    " math implementations.");
  }

  return true;
}

static bool ValidateConstant(JSContext* cx, const AsmJSGlobal& global,
                             HandleValue globalVal) {
  RootedValue v(cx, globalVal);

  if (global.constantKind() == AsmJSGlobal::MathConstant) {
    if (!GetDataProperty(cx, v, cx->names().Math, &v)) {
      return false;
    }
  }

  if (!GetDataProperty(cx, v, global.field(), &v)) {
    return false;
  }

  if (!v.isNumber()) {
    return LinkFail(cx, "math / global constant value needs to be a number");
  }

  // NaN != NaN
  if (std::isnan(global.constantValue())) {
    if (!std::isnan(v.toNumber())) {
      return LinkFail(cx, "global constant value needs to be NaN");
    }
  } else {
    if (v.toNumber() != global.constantValue()) {
      return LinkFail(cx, "global constant value mismatch");
    }
  }

  return true;
}

static bool CheckBuffer(JSContext* cx, const AsmJSMetadata& metadata,
                        HandleValue bufferVal,
                        MutableHandle<ArrayBufferObject*> buffer) {
  if (!bufferVal.isObject()) {
    return LinkFail(cx, "buffer must be an object");
  }
  JSObject* bufferObj = &bufferVal.toObject();

  if (metadata.memories[0].isShared()) {
    if (!bufferObj->is<SharedArrayBufferObject>()) {
      return LinkFail(
          cx, "shared views can only be constructed onto SharedArrayBuffer");
    }
    return LinkFail(cx, "Unable to prepare SharedArrayBuffer for asm.js use");
  }

  if (!bufferObj->is<ArrayBufferObject>()) {
    return LinkFail(cx,
                    "unshared views can only be constructed onto ArrayBuffer");
  }

  buffer.set(&bufferObj->as<ArrayBufferObject>());

  size_t memoryLength = buffer->byteLength();

  if (!IsValidAsmJSHeapLength(memoryLength)) {
    UniqueChars msg;
    if (memoryLength > MaxHeapLength) {
      msg = JS_smprintf("ArrayBuffer byteLength 0x%" PRIx64
                        " is not a valid heap length - it is too long."
                        " The longest valid length is 0x%" PRIx64,
                        uint64_t(memoryLength), MaxHeapLength);
    } else {
      msg = JS_smprintf("ArrayBuffer byteLength 0x%" PRIx64
                        " is not a valid heap length. The next "
                        "valid length is 0x%" PRIx64,
                        uint64_t(memoryLength),
                        RoundUpToNextValidAsmJSHeapLength(memoryLength));
    }
    if (!msg) {
      return false;
    }
    return LinkFail(cx, msg.get());
  }

  // This check is sufficient without considering the size of the loaded datum
  // because heap loads and stores start on an aligned boundary and the heap
  // byteLength has larger alignment.
  uint64_t minMemoryLength = metadata.memories.length() != 0
                                 ? metadata.memories[0].initialLength32()
                                 : 0;
  MOZ_ASSERT((minMemoryLength - 1) <= INT32_MAX);
  if (memoryLength < minMemoryLength) {
    UniqueChars msg(JS_smprintf("ArrayBuffer byteLength of 0x%" PRIx64
                                " is less than 0x%" PRIx64 " (the "
                                "size implied "
                                "by const heap accesses).",
                                uint64_t(memoryLength), minMemoryLength));
    if (!msg) {
      return false;
    }
    return LinkFail(cx, msg.get());
  }

  // ArrayBuffer lengths in SpiderMonkey used to be restricted to <= INT32_MAX,
  // but that has since been relaxed for the benefit of wasm.  We keep the old
  // limit for asm.js so as to avoid having to worry about whether the asm.js
  // implementation is safe for larger heaps.
  if (memoryLength >= INT32_MAX) {
    UniqueChars msg(
        JS_smprintf("ArrayBuffer byteLength 0x%" PRIx64
                    " is too large for asm.js (implementation limit).",
                    uint64_t(memoryLength)));
    if (!msg) {
      return false;
    }
    return LinkFail(cx, msg.get());
  }

  if (buffer->isResizable()) {
    return LinkFail(cx,
                    "Unable to prepare resizable ArrayBuffer for asm.js use");
  }

  if (!buffer->prepareForAsmJS()) {
    return LinkFail(cx, "Unable to prepare ArrayBuffer for asm.js use");
  }

  MOZ_ASSERT(buffer->isPreparedForAsmJS());
  return true;
}

static bool GetImports(JSContext* cx, const AsmJSMetadata& metadata,
                       HandleValue globalVal, HandleValue importVal,
                       ImportValues* imports) {
  Rooted<FunctionVector> ffis(cx, FunctionVector(cx));
  if (!ffis.resize(metadata.numFFIs)) {
    return false;
  }

  for (const AsmJSGlobal& global : metadata.asmJSGlobals) {
    switch (global.which()) {
      case AsmJSGlobal::Variable: {
        Maybe<LitValPOD> litVal;
        if (!ValidateGlobalVariable(cx, global, importVal, &litVal)) {
          return false;
        }
        if (!imports->globalValues.append(Val(litVal->asLitVal()))) {
          return false;
        }
        break;
      }
      case AsmJSGlobal::FFI:
        if (!ValidateFFI(cx, global, importVal, &ffis)) {
          return false;
        }
        break;
      case AsmJSGlobal::ArrayView:
      case AsmJSGlobal::ArrayViewCtor:
        if (!ValidateArrayView(cx, global, globalVal)) {
          return false;
        }
        break;
      case AsmJSGlobal::MathBuiltinFunction:
        if (!ValidateMathBuiltinFunction(cx, metadata, global, globalVal)) {
          return false;
        }
        break;
      case AsmJSGlobal::Constant:
        if (!ValidateConstant(cx, global, globalVal)) {
          return false;
        }
        break;
    }
  }

  for (const AsmJSImport& import : metadata.asmJSImports) {
    if (!imports->funcs.append(ffis[import.ffiIndex()])) {
      return false;
    }
  }

  return true;
}

static bool TryInstantiate(JSContext* cx, const CallArgs& args,
                           const Module& module, const AsmJSMetadata& metadata,
                           MutableHandle<WasmInstanceObject*> instanceObj,
                           MutableHandleObject exportObj) {
  HandleValue globalVal = args.get(0);
  HandleValue importVal = args.get(1);
  HandleValue bufferVal = args.get(2);

  MOZ_RELEASE_ASSERT(HasPlatformSupport());

  if (!wasm::EnsureFullSignalHandlers(cx)) {
    return LinkFail(cx, "failed to install signal handlers");
  }

  Rooted<ImportValues> imports(cx);

  if (module.metadata().memories.length() != 0) {
    MOZ_ASSERT(module.metadata().memories.length() == 1);
    Rooted<ArrayBufferObject*> buffer(cx);
    if (!CheckBuffer(cx, metadata, bufferVal, &buffer)) {
      return false;
    }

    Rooted<WasmMemoryObject*> memory(
        cx, WasmMemoryObject::create(cx, buffer, /* isHuge= */ false, nullptr));
    if (!memory || !imports.get().memories.append(memory)) {
      return false;
    }
  }

  if (!GetImports(cx, metadata, globalVal, importVal, imports.address())) {
    return false;
  }

  if (!module.instantiate(cx, imports.get(), nullptr, instanceObj)) {
    return false;
  }

  exportObj.set(&instanceObj->exportsObj());
  return true;
}

static bool HandleInstantiationFailure(JSContext* cx, const CallArgs& args,
                                       const AsmJSMetadata& metadata) {
  using js::frontend::FunctionSyntaxKind;

  Rooted<JSAtom*> name(cx, args.callee().as<JSFunction>().fullExplicitName());

  if (cx->isExceptionPending()) {
    return false;
  }

  ScriptSource* source = metadata.maybeScriptSource();

  // Source discarding is allowed to affect JS semantics because it is never
  // enabled for normal JS content.
  bool haveSource;
  if (!ScriptSource::loadSource(cx, source, &haveSource)) {
    return false;
  }
  if (!haveSource) {
    JS_ReportErrorASCII(cx,
                        "asm.js link failure with source discarding enabled");
    return false;
  }

  uint32_t begin = metadata.toStringStart;
  uint32_t end = metadata.srcEndAfterCurly();
  Rooted<JSLinearString*> src(cx, source->substringDontDeflate(cx, begin, end));
  if (!src) {
    return false;
  }

  JS::CompileOptions options(cx);
  options.setMutedErrors(source->mutedErrors())
      .setFile(source->filename())
      .setNoScriptRval(false);
  options.setAsmJSOption(AsmJSOption::DisabledByLinker);

  // The exported function inherits an implicit strict context if the module
  // also inherited it somehow.
  if (metadata.strict) {
    options.setForceStrictMode();
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.initTwoByte(cx, src)) {
    return false;
  }

  SourceText<char16_t> srcBuf;
  if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
    return false;
  }

  FunctionSyntaxKind syntaxKind = FunctionSyntaxKind::Statement;

  RootedFunction fun(cx, frontend::CompileStandaloneFunction(
                             cx, options, srcBuf, Nothing(), syntaxKind));
  if (!fun) {
    return false;
  }

  fun->initEnvironment(&cx->global()->lexicalEnvironment());

  // Call the function we just recompiled.
  args.setCallee(ObjectValue(*fun));
  return InternalCallOrConstruct(
      cx, args, args.isConstructing() ? CONSTRUCT : NO_CONSTRUCT);
}

static const Module& AsmJSModuleFunctionToModule(JSFunction* fun) {
  MOZ_ASSERT(IsAsmJSModule(fun));
  const Value& v = fun->getExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT);
  return v.toObject().as<WasmModuleObject>().module();
}

// Implements the semantics of an asm.js module function that has been
// successfully validated.
bool js::InstantiateAsmJS(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  JSFunction* callee = &args.callee().as<JSFunction>();
  const Module& module = AsmJSModuleFunctionToModule(callee);
  const AsmJSMetadata& metadata = module.metadata().asAsmJS();

  Rooted<WasmInstanceObject*> instanceObj(cx);
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

/*****************************************************************************/
// Top-level js::CompileAsmJS

static bool NoExceptionPending(FrontendContext* fc) { return !fc->hadErrors(); }

static bool SuccessfulValidation(frontend::ParserBase& parser,
                                 unsigned compilationTime) {
  unsigned errNum = js::SupportDifferentialTesting()
                        ? JSMSG_USE_ASM_TYPE_OK_NO_TIME
                        : JSMSG_USE_ASM_TYPE_OK;

  char timeChars[20];
  SprintfLiteral(timeChars, "%u", compilationTime);

  return parser.warningNoOffset(errNum, timeChars);
}

static bool TypeFailureWarning(frontend::ParserBase& parser, const char* str) {
  if (parser.options().throwOnAsmJSValidationFailure()) {
    parser.errorNoOffset(JSMSG_USE_ASM_TYPE_FAIL, str ? str : "");
    return false;
  }

  // Per the asm.js standard convention, whether failure sets a pending
  // exception determines whether to attempt non-asm.js reparsing, so ignore
  // the return value below.
  (void)parser.warningNoOffset(JSMSG_USE_ASM_TYPE_FAIL, str ? str : "");
  return false;
}

// asm.js requires Ion to be available on the current hardware/OS and to be
// enabled for wasm, since asm.js compilation goes via wasm.
static bool IsAsmJSCompilerAvailable(JSContext* cx) {
  return HasPlatformSupport() && WasmCompilerForAsmJSAvailable(cx);
}

static bool EstablishPreconditions(frontend::ParserBase& parser) {
  switch (parser.options().asmJSOption()) {
    case AsmJSOption::DisabledByAsmJSPref:
      return TypeFailureWarning(
          parser, "Asm.js optimizer disabled by 'asmjs' runtime option");
    case AsmJSOption::DisabledByLinker:
      return TypeFailureWarning(
          parser,
          "Asm.js optimizer disabled by linker (instantiation failure)");
    case AsmJSOption::DisabledByNoWasmCompiler:
      return TypeFailureWarning(parser,
                                "Asm.js optimizer disabled because no suitable "
                                "wasm compiler is available");
    case AsmJSOption::DisabledByDebugger:
      return TypeFailureWarning(
          parser, "Asm.js optimizer disabled because debugger is active");
    case AsmJSOption::Enabled:
      break;
  }

  if (parser.pc_->isGenerator()) {
    return TypeFailureWarning(parser,
                              "Asm.js optimizer disabled in generator context");
  }

  if (parser.pc_->isAsync()) {
    return TypeFailureWarning(parser,
                              "Asm.js optimizer disabled in async context");
  }

  if (parser.pc_->isArrowFunction()) {
    return TypeFailureWarning(
        parser, "Asm.js optimizer disabled in arrow function context");
  }

  // Class constructors are also methods
  if (parser.pc_->isMethod() || parser.pc_->isGetterOrSetter()) {
    return TypeFailureWarning(
        parser,
        "Asm.js optimizer disabled in class constructor or method context");
  }

  return true;
}

template <typename Unit>
static bool DoCompileAsmJS(FrontendContext* fc, ParserAtomsTable& parserAtoms,
                           AsmJSParser<Unit>& parser, ParseNode* stmtList,
                           bool* validated) {
  *validated = false;

  // Various conditions disable asm.js optimizations.
  if (!EstablishPreconditions(parser)) {
    return NoExceptionPending(fc);
  }

  // "Checking" parses, validates and compiles, producing a fully compiled
  // WasmModuleObject as result.
  unsigned time;
  SharedModule module = CheckModule(fc, parserAtoms, parser, stmtList, &time);
  if (!module) {
    return NoExceptionPending(fc);
  }

  // Finished! Save the ref-counted module on the FunctionBox. When JSFunctions
  // are eventually allocated we will create an asm.js constructor for it.
  FunctionBox* funbox = parser.pc_->functionBox();
  MOZ_ASSERT(funbox->isInterpreted());
  if (!funbox->setAsmJSModule(module)) {
    return NoExceptionPending(fc);
  }

  // Success! Write to the console with a "warning" message indicating
  // total compilation time.
  *validated = true;
  SuccessfulValidation(parser, time);
  return NoExceptionPending(fc);
}

bool js::CompileAsmJS(FrontendContext* fc, ParserAtomsTable& parserAtoms,
                      AsmJSParser<char16_t>& parser, ParseNode* stmtList,
                      bool* validated) {
  return DoCompileAsmJS(fc, parserAtoms, parser, stmtList, validated);
}

bool js::CompileAsmJS(FrontendContext* fc, ParserAtomsTable& parserAtoms,
                      AsmJSParser<Utf8Unit>& parser, ParseNode* stmtList,
                      bool* validated) {
  return DoCompileAsmJS(fc, parserAtoms, parser, stmtList, validated);
}

/*****************************************************************************/
// asm.js testing functions

bool js::IsAsmJSModuleNative(Native native) {
  return native == InstantiateAsmJS;
}

bool js::IsAsmJSModule(JSFunction* fun) {
  return fun->maybeNative() == InstantiateAsmJS;
}

bool js::IsAsmJSFunction(JSFunction* fun) {
  return fun->kind() == FunctionFlags::AsmJS;
}

bool js::IsAsmJSStrictModeModuleOrFunction(JSFunction* fun) {
  if (IsAsmJSModule(fun)) {
    return AsmJSModuleFunctionToModule(fun).metadata().asAsmJS().strict;
  }

  if (IsAsmJSFunction(fun)) {
    return ExportedFunctionToInstance(fun).metadata().asAsmJS().strict;
  }

  return false;
}

bool js::IsAsmJSCompilationAvailable(JSContext* cx) {
  return cx->options().asmJS() && IsAsmJSCompilerAvailable(cx);
}

bool js::IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  bool available = IsAsmJSCompilationAvailable(cx);
  args.rval().set(BooleanValue(available));
  return true;
}

static JSFunction* MaybeWrappedNativeFunction(const Value& v) {
  if (!v.isObject()) {
    return nullptr;
  }

  return v.toObject().maybeUnwrapIf<JSFunction>();
}

bool js::IsAsmJSModule(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  bool rval = false;
  if (JSFunction* fun = MaybeWrappedNativeFunction(args.get(0))) {
    rval = IsAsmJSModule(fun);
  }

  args.rval().set(BooleanValue(rval));
  return true;
}

bool js::IsAsmJSFunction(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  bool rval = false;
  if (JSFunction* fun = MaybeWrappedNativeFunction(args.get(0))) {
    rval = IsAsmJSFunction(fun);
  }

  args.rval().set(BooleanValue(rval));
  return true;
}

/*****************************************************************************/
// asm.js toString/toSource support

JSString* js::AsmJSModuleToString(JSContext* cx, HandleFunction fun,
                                  bool isToSource) {
  MOZ_ASSERT(IsAsmJSModule(fun));

  const AsmJSMetadata& metadata =
      AsmJSModuleFunctionToModule(fun).metadata().asAsmJS();
  uint32_t begin = metadata.toStringStart;
  uint32_t end = metadata.srcEndAfterCurly();
  ScriptSource* source = metadata.maybeScriptSource();

  JSStringBuilder out(cx);

  if (isToSource && fun->isLambda() && !out.append("(")) {
    return nullptr;
  }

  bool haveSource;
  if (!ScriptSource::loadSource(cx, source, &haveSource)) {
    return nullptr;
  }

  if (!haveSource) {
    if (!out.append("function ")) {
      return nullptr;
    }
    if (fun->fullExplicitName() && !out.append(fun->fullExplicitName())) {
      return nullptr;
    }
    if (!out.append("() {\n    [native code]\n}")) {
      return nullptr;
    }
  } else {
    Rooted<JSLinearString*> src(cx, source->substring(cx, begin, end));
    if (!src) {
      return nullptr;
    }

    if (!out.append(src)) {
      return nullptr;
    }
  }

  if (isToSource && fun->isLambda() && !out.append(")")) {
    return nullptr;
  }

  return out.finishString();
}

JSString* js::AsmJSFunctionToString(JSContext* cx, HandleFunction fun) {
  MOZ_ASSERT(IsAsmJSFunction(fun));

  const AsmJSMetadata& metadata =
      ExportedFunctionToInstance(fun).metadata().asAsmJS();
  const AsmJSExport& f =
      metadata.lookupAsmJSExport(ExportedFunctionToFuncIndex(fun));

  uint32_t begin = metadata.srcStart + f.startOffsetInModule();
  uint32_t end = metadata.srcStart + f.endOffsetInModule();

  ScriptSource* source = metadata.maybeScriptSource();
  JSStringBuilder out(cx);

  if (!out.append("function ")) {
    return nullptr;
  }

  bool haveSource;
  if (!ScriptSource::loadSource(cx, source, &haveSource)) {
    return nullptr;
  }

  if (!haveSource) {
    // asm.js functions can't be anonymous
    MOZ_ASSERT(fun->fullExplicitName());
    if (!out.append(fun->fullExplicitName())) {
      return nullptr;
    }
    if (!out.append("() {\n    [native code]\n}")) {
      return nullptr;
    }
  } else {
    Rooted<JSLinearString*> src(cx, source->substring(cx, begin, end));
    if (!src) {
      return nullptr;
    }
    if (!out.append(src)) {
      return nullptr;
    }
  }

  return out.finishString();
}

bool js::IsValidAsmJSHeapLength(size_t length) {
  if (length < MinHeapLength) {
    return false;
  }

  // The heap length is limited by what a wasm memory32 can handle.
  if (length > MaxMemoryBytes(IndexType::I32)) {
    return false;
  }

  return wasm::IsValidARMImmediate(length);
}
