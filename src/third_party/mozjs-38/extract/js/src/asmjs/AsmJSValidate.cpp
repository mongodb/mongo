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

#include "asmjs/AsmJSValidate.h"

#include "mozilla/Move.h"
#include "mozilla/UniquePtr.h"

#ifdef MOZ_VTUNE
# include "vtune/VTuneWrapper.h"
#endif

#include "jsmath.h"
#include "jsprf.h"
#include "jsutil.h"
#include "prmjtime.h"

#include "asmjs/AsmJSLink.h"
#include "asmjs/AsmJSModule.h"
#include "asmjs/AsmJSSignalHandlers.h"
#include "builtin/SIMD.h"
#include "frontend/Parser.h"
#include "jit/CodeGenerator.h"
#include "jit/CompileWrappers.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#ifdef JS_ION_PERF
# include "jit/PerfSpewer.h"
#endif
#include "vm/HelperThreads.h"
#include "vm/Interpreter.h"

#include "jsobjinlines.h"

#include "frontend/ParseNode-inl.h"
#include "frontend/Parser-inl.h"

using namespace js;
using namespace js::frontend;
using namespace js::jit;

using mozilla::AddToHash;
using mozilla::ArrayLength;
using mozilla::CountLeadingZeroes32;
using mozilla::DebugOnly;
using mozilla::HashGeneric;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;
using mozilla::Maybe;
using mozilla::Move;
using mozilla::PositiveInfinity;
using mozilla::UniquePtr;
using JS::GenericNaN;

static const size_t LIFO_ALLOC_PRIMARY_CHUNK_SIZE = 1 << 12;

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
    MOZ_ASSERT(pn->isKind(PNK_RETURN));
    return BinaryLeft(pn);
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
    MOZ_ASSERT(pn->isKind(PNK_CALL));
    return ListHead(pn);
}

static inline unsigned
CallArgListLength(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CALL));
    MOZ_ASSERT(ListLength(pn) >= 1);
    return ListLength(pn) - 1;
}

static inline ParseNode*
CallArgList(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CALL));
    return NextNode(ListHead(pn));
}

static inline ParseNode*
VarListHead(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_VAR) || pn->isKind(PNK_CONST));
    return ListHead(pn);
}

static inline ParseNode*
CaseExpr(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CASE) || pn->isKind(PNK_DEFAULT));
    return BinaryLeft(pn);
}

static inline ParseNode*
CaseBody(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_CASE) || pn->isKind(PNK_DEFAULT));
    return BinaryRight(pn);
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
    MOZ_ASSERT(pn->isKind(PNK_STAR));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
MultiplyRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_STAR));
    return BinaryOpRight(pn);
}

static inline ParseNode*
AddSubLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ADD) || pn->isKind(PNK_SUB));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
AddSubRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ADD) || pn->isKind(PNK_SUB));
    return BinaryOpRight(pn);
}

static inline ParseNode*
DivOrModLeft(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DIV) || pn->isKind(PNK_MOD));
    return BinaryOpLeft(pn);
}

static inline ParseNode*
DivOrModRight(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DIV) || pn->isKind(PNK_MOD));
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

static inline ParseNode*
AndOrLeft(ParseNode* pn)
{
    return BinaryOpLeft(pn);
}

static inline ParseNode*
AndOrRight(ParseNode* pn)
{
    return BinaryOpRight(pn);
}

static inline ParseNode*
RelationalLeft(ParseNode* pn)
{
    return BinaryOpLeft(pn);
}

static inline ParseNode*
RelationalRight(ParseNode* pn)
{
    return BinaryOpRight(pn);
}

static inline bool
IsExpressionStatement(ParseNode* pn)
{
    return pn->isKind(PNK_SEMI);
}

static inline ParseNode*
ExpressionStatementExpr(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_SEMI));
    return UnaryKid(pn);
}

static inline PropertyName*
LoopControlMaybeLabel(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_BREAK) || pn->isKind(PNK_CONTINUE));
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
    MOZ_ASSERT(pn->isKind(PNK_NUMBER));
    return pn->pn_dval;
}

static bool
NumberNodeHasFrac(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_NUMBER));
    return pn->pn_u.number.decimalPoint == HasDecimal;
}

static ParseNode*
DotBase(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DOT));
    MOZ_ASSERT(pn->isArity(PN_NAME));
    return pn->expr();
}

static PropertyName*
DotMember(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_DOT));
    MOZ_ASSERT(pn->isArity(PN_NAME));
    return pn->pn_atom->asPropertyName();
}

static ParseNode*
ElemBase(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ELEM));
    return BinaryLeft(pn);
}

static ParseNode*
ElemIndex(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(PNK_ELEM));
    return BinaryRight(pn);
}

static inline JSFunction*
FunctionObject(ParseNode* fn)
{
    MOZ_ASSERT(fn->isKind(PNK_FUNCTION));
    MOZ_ASSERT(fn->isArity(PN_CODE));
    return fn->pn_funbox->function();
}

static inline PropertyName*
FunctionName(ParseNode* fn)
{
    if (JSAtom* atom = FunctionObject(fn)->atom())
        return atom->asPropertyName();
    return nullptr;
}

static inline ParseNode*
FunctionStatementList(ParseNode* fn)
{
    MOZ_ASSERT(fn->pn_body->isKind(PNK_ARGSBODY));
    ParseNode* last = fn->pn_body->last();
    MOZ_ASSERT(last->isKind(PNK_STATEMENTLIST));
    return last;
}

static inline bool
IsNormalObjectField(ExclusiveContext* cx, ParseNode* pn)
{
    return pn->isKind(PNK_COLON) &&
           pn->getOp() == JSOP_INITPROP &&
           BinaryLeft(pn)->isKind(PNK_OBJECT_PROPERTY_NAME);
}

static inline PropertyName*
ObjectNormalFieldName(ExclusiveContext* cx, ParseNode* pn)
{
    MOZ_ASSERT(IsNormalObjectField(cx, pn));
    MOZ_ASSERT(BinaryLeft(pn)->isKind(PNK_OBJECT_PROPERTY_NAME));
    return BinaryLeft(pn)->pn_atom->asPropertyName();
}

static inline ParseNode*
ObjectNormalFieldInitializer(ExclusiveContext* cx, ParseNode* pn)
{
    MOZ_ASSERT(IsNormalObjectField(cx, pn));
    return BinaryRight(pn);
}

static inline bool
IsDefinition(ParseNode* pn)
{
    return pn->isKind(PNK_NAME) && pn->isDefn();
}

static inline ParseNode*
MaybeDefinitionInitializer(ParseNode* pn)
{
    MOZ_ASSERT(IsDefinition(pn));
    return pn->expr();
}

static inline bool
IsUseOfName(ParseNode* pn, PropertyName* name)
{
    return pn->isKind(PNK_NAME) && pn->name() == name;
}

static inline bool
IsIgnoredDirectiveName(ExclusiveContext* cx, JSAtom* atom)
{
    return atom != cx->names().useStrict;
}

static inline bool
IsIgnoredDirective(ExclusiveContext* cx, ParseNode* pn)
{
    return pn->isKind(PNK_SEMI) &&
           UnaryKid(pn) &&
           UnaryKid(pn)->isKind(PNK_STRING) &&
           IsIgnoredDirectiveName(cx, UnaryKid(pn)->pn_atom);
}

static inline bool
IsEmptyStatement(ParseNode* pn)
{
    return pn->isKind(PNK_SEMI) && !UnaryKid(pn);
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
PeekToken(AsmJSParser& parser, TokenKind* tkp)
{
    TokenStream& ts = parser.tokenStream;
    TokenKind tk;
    while (true) {
        if (!ts.peekToken(&tk, TokenStream::Operand))
            return false;
        if (tk != TOK_SEMI)
            break;
        ts.consumeKnownToken(TOK_SEMI);
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
    if (tk != TOK_VAR && tk != TOK_CONST) {
        *var = nullptr;
        return true;
    }

    *var = parser.statement();
    if (!*var)
        return false;

    MOZ_ASSERT((*var)->isKind(PNK_VAR) || (*var)->isKind(PNK_CONST));
    return true;
}

/*****************************************************************************/

namespace {

// Respresents the type of a general asm.js expression.
class Type
{
  public:
    enum Which {
        Fixnum = AsmJSNumLit::Fixnum,
        Signed = AsmJSNumLit::NegativeInt,
        Unsigned = AsmJSNumLit::BigUnsigned,
        DoubleLit = AsmJSNumLit::Double,
        Float = AsmJSNumLit::Float,
        Int32x4 = AsmJSNumLit::Int32x4,
        Float32x4 = AsmJSNumLit::Float32x4,
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
    Type() : which_(Which(-1)) {}
    static Type Of(const AsmJSNumLit& lit) {
        MOZ_ASSERT(lit.hasType());
        MOZ_ASSERT(Type::Which(lit.which()) >= Fixnum && Type::Which(lit.which()) <= Float32x4);
        Type t;
        t.which_ = Type::Which(lit.which());
        return t;
    }
    MOZ_IMPLICIT Type(Which w) : which_(w) {}
    Which which() const { return which_; }
    MOZ_IMPLICIT Type(AsmJSSimdType type) {
        switch (type) {
          case AsmJSSimdType_int32x4:
            which_ = Int32x4;
            return;
          case AsmJSSimdType_float32x4:
            which_ = Float32x4;
            return;
        }
        MOZ_CRASH("unexpected AsmJSSimdType");
    }

    bool operator==(Type rhs) const { return which_ == rhs.which_; }
    bool operator!=(Type rhs) const { return which_ != rhs.which_; }

    inline bool operator<=(Type rhs) const {
        switch (rhs.which_) {
          case Signed:      return isSigned();
          case Unsigned:    return isUnsigned();
          case DoubleLit:   return isDoubleLit();
          case Double:      return isDouble();
          case Float:       return isFloat();
          case Int32x4:     return isInt32x4();
          case Float32x4:   return isFloat32x4();
          case MaybeDouble: return isMaybeDouble();
          case MaybeFloat:  return isMaybeFloat();
          case Floatish:    return isFloatish();
          case Int:         return isInt();
          case Intish:      return isIntish();
          case Fixnum:      return isFixnum();
          case Void:        return isVoid();
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected this type");
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

    bool isInt32x4() const {
        return which_ == Int32x4;
    }

    bool isFloat32x4() const {
        return which_ == Float32x4;
    }

    bool isSimd() const {
        return isInt32x4() || isFloat32x4();
    }

    bool isVarType() const {
        return isInt() || isDouble() || isFloat() || isSimd();
    }

    MIRType toMIRType() const {
        switch (which_) {
          case Double:
          case DoubleLit:
          case MaybeDouble:
            return MIRType_Double;
          case Float:
          case Floatish:
          case MaybeFloat:
            return MIRType_Float32;
          case Fixnum:
          case Int:
          case Signed:
          case Unsigned:
          case Intish:
            return MIRType_Int32;
          case Int32x4:
            return MIRType_Int32x4;
          case Float32x4:
            return MIRType_Float32x4;
          case Void:
            return MIRType_None;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Invalid Type");
    }

    AsmJSSimdType simdType() const {
        MOZ_ASSERT(isSimd());
        switch (which_) {
          case Int32x4:
            return AsmJSSimdType_int32x4;
          case Float32x4:
            return AsmJSSimdType_float32x4;
          // Scalar types
          case Double:
          case DoubleLit:
          case MaybeDouble:
          case Float:
          case MaybeFloat:
          case Floatish:
          case Fixnum:
          case Int:
          case Signed:
          case Unsigned:
          case Intish:
          case Void:
            break;
        }
        MOZ_CRASH("not a SIMD Type");
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
          case Int32x4:     return "int32x4";
          case Float32x4:   return "float32x4";
          case Void:        return "void";
        }
        MOZ_CRASH("Invalid Type");
    }
};

} /* anonymous namespace */

// Represents the subset of Type that can be used as the return type of a
// function.
class RetType
{
  public:
    enum Which {
        Void = Type::Void,
        Signed = Type::Signed,
        Double = Type::Double,
        Float = Type::Float,
        Int32x4 = Type::Int32x4,
        Float32x4 = Type::Float32x4
    };

  private:
    Which which_;

  public:
    RetType() : which_(Which(-1)) {}
    MOZ_IMPLICIT RetType(Which w) : which_(w) {}
    MOZ_IMPLICIT RetType(AsmJSCoercion coercion) {
        which_ = Which(-1);  // initialize to silence GCC warning
        switch (coercion) {
          case AsmJS_ToInt32: which_ = Signed; break;
          case AsmJS_ToNumber: which_ = Double; break;
          case AsmJS_FRound: which_ = Float; break;
          case AsmJS_ToInt32x4: which_ = Int32x4; break;
          case AsmJS_ToFloat32x4: which_ = Float32x4; break;
        }
    }
    Which which() const {
        return which_;
    }
    Type toType() const {
        return Type::Which(which_);
    }
    AsmJSModule::ReturnType toModuleReturnType() const {
        switch (which_) {
          case Void: return AsmJSModule::Return_Void;
          case Signed: return AsmJSModule::Return_Int32;
          case Float: // will be converted to a Double
          case Double: return AsmJSModule::Return_Double;
          case Int32x4: return AsmJSModule::Return_Int32x4;
          case Float32x4: return AsmJSModule::Return_Float32x4;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Unexpected return type");
    }
    MIRType toMIRType() const {
        switch (which_) {
          case Void: return MIRType_None;
          case Signed: return MIRType_Int32;
          case Double: return MIRType_Double;
          case Float: return MIRType_Float32;
          case Int32x4: return MIRType_Int32x4;
          case Float32x4: return MIRType_Float32x4;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Unexpected return type");
    }
    bool operator==(RetType rhs) const { return which_ == rhs.which_; }
    bool operator!=(RetType rhs) const { return which_ != rhs.which_; }
};

namespace {

// Represents the subset of Type that can be used as a variable or
// argument's type. Note: AsmJSCoercion and VarType are kept separate to
// make very clear the signed/int distinction: a coercion may explicitly sign
// an *expression* but, when stored as a variable, this signedness information
// is explicitly thrown away by the asm.js type system. E.g., in
//
//   function f(i) {
//     i = i | 0;             (1)
//     if (...)
//         i = foo() >>> 0;
//     else
//         i = bar() | 0;
//     return i | 0;          (2)
//   }
//
// the AsmJSCoercion of (1) is Signed (since | performs ToInt32) but, when
// translated to a VarType, the result is a plain Int since, as shown, it
// is legal to assign both Signed and Unsigned (or some other Int) values to
// it. For (2), the AsmJSCoercion is also Signed but, when translated to an
// RetType, the result is Signed since callers (asm.js and non-asm.js) can
// rely on the return value being Signed.
class VarType
{
  public:
    enum Which {
        Int = Type::Int,
        Double = Type::Double,
        Float = Type::Float,
        Int32x4 = Type::Int32x4,
        Float32x4 = Type::Float32x4
    };

  private:
    Which which_;

  public:
    VarType()
      : which_(Which(-1)) {}
    MOZ_IMPLICIT VarType(Which w)
      : which_(w) {}
    MOZ_IMPLICIT VarType(AsmJSCoercion coercion) {
        switch (coercion) {
          case AsmJS_ToInt32: which_ = Int; break;
          case AsmJS_ToNumber: which_ = Double; break;
          case AsmJS_FRound: which_ = Float; break;
          case AsmJS_ToInt32x4: which_ = Int32x4; break;
          case AsmJS_ToFloat32x4: which_ = Float32x4; break;
        }
    }
    static VarType Of(const AsmJSNumLit& lit) {
        MOZ_ASSERT(lit.hasType());
        switch (lit.which()) {
          case AsmJSNumLit::Fixnum:
          case AsmJSNumLit::NegativeInt:
          case AsmJSNumLit::BigUnsigned:
            return Int;
          case AsmJSNumLit::Double:
            return Double;
          case AsmJSNumLit::Float:
            return Float;
          case AsmJSNumLit::Int32x4:
            return Int32x4;
          case AsmJSNumLit::Float32x4:
            return Float32x4;
          case AsmJSNumLit::OutOfRangeInt:
            MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("can't be out of range int");
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected literal type");
    }

    Which which() const {
        return which_;
    }
    Type toType() const {
        return Type::Which(which_);
    }
    MIRType toMIRType() const {
        switch(which_) {
          case Int:       return MIRType_Int32;
          case Double:    return MIRType_Double;
          case Float:     return MIRType_Float32;
          case Int32x4:   return MIRType_Int32x4;
          case Float32x4: return MIRType_Float32x4;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("VarType can only be Int, SIMD, Double or Float");
    }
    AsmJSCoercion toCoercion() const {
        switch(which_) {
          case Int:       return AsmJS_ToInt32;
          case Double:    return AsmJS_ToNumber;
          case Float:     return AsmJS_FRound;
          case Int32x4:   return AsmJS_ToInt32x4;
          case Float32x4: return AsmJS_ToFloat32x4;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("VarType can only be Int, SIMD, Double or Float");
    }
    static VarType FromCheckedType(Type type) {
        MOZ_ASSERT(type.isInt() || type.isMaybeDouble() || type.isFloatish() || type.isSimd());
        if (type.isMaybeDouble())
            return Double;
        else if (type.isFloatish())
            return Float;
        else if (type.isInt())
            return Int;
        else if (type.isInt32x4())
            return Int32x4;
        else if (type.isFloat32x4())
            return Float32x4;
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unknown type in FromCheckedType");
    }
    bool operator==(VarType rhs) const { return which_ == rhs.which_; }
    bool operator!=(VarType rhs) const { return which_ != rhs.which_; }
};

} /* anonymous namespace */

// Implements <: (subtype) operator when the rhs is a VarType
static inline bool
operator<=(Type lhs, VarType rhs)
{
    switch (rhs.which()) {
      case VarType::Int:       return lhs.isInt();
      case VarType::Double:    return lhs.isDouble();
      case VarType::Float:     return lhs.isFloat();
      case VarType::Int32x4:   return lhs.isInt32x4();
      case VarType::Float32x4: return lhs.isFloat32x4();
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Unexpected rhs type");
}

/*****************************************************************************/

static inline MIRType ToMIRType(MIRType t) { return t; }
static inline MIRType ToMIRType(VarType t) { return t.toMIRType(); }

namespace {

template <class VecT>
class ABIArgIter
{
    ABIArgGenerator gen_;
    const VecT& types_;
    unsigned i_;

    void settle() { if (!done()) gen_.next(ToMIRType(types_[i_])); }

  public:
    explicit ABIArgIter(const VecT& types) : types_(types), i_(0) { settle(); }
    void operator++(int) { MOZ_ASSERT(!done()); i_++; settle(); }
    bool done() const { return i_ == types_.length(); }

    ABIArg* operator->() { MOZ_ASSERT(!done()); return &gen_.current(); }
    ABIArg& operator*() { MOZ_ASSERT(!done()); return gen_.current(); }

    unsigned index() const { MOZ_ASSERT(!done()); return i_; }
    MIRType mirType() const { MOZ_ASSERT(!done()); return ToMIRType(types_[i_]); }
    uint32_t stackBytesConsumedSoFar() const { return gen_.stackBytesConsumedSoFar(); }
};

typedef Vector<MIRType, 8> MIRTypeVector;
typedef ABIArgIter<MIRTypeVector> ABIArgMIRTypeIter;

typedef Vector<VarType, 8, LifoAllocPolicy<Fallible> > VarTypeVector;
typedef ABIArgIter<VarTypeVector> ABIArgTypeIter;

class Signature
{
    VarTypeVector argTypes_;
    RetType retType_;

  public:
    explicit Signature(LifoAlloc& alloc)
      : argTypes_(alloc) {}
    Signature(LifoAlloc& alloc, RetType retType)
      : argTypes_(alloc), retType_(retType) {}
    Signature(VarTypeVector&& argTypes, RetType retType)
      : argTypes_(Move(argTypes)), retType_(Move(retType)) {}
    Signature(Signature&& rhs)
      : argTypes_(Move(rhs.argTypes_)), retType_(Move(rhs.retType_)) {}

    bool copy(const Signature& rhs) {
        if (!argTypes_.resize(rhs.argTypes_.length()))
            return false;
        for (unsigned i = 0; i < argTypes_.length(); i++)
            argTypes_[i] = rhs.argTypes_[i];
        retType_ = rhs.retType_;
        return true;
    }

    bool appendArg(VarType type) { return argTypes_.append(type); }
    VarType arg(unsigned i) const { return argTypes_[i]; }
    const VarTypeVector& args() const { return argTypes_; }
    VarTypeVector&& extractArgs() { return Move(argTypes_); }

    RetType retType() const { return retType_; }
};

} /* namespace anonymous */

static
bool operator==(const Signature& lhs, const Signature& rhs)
{
    if (lhs.retType() != rhs.retType())
        return false;
    if (lhs.args().length() != rhs.args().length())
        return false;
    for (unsigned i = 0; i < lhs.args().length(); i++) {
        if (lhs.arg(i) != rhs.arg(i))
            return false;
    }
    return true;
}

static inline
bool operator!=(const Signature& lhs, const Signature& rhs)
{
    return !(lhs == rhs);
}

/*****************************************************************************/
// Typed array utilities

static Type
TypedArrayLoadType(Scalar::Type viewType)
{
    switch (viewType) {
      case Scalar::Int8:
      case Scalar::Int16:
      case Scalar::Int32:
      case Scalar::Uint8:
      case Scalar::Uint16:
      case Scalar::Uint32:
        return Type::Intish;
      case Scalar::Float32:
        return Type::MaybeFloat;
      case Scalar::Float64:
        return Type::MaybeDouble;
      default:;
    }
    MOZ_CRASH("Unexpected array type");
}

enum NeedsBoundsCheck {
    NO_BOUNDS_CHECK,
    NEEDS_BOUNDS_CHECK
};

namespace {

typedef Vector<PropertyName*,1> LabelVector;
typedef Vector<MBasicBlock*,8> BlockVector;

// ModuleCompiler encapsulates the compilation of an entire asm.js module. Over
// the course of an ModuleCompiler object's lifetime, many FunctionCompiler
// objects will be created and destroyed in sequence, one for each function in
// the module.
//
// *** asm.js FFI calls ***
//
// asm.js allows calling out to non-asm.js via "FFI calls". The asm.js type
// system does not place any constraints on the FFI call. In particular:
//  - an FFI call's target is not known or speculated at module-compile time;
//  - a single external function can be called with different signatures.
//
// If performance didn't matter, all FFI calls could simply box their arguments
// and call js::Invoke. However, we'd like to be able to specialize FFI calls
// to be more efficient in several cases:
//
//  - for calls to JS functions which have been jitted, we'd like to call
//    directly into JIT code without going through C++.
//
//  - for calls to certain builtins, we'd like to be call directly into the C++
//    code for the builtin without going through the general call path.
//
// All of this requires dynamic specialization techniques which must happen
// after module compilation. To support this, at module-compilation time, each
// FFI call generates a call signature according to the system ABI, as if the
// callee was a C++ function taking/returning the same types as the caller was
// passing/expecting. The callee is loaded from a fixed offset in the global
// data array which allows the callee to change at runtime. Initially, the
// callee is stub which boxes its arguments and calls js::Invoke.
//
// To do this, we need to generate a callee stub for each pairing of FFI callee
// and signature. We call this pairing an "exit". For example, this code has
// two external functions and three exits:
//
//  function f(global, imports) {
//    "use asm";
//    var foo = imports.foo;
//    var bar = imports.bar;
//    function g() {
//      foo(1);      // Exit #1: (int) -> void
//      foo(1.5);    // Exit #2: (double) -> void
//      bar(1)|0;    // Exit #3: (int) -> int
//      bar(2)|0;    // Exit #3: (int) -> int
//    }
//  }
//
// The ModuleCompiler maintains a hash table (ExitMap) which allows a call site
// to add a new exit or reuse an existing one. The key is an ExitDescriptor
// (which holds the exit pairing) and the value is an index into the
// Vector<Exit> stored in the AsmJSModule.
//
// Rooting note: ModuleCompiler is a stack class that contains unrooted
// PropertyName (JSAtom) pointers.  This is safe because it cannot be
// constructed without a TokenStream reference.  TokenStream is itself a stack
// class that cannot be constructed without an AutoKeepAtoms being live on the
// stack, which prevents collection of atoms.
//
// ModuleCompiler is marked as rooted in the rooting analysis.  Don't add
// non-JSAtom pointers, or this will break!
class MOZ_STACK_CLASS ModuleCompiler
{
  public:
    class Func
    {
        Signature sig_;
        PropertyName* name_;
        Label* entry_;
        uint32_t srcBegin_;
        uint32_t srcEnd_;
        uint32_t compileTime_;
        bool defined_;

      public:
        Func(PropertyName* name, Signature&& sig, Label* entry)
          : sig_(Move(sig)), name_(name), entry_(entry), srcBegin_(0), srcEnd_(0),
            compileTime_(0), defined_(false)
        {}

        PropertyName* name() const { return name_; }
        bool defined() const { return defined_; }

        void define(ModuleCompiler& m, ParseNode* fn) {
            MOZ_ASSERT(!defined_);
            defined_ = true;
            srcBegin_ = fn->pn_pos.begin;
            srcEnd_ = fn->pn_pos.end;
        }

        uint32_t srcBegin() const { MOZ_ASSERT(defined_); return srcBegin_; }
        uint32_t srcEnd() const { MOZ_ASSERT(defined_); return srcEnd_; }
        Signature& sig() { return sig_; }
        const Signature& sig() const { return sig_; }
        Label& entry() const { return *entry_; }
        uint32_t compileTime() const { return compileTime_; }
        void accumulateCompileTime(uint32_t ms) { compileTime_ += ms; }
    };

    class Global
    {
      public:
        enum Which {
            Variable,
            ConstantLiteral,
            ConstantImport,
            Function,
            FuncPtrTable,
            FFI,
            ArrayView,
            ArrayViewCtor,
            MathBuiltinFunction,
            AtomicsBuiltinFunction,
            SimdCtor,
            SimdOperation,
            ByteLength,
            ChangeHeap
        };

      private:
        Which which_;
        union {
            struct {
                Type::Which type_;
                uint32_t index_;
                AsmJSNumLit literalValue_;
            } varOrConst;
            uint32_t funcIndex_;
            uint32_t funcPtrTableIndex_;
            uint32_t ffiIndex_;
            Scalar::Type viewType_;
            AsmJSMathBuiltinFunction mathBuiltinFunc_;
            AsmJSAtomicsBuiltinFunction atomicsBuiltinFunc_;
            AsmJSSimdType simdCtorType_;
            struct {
                AsmJSSimdType type_;
                AsmJSSimdOperation which_;
            } simdOp;
            struct {
                uint32_t srcBegin_;
                uint32_t srcEnd_;
            } changeHeap;
        } u;

        friend class ModuleCompiler;
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
        uint32_t varOrConstIndex() const {
            MOZ_ASSERT(which_ == Variable || which_ == ConstantImport);
            return u.varOrConst.index_;
        }
        bool isConst() const {
            return which_ == ConstantLiteral || which_ == ConstantImport;
        }
        AsmJSNumLit constLiteralValue() const {
            MOZ_ASSERT(which_ == ConstantLiteral);
            return u.varOrConst.literalValue_;
        }
        uint32_t funcIndex() const {
            MOZ_ASSERT(which_ == Function);
            return u.funcIndex_;
        }
        uint32_t funcPtrTableIndex() const {
            MOZ_ASSERT(which_ == FuncPtrTable);
            return u.funcPtrTableIndex_;
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
            return u.viewType_;
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
        AsmJSSimdType simdCtorType() const {
            MOZ_ASSERT(which_ == SimdCtor);
            return u.simdCtorType_;
        }
        bool isSimdOperation() const {
            return which_ == SimdOperation;
        }
        AsmJSSimdOperation simdOperation() const {
            MOZ_ASSERT(which_ == SimdOperation);
            return u.simdOp.which_;
        }
        AsmJSSimdType simdOperationType() const {
            MOZ_ASSERT(which_ == SimdOperation);
            return u.simdOp.type_;
        }
        uint32_t changeHeapSrcBegin() const {
            MOZ_ASSERT(which_ == ChangeHeap);
            return u.changeHeap.srcBegin_;
        }
        uint32_t changeHeapSrcEnd() const {
            MOZ_ASSERT(which_ == ChangeHeap);
            return u.changeHeap.srcEnd_;
        }
    };

    typedef Vector<const Func*> FuncPtrVector;

    class FuncPtrTable
    {
        Signature sig_;
        uint32_t mask_;
        uint32_t globalDataOffset_;
        FuncPtrVector elems_;

      public:
        FuncPtrTable(ExclusiveContext* cx, Signature&& sig, uint32_t mask, uint32_t gdo)
          : sig_(Move(sig)), mask_(mask), globalDataOffset_(gdo), elems_(cx)
        {}

        FuncPtrTable(FuncPtrTable&& rhs)
          : sig_(Move(rhs.sig_)), mask_(rhs.mask_), globalDataOffset_(rhs.globalDataOffset_),
            elems_(Move(rhs.elems_))
        {}

        Signature& sig() { return sig_; }
        const Signature& sig() const { return sig_; }
        unsigned mask() const { return mask_; }
        unsigned globalDataOffset() const { return globalDataOffset_; }

        bool initialized() const { return !elems_.empty(); }
        void initElems(FuncPtrVector&& elems) { elems_ = Move(elems); MOZ_ASSERT(initialized()); }
        unsigned numElems() const { MOZ_ASSERT(initialized()); return elems_.length(); }
        const Func& elem(unsigned i) const { return *elems_[i]; }
    };

    typedef Vector<FuncPtrTable> FuncPtrTableVector;

    class ExitDescriptor
    {
        PropertyName* name_;
        Signature sig_;

      public:
        ExitDescriptor(PropertyName* name, Signature&& sig)
          : name_(name), sig_(Move(sig)) {}
        ExitDescriptor(ExitDescriptor&& rhs)
          : name_(rhs.name_), sig_(Move(rhs.sig_))
        {}
        PropertyName* name() const {
            return name_;
        }
        const Signature& sig() const {
            return sig_;
        }

        // ExitDescriptor is a HashPolicy:
        typedef ExitDescriptor Lookup;
        static HashNumber hash(const ExitDescriptor& d) {
            HashNumber hn = HashGeneric(d.name_, d.sig_.retType().which());
            const VarTypeVector& args = d.sig_.args();
            for (unsigned i = 0; i < args.length(); i++)
                hn = AddToHash(hn, args[i].which());
            return hn;
        }
        static bool match(const ExitDescriptor& lhs, const ExitDescriptor& rhs) {
            return lhs.name_ == rhs.name_ && lhs.sig_ == rhs.sig_;
        }
    };

    typedef HashMap<ExitDescriptor, unsigned, ExitDescriptor> ExitMap;

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
    struct SlowFunction
    {
        SlowFunction(PropertyName* name, unsigned ms, unsigned line, unsigned column)
         : name(name), ms(ms), line(line), column(column)
        {}

        PropertyName* name;
        unsigned ms;
        unsigned line;
        unsigned column;
    };

    typedef HashMap<PropertyName*, MathBuiltin> MathNameMap;
    typedef HashMap<PropertyName*, AsmJSAtomicsBuiltinFunction> AtomicsNameMap;
    typedef HashMap<PropertyName*, AsmJSSimdOperation> SimdOperationNameMap;
    typedef HashMap<PropertyName*, Global*> GlobalMap;
    typedef Vector<Func*> FuncVector;
    typedef Vector<AsmJSGlobalAccess> GlobalAccessVector;
    typedef Vector<SlowFunction> SlowFunctionVector;
    typedef Vector<ArrayView> ArrayViewVector;

    ExclusiveContext *             cx_;
    AsmJSParser &                  parser_;

    MacroAssembler                 masm_;

    ScopedJSDeletePtr<AsmJSModule> module_;
    LifoAlloc                      moduleLifo_;
    ParseNode *                    moduleFunctionNode_;
    PropertyName *                 moduleFunctionName_;

    GlobalMap                      globals_;
    FuncVector                     functions_;
    FuncPtrTableVector             funcPtrTables_;
    ArrayViewVector                arrayViews_;
    ExitMap                        exits_;
    MathNameMap                    standardLibraryMathNames_;
    AtomicsNameMap                 standardLibraryAtomicsNames_;
    SimdOperationNameMap           standardLibrarySimdOpNames_;
    NonAssertingLabel              stackOverflowLabel_;
    NonAssertingLabel              asyncInterruptLabel_;
    NonAssertingLabel              syncInterruptLabel_;
    NonAssertingLabel              onDetachedLabel_;
    NonAssertingLabel              onOutOfBoundsLabel_;

    UniquePtr<char[], JS::FreePolicy> errorString_;
    uint32_t                       errorOffset_;
    bool                           errorOverRecursed_;

    int64_t                        usecBefore_;
    SlowFunctionVector             slowFunctions_;

    DebugOnly<bool>                finishedFunctionBodies_;
    bool                           supportsSimd_;
    bool                           canValidateChangeHeap_;
    bool                           hasChangeHeap_;

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
    bool addStandardLibrarySimdOpName(const char* name, AsmJSSimdOperation op) {
        JSAtom* atom = Atomize(cx_, name, strlen(name));
        if (!atom)
            return false;
        return standardLibrarySimdOpNames_.putNew(atom->asPropertyName(), op);
    }

  public:
    ModuleCompiler(ExclusiveContext* cx, AsmJSParser& parser)
      : cx_(cx),
        parser_(parser),
        masm_(MacroAssembler::AsmJSToken()),
        moduleLifo_(LIFO_ALLOC_PRIMARY_CHUNK_SIZE),
        moduleFunctionNode_(parser.pc->maybeFunction),
        moduleFunctionName_(nullptr),
        globals_(cx),
        functions_(cx),
        funcPtrTables_(cx),
        arrayViews_(cx),
        exits_(cx),
        standardLibraryMathNames_(cx),
        standardLibraryAtomicsNames_(cx),
        standardLibrarySimdOpNames_(cx),
        errorString_(nullptr),
        errorOffset_(UINT32_MAX),
        errorOverRecursed_(false),
        usecBefore_(PRMJ_Now()),
        slowFunctions_(cx),
        finishedFunctionBodies_(false),
        supportsSimd_(cx->jitSupportsSimd()),
        canValidateChangeHeap_(false),
        hasChangeHeap_(false)
    {
        MOZ_ASSERT(moduleFunctionNode_->pn_funbox == parser.pc->sc->asFunctionBox());
    }

    ~ModuleCompiler() {
        if (errorString_) {
            MOZ_ASSERT(errorOffset_ != UINT32_MAX);
            tokenStream().reportAsmJSError(errorOffset_,
                                           JSMSG_USE_ASM_TYPE_FAIL,
                                           errorString_.get());
        }
        if (errorOverRecursed_)
            js_ReportOverRecursed(cx_);
    }

    bool init() {
        if (!globals_.init() || !exits_.init())
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
            !addStandardLibraryAtomicsName("load", AsmJSAtomicsBuiltin_load) ||
            !addStandardLibraryAtomicsName("store", AsmJSAtomicsBuiltin_store) ||
            !addStandardLibraryAtomicsName("fence", AsmJSAtomicsBuiltin_fence) ||
            !addStandardLibraryAtomicsName("add", AsmJSAtomicsBuiltin_add) ||
            !addStandardLibraryAtomicsName("sub", AsmJSAtomicsBuiltin_sub) ||
            !addStandardLibraryAtomicsName("and", AsmJSAtomicsBuiltin_and) ||
            !addStandardLibraryAtomicsName("or", AsmJSAtomicsBuiltin_or) ||
            !addStandardLibraryAtomicsName("xor", AsmJSAtomicsBuiltin_xor))
        {
            return false;
        }

#define ADDSTDLIBSIMDOPNAME(op) || !addStandardLibrarySimdOpName(#op, AsmJSSimdOperation_##op)
        if (!standardLibrarySimdOpNames_.init()
            FORALL_SIMD_OP(ADDSTDLIBSIMDOPNAME))
        {
            return false;
        }
#undef ADDSTDLIBSIMDOPNAME

        uint32_t srcStart = parser_.pc->maybeFunction->pn_body->pn_pos.begin;
        uint32_t srcBodyStart = tokenStream().currentToken().pos.end;

        // "use strict" should be added to the source if we are in an implicit
        // strict context, see also comment above addUseStrict in
        // js::FunctionToString.
        bool strict = parser_.pc->sc->strict && !parser_.pc->sc->hasExplicitUseStrict();
        module_ = cx_->new_<AsmJSModule>(parser_.ss, srcStart, srcBodyStart, strict,
                                         cx_->canUseSignalHandlers());
        if (!module_)
            return false;

        return true;
    }

    bool failOffset(uint32_t offset, const char* str) {
        MOZ_ASSERT(!errorString_);
        MOZ_ASSERT(errorOffset_ == UINT32_MAX);
        MOZ_ASSERT(str);
        errorOffset_ = offset;
        errorString_ = DuplicateString(cx_, str);
        return false;
    }

    bool fail(ParseNode* pn, const char* str) {
        if (pn)
            return failOffset(pn->pn_pos.begin, str);

        // The exact rooting static analysis does not perform dataflow analysis, so it believes
        // that unrooted things on the stack during compilation may still be accessed after this.
        // Since pn is typically only null under OOM, this suppression simply forces any GC to be
        // delayed until the compilation is off the stack and more memory can be freed.
        gc::AutoSuppressGC nogc(cx_);
        TokenPos pos;
        if (!tokenStream().peekTokenPos(&pos))
            return false;
        return failOffset(pos.begin, str);
    }

    bool failfVA(ParseNode* pn, const char* fmt, va_list ap) {
        MOZ_ASSERT(!errorString_);
        MOZ_ASSERT(errorOffset_ == UINT32_MAX);
        MOZ_ASSERT(fmt);
        errorOffset_ = pn ? pn->pn_pos.begin : tokenStream().currentToken().pos.end;
        errorString_.reset(JS_vsmprintf(fmt, ap));
        return false;
    }

    bool failf(ParseNode* pn, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        failfVA(pn, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failName(ParseNode* pn, const char* fmt, PropertyName* name) {
        // This function is invoked without the caller properly rooting its locals.
        gc::AutoSuppressGC suppress(cx_);
        JSAutoByteString bytes;
        if (AtomToPrintableString(cx_, name, &bytes))
            failf(pn, fmt, bytes.ptr());
        return false;
    }

    bool failOverRecursed() {
        errorOverRecursed_ = true;
        return false;
    }

    /*************************************************** Read-only interface */

    ExclusiveContext* cx() const { return cx_; }
    AsmJSParser& parser() const { return parser_; }
    TokenStream& tokenStream() const { return parser_.tokenStream; }
    MacroAssembler& masm() { return masm_; }
    Label& stackOverflowLabel() { return stackOverflowLabel_; }
    Label& asyncInterruptLabel() { return asyncInterruptLabel_; }
    Label& syncInterruptLabel() { return syncInterruptLabel_; }
    Label& onDetachedLabel() { return onDetachedLabel_; }
    Label& onOutOfBoundsLabel() { return onOutOfBoundsLabel_; }
    bool hasError() const { return errorString_ != nullptr; }
    const AsmJSModule& module() const { return *module_.get(); }
    bool usesSignalHandlersForInterrupt() const { return module_->usesSignalHandlersForInterrupt(); }
    bool usesSignalHandlersForOOB() const { return module_->usesSignalHandlersForOOB(); }
    bool supportsSimd() const { return supportsSimd_; }

    ParseNode* moduleFunctionNode() const { return moduleFunctionNode_; }
    PropertyName* moduleFunctionName() const { return moduleFunctionName_; }

    const Global* lookupGlobal(PropertyName* name) const {
        if (GlobalMap::Ptr p = globals_.lookup(name))
            return p->value();
        return nullptr;
    }
    Func* lookupFunction(PropertyName* name) {
        if (GlobalMap::Ptr p = globals_.lookup(name)) {
            Global* value = p->value();
            if (value->which() == Global::Function)
                return functions_[value->funcIndex()];
        }
        return nullptr;
    }
    unsigned numFunctions() const {
        return functions_.length();
    }
    Func& function(unsigned i) {
        return *functions_[i];
    }
    unsigned numFuncPtrTables() const {
        return funcPtrTables_.length();
    }
    FuncPtrTable& funcPtrTable(unsigned i) {
        return funcPtrTables_[i];
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
    bool lookupStandardSimdOpName(PropertyName* name, AsmJSSimdOperation* op) const {
        if (SimdOperationNameMap::Ptr p = standardLibrarySimdOpNames_.lookup(name)) {
            *op = p->value();
            return true;
        }
        return false;
    }
    ExitMap::Range allExits() const {
        return exits_.all();
    }
    unsigned numArrayViews() const {
        return arrayViews_.length();
    }
    const ArrayView& arrayView(unsigned i) const {
        return arrayViews_[i];
    }

    /***************************************************** Mutable interface */

    void initModuleFunctionName(PropertyName* name) { moduleFunctionName_ = name; }

    void initGlobalArgumentName(PropertyName* n) { module_->initGlobalArgumentName(n); }
    void initImportArgumentName(PropertyName* n) { module_->initImportArgumentName(n); }
    void initBufferArgumentName(PropertyName* n) { module_->initBufferArgumentName(n); }

    bool addGlobalVarInit(PropertyName* varName, const AsmJSNumLit& lit, bool isConst) {
        // The type of a const is the exact type of the literal (since its value
        // cannot change) which is more precise than the corresponding vartype.
        Type type = isConst ? Type::Of(lit) : VarType::Of(lit).toType();
        uint32_t index;
        if (!module_->addGlobalVarInit(lit, &index))
            return false;
        Global::Which which = isConst ? Global::ConstantLiteral : Global::Variable;
        Global* global = moduleLifo_.new_<Global>(which);
        if (!global)
            return false;
        global->u.varOrConst.index_ = index;
        global->u.varOrConst.type_ = type.which();
        if (isConst)
            global->u.varOrConst.literalValue_ = lit;
        return globals_.putNew(varName, global);
    }
    bool addGlobalVarImport(PropertyName* varName, PropertyName* fieldName, AsmJSCoercion coercion,
                            bool isConst)
    {
        uint32_t index;
        if (!module_->addGlobalVarImport(fieldName, coercion, &index))
            return false;

        Global::Which which = isConst ? Global::ConstantImport : Global::Variable;
        Global* global = moduleLifo_.new_<Global>(which);
        if (!global)
            return false;
        global->u.varOrConst.index_ = index;
        global->u.varOrConst.type_ = VarType(coercion).toType().which();
        return globals_.putNew(varName, global);
    }
    bool addFunction(PropertyName* name, Signature&& sig, Func** func) {
        MOZ_ASSERT(!finishedFunctionBodies_);
        Global* global = moduleLifo_.new_<Global>(Global::Function);
        if (!global)
            return false;
        global->u.funcIndex_ = functions_.length();
        if (!globals_.putNew(name, global))
            return false;
        Label* entry = moduleLifo_.new_<Label>();
        if (!entry)
            return false;
        *func = moduleLifo_.new_<Func>(name, Move(sig), entry);
        if (!*func)
            return false;
        return functions_.append(*func);
    }
    bool addFuncPtrTable(PropertyName* name, Signature&& sig, uint32_t mask, FuncPtrTable** table) {
        Global* global = moduleLifo_.new_<Global>(Global::FuncPtrTable);
        if (!global)
            return false;
        global->u.funcPtrTableIndex_ = funcPtrTables_.length();
        if (!globals_.putNew(name, global))
            return false;
        uint32_t globalDataOffset;
        if (!module_->addFuncPtrTable(/* numElems = */ mask + 1, &globalDataOffset))
            return false;
        FuncPtrTable tmpTable(cx_, Move(sig), mask, globalDataOffset);
        if (!funcPtrTables_.append(Move(tmpTable)))
            return false;
        *table = &funcPtrTables_.back();
        return true;
    }
    bool addByteLength(PropertyName* name) {
        canValidateChangeHeap_ = true;
        if (!module_->addByteLength())
            return false;
        Global* global = moduleLifo_.new_<Global>(Global::ByteLength);
        return global && globals_.putNew(name, global);
    }
    bool addChangeHeap(PropertyName* name, ParseNode* fn, uint32_t mask, uint32_t min, uint32_t max) {
        hasChangeHeap_ = true;
        module_->addChangeHeap(mask, min, max);
        Global* global = moduleLifo_.new_<Global>(Global::ChangeHeap);
        if (!global)
            return false;
        global->u.changeHeap.srcBegin_ = fn->pn_pos.begin;
        global->u.changeHeap.srcEnd_ = fn->pn_pos.end;
        return globals_.putNew(name, global);
    }
    bool addFFI(PropertyName* varName, PropertyName* field) {
        Global* global = moduleLifo_.new_<Global>(Global::FFI);
        if (!global)
            return false;
        uint32_t index;
        if (!module_->addFFI(field, &index))
            return false;
        global->u.ffiIndex_ = index;
        return globals_.putNew(varName, global);
    }
    bool addArrayView(PropertyName* varName, Scalar::Type vt, PropertyName* maybeField, bool isSharedView) {
        if (!arrayViews_.append(ArrayView(varName, vt)))
            return false;
        Global* global = moduleLifo_.new_<Global>(Global::ArrayView);
        if (!global)
            return false;
        if (!module_->addArrayView(vt, maybeField, isSharedView))
            return false;
        global->u.viewType_ = vt;
        return globals_.putNew(varName, global);
    }
    bool addArrayViewCtor(PropertyName* varName, Scalar::Type vt, PropertyName* fieldName, bool isSharedView) {
        Global* global = moduleLifo_.new_<Global>(Global::ArrayViewCtor);
        if (!global)
            return false;
        if (!module_->addArrayViewCtor(vt, fieldName, isSharedView))
            return false;
        global->u.viewType_ = vt;
        return globals_.putNew(varName, global);
    }
    bool addMathBuiltinFunction(PropertyName* varName, AsmJSMathBuiltinFunction func, PropertyName* fieldName) {
        if (!module_->addMathBuiltinFunction(func, fieldName))
            return false;
        Global* global = moduleLifo_.new_<Global>(Global::MathBuiltinFunction);
        if (!global)
            return false;
        global->u.mathBuiltinFunc_ = func;
        return globals_.putNew(varName, global);
    }
    bool addAtomicsBuiltinFunction(PropertyName* varName, AsmJSAtomicsBuiltinFunction func, PropertyName* fieldName) {
        if (!module_->addAtomicsBuiltinFunction(func, fieldName))
            return false;
        Global* global = moduleLifo_.new_<Global>(Global::AtomicsBuiltinFunction);
        if (!global)
            return false;
        global->u.atomicsBuiltinFunc_ = func;
        return globals_.putNew(varName, global);
    }
    bool addSimdCtor(PropertyName* varName, AsmJSSimdType type, PropertyName* fieldName) {
        if (!module_->addSimdCtor(type, fieldName))
            return false;
        Global* global = moduleLifo_.new_<Global>(Global::SimdCtor);
        if (!global)
            return false;
        global->u.simdCtorType_ = type;
        return globals_.putNew(varName, global);
    }
    bool addSimdOperation(PropertyName* varName, AsmJSSimdType type, AsmJSSimdOperation op,
                          PropertyName* typeVarName, PropertyName* opName)
    {
        if (!module_->addSimdOperation(type, op, opName))
            return false;
        Global* global = moduleLifo_.new_<Global>(Global::SimdOperation);
        if (!global)
            return false;
        global->u.simdOp.type_ = type;
        global->u.simdOp.which_ = op;
        return globals_.putNew(varName, global);
    }
  private:
    bool addGlobalDoubleConstant(PropertyName* varName, double constant) {
        Global* global = moduleLifo_.new_<Global>(Global::ConstantLiteral);
        if (!global)
            return false;
        global->u.varOrConst.type_ = Type::Double;
        global->u.varOrConst.literalValue_ = AsmJSNumLit::Create(AsmJSNumLit::Double,
                                                                 DoubleValue(constant));
        return globals_.putNew(varName, global);
    }
  public:
    bool addMathBuiltinConstant(PropertyName* varName, double constant, PropertyName* fieldName) {
        if (!module_->addMathBuiltinConstant(constant, fieldName))
            return false;
        return addGlobalDoubleConstant(varName, constant);
    }
    bool addGlobalConstant(PropertyName* varName, double constant, PropertyName* fieldName) {
        if (!module_->addGlobalConstant(constant, fieldName))
            return false;
        return addGlobalDoubleConstant(varName, constant);
    }
    bool addExportedFunction(const Func& func, PropertyName* maybeFieldName) {
        AsmJSModule::ArgCoercionVector argCoercions;
        const VarTypeVector& args = func.sig().args();
        if (!argCoercions.resize(args.length()))
            return false;
        for (unsigned i = 0; i < args.length(); i++)
            argCoercions[i] = args[i].toCoercion();
        AsmJSModule::ReturnType retType = func.sig().retType().toModuleReturnType();
        return module_->addExportedFunction(func.name(), func.srcBegin(), func.srcEnd(),
                                            maybeFieldName, Move(argCoercions), retType);
    }
    bool addExportedChangeHeap(PropertyName* name, const Global& g, PropertyName* maybeFieldName) {
        return module_->addExportedChangeHeap(name, g.changeHeapSrcBegin(), g.changeHeapSrcEnd(),
                                              maybeFieldName);
    }
    bool addExit(unsigned ffiIndex, PropertyName* name, Signature&& sig, unsigned* exitIndex) {
        ExitDescriptor exitDescriptor(name, Move(sig));
        ExitMap::AddPtr p = exits_.lookupForAdd(exitDescriptor);
        if (p) {
            *exitIndex = p->value();
            return true;
        }
        if (!module_->addExit(ffiIndex, exitIndex))
            return false;
        return exits_.add(p, Move(exitDescriptor), *exitIndex);
    }

    bool tryRequireHeapLengthToBeAtLeast(uint32_t len) {
        return module_->tryRequireHeapLengthToBeAtLeast(len);
    }
    uint32_t minHeapLength() const {
        return module_->minHeapLength();
    }
    LifoAlloc& lifo() {
        return moduleLifo_;
    }

    void startFunctionBodies() {
        module_->startFunctionBodies();
    }
    bool tryOnceToValidateChangeHeap() {
        bool ret = canValidateChangeHeap_;
        canValidateChangeHeap_ = false;
        return ret;
    }
    bool hasChangeHeap() const {
        return hasChangeHeap_;
    }
    bool finishGeneratingFunction(Func& func, CodeGenerator& codegen,
                                  const AsmJSFunctionLabels& labels)
    {
        uint32_t line, column;
        tokenStream().srcCoords.lineNumAndColumnIndex(func.srcBegin(), &line, &column);

        if (!module_->addFunctionCodeRange(func.name(), line, labels))
            return false;

        jit::IonScriptCounts* counts = codegen.extractScriptCounts();
        if (counts && !module_->addFunctionCounts(counts)) {
            js_delete(counts);
            return false;
        }

        if (func.compileTime() >= 250) {
            SlowFunction sf(func.name(), func.compileTime(), line, column);
            if (!slowFunctions_.append(sf))
                return false;
        }

#if defined(MOZ_VTUNE) || defined(JS_ION_PERF)
        unsigned begin = labels.begin.offset();
        unsigned end = labels.end.offset();
        if (!module_->addProfiledFunction(func.name(), begin, end, line, column))
            return false;
# ifdef JS_ION_PERF
        // Per-block profiling info uses significantly more memory so only store
        // this information if it is actively requested.
        if (PerfBlockEnabled()) {
            AsmJSPerfSpewer& ps = codegen.mirGen().perfSpewer();
            ps.noteBlocksOffsets();
            unsigned inlineEnd = ps.endInlineCode.offset();
            if (!module_->addProfiledBlocks(func.name(), begin, inlineEnd, end, ps.basicBlocks()))
                return false;
        }
# endif
#endif
        return true;
    }
    void finishFunctionBodies() {
        // When an interrupt is triggered, all function code is mprotected and,
        // for sanity, stub code (particularly the interrupt stub) is not.
        // Protection works at page granularity, so we need to ensure that no
        // stub code gets into the function code pages.
        MOZ_ASSERT(!finishedFunctionBodies_);
        masm_.align(AsmJSPageSize);
        module_->finishFunctionBodies(masm_.currentOffset());
        finishedFunctionBodies_ = true;
    }

    bool finishGeneratingEntry(unsigned exportIndex, Label* begin) {
        MOZ_ASSERT(finishedFunctionBodies_);
        module_->exportedFunction(exportIndex).initCodeOffset(begin->offset());
        uint32_t end = masm_.currentOffset();
        return module_->addCodeRange(AsmJSModule::CodeRange::Entry, begin->offset(), end);
    }
    bool finishGeneratingInterpExit(unsigned exitIndex, Label* begin, Label* profilingReturn) {
        MOZ_ASSERT(finishedFunctionBodies_);
        uint32_t beg = begin->offset();
        module_->exit(exitIndex).initInterpOffset(beg);
        uint32_t pret = profilingReturn->offset();
        uint32_t end = masm_.currentOffset();
        return module_->addCodeRange(AsmJSModule::CodeRange::SlowFFI, beg, pret, end);
    }
    bool finishGeneratingJitExit(unsigned exitIndex, Label* begin, Label* profilingReturn) {
        MOZ_ASSERT(finishedFunctionBodies_);
        uint32_t beg = begin->offset();
        module_->exit(exitIndex).initJitOffset(beg);
        uint32_t pret = profilingReturn->offset();
        uint32_t end = masm_.currentOffset();
        return module_->addCodeRange(AsmJSModule::CodeRange::JitFFI, beg, pret, end);
    }
    bool finishGeneratingInterrupt(Label* begin, Label* profilingReturn) {
        MOZ_ASSERT(finishedFunctionBodies_);
        uint32_t beg = begin->offset();
        uint32_t pret = profilingReturn->offset();
        uint32_t end = masm_.currentOffset();
        return module_->addCodeRange(AsmJSModule::CodeRange::Interrupt, beg, pret, end);
    }
    bool finishGeneratingInlineStub(Label* begin) {
        MOZ_ASSERT(finishedFunctionBodies_);
        uint32_t end = masm_.currentOffset();
        return module_->addCodeRange(AsmJSModule::CodeRange::Inline, begin->offset(), end);
    }
    bool finishGeneratingBuiltinThunk(AsmJSExit::BuiltinKind builtin, Label* begin, Label* pret) {
        MOZ_ASSERT(finishedFunctionBodies_);
        uint32_t end = masm_.currentOffset();
        return module_->addBuiltinThunkCodeRange(builtin, begin->offset(), pret->offset(), end);
    }

    void buildCompilationTimeReport(JS::AsmJSCacheResult cacheResult, ScopedJSFreePtr<char>* out) {
        ScopedJSFreePtr<char> slowFuns;
#ifndef JS_MORE_DETERMINISTIC
        int64_t usecAfter = PRMJ_Now();
        int msTotal = (usecAfter - usecBefore_) / PRMJ_USEC_PER_MSEC;
        if (!slowFunctions_.empty()) {
            slowFuns.reset(JS_smprintf("; %d functions compiled slowly: ", slowFunctions_.length()));
            if (!slowFuns)
                return;
            for (unsigned i = 0; i < slowFunctions_.length(); i++) {
                SlowFunction& func = slowFunctions_[i];
                JSAutoByteString name;
                if (!AtomToPrintableString(cx_, func.name, &name))
                    return;
                slowFuns.reset(JS_smprintf("%s%s:%u:%u (%ums)%s", slowFuns.get(),
                                           name.ptr(), func.line, func.column, func.ms,
                                           i+1 < slowFunctions_.length() ? ", " : ""));
                if (!slowFuns)
                    return;
            }
        }
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
          case JS::AsmJSCache_LIMIT:
            MOZ_CRASH("bad AsmJSCacheResult");
            break;
        }
        out->reset(JS_smprintf("total compilation time %dms; %s%s",
                               msTotal, cacheString, slowFuns ? slowFuns.get() : ""));
#endif
    }

    bool finish(ScopedJSDeletePtr<AsmJSModule>* module)
    {
        masm_.finish();
        if (masm_.oom())
            return false;

        if (!module_->finish(cx_, tokenStream(), masm_, asyncInterruptLabel_, onOutOfBoundsLabel_))
            return false;

        // Finally, convert all the function-pointer table elements into
        // RelativeLinks that will be patched by AsmJSModule::staticallyLink.
        for (unsigned tableIndex = 0; tableIndex < funcPtrTables_.length(); tableIndex++) {
            FuncPtrTable& table = funcPtrTables_[tableIndex];
            unsigned tableBaseOffset = module_->offsetOfGlobalData() + table.globalDataOffset();
            for (unsigned elemIndex = 0; elemIndex < table.numElems(); elemIndex++) {
                AsmJSModule::RelativeLink link(AsmJSModule::RelativeLink::RawPointer);
                link.patchAtOffset = tableBaseOffset + elemIndex * sizeof(uint8_t*);
                link.targetOffset = masm_.actualOffset(table.elem(elemIndex).entry().offset());
                if (!module_->addRelativeLink(link))
                    return false;
            }
        }

        *module = module_.forget();
        return true;
    }
};

} /* anonymous namespace */

/*****************************************************************************/
// Numeric literal utilities

static bool
IsNumericNonFloatLiteral(ParseNode* pn)
{
    // Note: '-' is never rolled into the number; numbers are always positive
    // and negations must be applied manually.
    return pn->isKind(PNK_NUMBER) ||
           (pn->isKind(PNK_NEG) && UnaryKid(pn)->isKind(PNK_NUMBER));
}

static bool
IsCallToGlobal(ModuleCompiler& m, ParseNode* pn, const ModuleCompiler::Global** global)
{
    if (!pn->isKind(PNK_CALL))
        return false;

    ParseNode* callee = CallCallee(pn);
    if (!callee->isKind(PNK_NAME))
        return false;

    *global = m.lookupGlobal(callee->name());
    return !!*global;
}

static bool
IsCoercionCall(ModuleCompiler& m, ParseNode* pn, AsmJSCoercion* coercion, ParseNode** coercedExpr)
{
    const ModuleCompiler::Global* global;
    if (!IsCallToGlobal(m, pn, &global))
        return false;

    if (CallArgListLength(pn) != 1)
        return false;

    if (coercedExpr)
        *coercedExpr = CallArgList(pn);

    if (global->isMathFunction() && global->mathBuiltinFunction() == AsmJSMathBuiltin_fround) {
        *coercion = AsmJS_FRound;
        return true;
    }

    if (global->isSimdOperation() && global->simdOperation() == AsmJSSimdOperation_check) {
        switch (global->simdOperationType()) {
          case AsmJSSimdType_int32x4:
            *coercion = AsmJS_ToInt32x4;
            return true;
          case AsmJSSimdType_float32x4:
            *coercion = AsmJS_ToFloat32x4;
            return true;
        }
    }

    return false;
}

static bool
IsFloatLiteral(ModuleCompiler& m, ParseNode* pn)
{
    ParseNode* coercedExpr;
    AsmJSCoercion coercion;
    if (!IsCoercionCall(m, pn, &coercion, &coercedExpr))
        return false;
    // Don't fold into || to avoid clang/memcheck bug (bug 1077031).
    if (coercion != AsmJS_FRound)
        return false;
    return IsNumericNonFloatLiteral(coercedExpr);
}

static unsigned
SimdTypeToLength(AsmJSSimdType type)
{
    switch (type) {
      case AsmJSSimdType_float32x4:
      case AsmJSSimdType_int32x4:
        return 4;
    }
    MOZ_CRASH("unexpected SIMD type");
}

static bool
IsSimdTuple(ModuleCompiler& m, ParseNode* pn, AsmJSSimdType* type)
{
    const ModuleCompiler::Global* global;
    if (!IsCallToGlobal(m, pn, &global))
        return false;

    if (!global->isSimdCtor())
        return false;

    if (CallArgListLength(pn) != SimdTypeToLength(global->simdCtorType()))
        return false;

    *type = global->simdCtorType();
    return true;
}

static bool
IsNumericLiteral(ModuleCompiler& m, ParseNode* pn);

static AsmJSNumLit
ExtractNumericLiteral(ModuleCompiler& m, ParseNode* pn);

static inline bool
IsLiteralInt(ModuleCompiler& m, ParseNode* pn, uint32_t* u32);

static bool
IsSimdLiteral(ModuleCompiler& m, ParseNode* pn)
{
    AsmJSSimdType type;
    if (!IsSimdTuple(m, pn, &type))
        return false;

    ParseNode* arg = CallArgList(pn);
    unsigned length = SimdTypeToLength(type);
    for (unsigned i = 0; i < length; i++) {
        if (!IsNumericLiteral(m, arg))
            return false;

        uint32_t _;
        switch (type) {
          case AsmJSSimdType_int32x4:
            if (!IsLiteralInt(m, arg, &_))
                return false;
          case AsmJSSimdType_float32x4:
            if (!IsNumericNonFloatLiteral(arg))
                return false;
        }

        arg = NextNode(arg);
    }

    MOZ_ASSERT(arg == nullptr);
    return true;
}

static bool
IsNumericLiteral(ModuleCompiler& m, ParseNode* pn)
{
    return IsNumericNonFloatLiteral(pn) ||
           IsFloatLiteral(m, pn) ||
           IsSimdLiteral(m, pn);
}

// The JS grammar treats -42 as -(42) (i.e., with separate grammar
// productions) for the unary - and literal 42). However, the asm.js spec
// recognizes -42 (modulo parens, so -(42) and -((42))) as a single literal
// so fold the two potential parse nodes into a single double value.
static double
ExtractNumericNonFloatValue(ParseNode* pn, ParseNode** out = nullptr)
{
    MOZ_ASSERT(IsNumericNonFloatLiteral(pn));

    if (pn->isKind(PNK_NEG)) {
        pn = UnaryKid(pn);
        if (out)
            *out = pn;
        return -NumberNodeValue(pn);
    }

    return NumberNodeValue(pn);
}

static AsmJSNumLit
ExtractSimdValue(ModuleCompiler& m, ParseNode* pn)
{
    MOZ_ASSERT(IsSimdLiteral(m, pn));

    AsmJSSimdType type;
    JS_ALWAYS_TRUE(IsSimdTuple(m, pn, &type));

    ParseNode* arg = CallArgList(pn);
    switch (type) {
      case AsmJSSimdType_int32x4: {
        MOZ_ASSERT(SimdTypeToLength(type) == 4);
        int32_t val[4];
        for (size_t i = 0; i < 4; i++, arg = NextNode(arg)) {
            uint32_t u32;
            JS_ALWAYS_TRUE(IsLiteralInt(m, arg, &u32));
            val[i] = int32_t(u32);
        }
        MOZ_ASSERT(arg== nullptr);
        return AsmJSNumLit::Create(AsmJSNumLit::Int32x4, SimdConstant::CreateX4(val));
      }
      case AsmJSSimdType_float32x4: {
        MOZ_ASSERT(SimdTypeToLength(type) == 4);
        float val[4];
        for (size_t i = 0; i < 4; i++, arg = NextNode(arg))
            val[i] = float(ExtractNumericNonFloatValue(arg));
        MOZ_ASSERT(arg == nullptr);
        return AsmJSNumLit::Create(AsmJSNumLit::Float32x4, SimdConstant::CreateX4(val));
      }
    }

    MOZ_CRASH("Unexpected SIMD type.");
}

static AsmJSNumLit
ExtractNumericLiteral(ModuleCompiler& m, ParseNode* pn)
{
    MOZ_ASSERT(IsNumericLiteral(m, pn));

    if (pn->isKind(PNK_CALL)) {
        // Float literals are explicitly coerced and thus the coerced literal may be
        // any valid (non-float) numeric literal.
        if (CallArgListLength(pn) == 1) {
            pn = CallArgList(pn);
            double d = ExtractNumericNonFloatValue(pn);
            return AsmJSNumLit::Create(AsmJSNumLit::Float, DoubleValue(d));
        }

        MOZ_ASSERT(CallArgListLength(pn) == 4);
        return ExtractSimdValue(m, pn);
    }

    double d = ExtractNumericNonFloatValue(pn, &pn);

    // The asm.js spec syntactically distinguishes any literal containing a
    // decimal point or the literal -0 as having double type.
    if (NumberNodeHasFrac(pn) || IsNegativeZero(d))
        return AsmJSNumLit::Create(AsmJSNumLit::Double, DoubleValue(d));

    // The syntactic checks above rule out these double values.
    MOZ_ASSERT(!IsNegativeZero(d));
    MOZ_ASSERT(!IsNaN(d));

    // Although doubles can only *precisely* represent 53-bit integers, they
    // can *imprecisely* represent integers much bigger than an int64_t.
    // Furthermore, d may be inf or -inf. In both cases, casting to an int64_t
    // is undefined, so test against the integer bounds using doubles.
    if (d < double(INT32_MIN) || d > double(UINT32_MAX))
        return AsmJSNumLit::Create(AsmJSNumLit::OutOfRangeInt, UndefinedValue());

    // With the above syntactic and range limitations, d is definitely an
    // integer in the range [INT32_MIN, UINT32_MAX] range.
    int64_t i64 = int64_t(d);
    if (i64 >= 0) {
        if (i64 <= INT32_MAX)
            return AsmJSNumLit::Create(AsmJSNumLit::Fixnum, Int32Value(i64));
        MOZ_ASSERT(i64 <= UINT32_MAX);
        return AsmJSNumLit::Create(AsmJSNumLit::BigUnsigned, Int32Value(uint32_t(i64)));
    }
    MOZ_ASSERT(i64 >= INT32_MIN);
    return AsmJSNumLit::Create(AsmJSNumLit::NegativeInt, Int32Value(i64));
}

static inline bool
IsLiteralInt(AsmJSNumLit lit, uint32_t* u32)
{
    switch (lit.which()) {
      case AsmJSNumLit::Fixnum:
      case AsmJSNumLit::BigUnsigned:
      case AsmJSNumLit::NegativeInt:
        *u32 = uint32_t(lit.toInt32());
        return true;
      case AsmJSNumLit::Double:
      case AsmJSNumLit::Float:
      case AsmJSNumLit::OutOfRangeInt:
      case AsmJSNumLit::Int32x4:
      case AsmJSNumLit::Float32x4:
        return false;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad literal type");
}

static inline bool
IsLiteralInt(ModuleCompiler& m, ParseNode* pn, uint32_t* u32)
{
    return IsNumericLiteral(m, pn) &&
           IsLiteralInt(ExtractNumericLiteral(m, pn), u32);
}

/*****************************************************************************/

namespace {

// Encapsulates the compilation of a single function in an asm.js module. The
// function compiler handles the creation and final backend compilation of the
// MIR graph. Also see ModuleCompiler comment.
class FunctionCompiler
{
  public:
    struct Local
    {
        VarType type;
        unsigned slot;
        Local(VarType t, unsigned slot) : type(t), slot(slot) {}
    };

  private:
    typedef HashMap<PropertyName*, Local> LocalMap;
    typedef Vector<AsmJSNumLit> VarInitializerVector;
    typedef HashMap<PropertyName*, BlockVector> LabeledBlockMap;
    typedef HashMap<ParseNode*, BlockVector> UnlabeledBlockMap;
    typedef Vector<ParseNode*, 4> NodeStack;

    ModuleCompiler &       m_;
    LifoAlloc &            lifo_;
    ParseNode *            fn_;

    LocalMap               locals_;
    VarInitializerVector   varInitializers_;
    Maybe<RetType>         alreadyReturned_;

    TempAllocator *        alloc_;
    MIRGraph *             graph_;
    CompileInfo *          info_;
    MIRGenerator *         mirGen_;
    Maybe<JitContext>      jitContext_;

    MBasicBlock *          curBlock_;

    NodeStack              loopStack_;
    NodeStack              breakableStack_;
    UnlabeledBlockMap      unlabeledBreaks_;
    UnlabeledBlockMap      unlabeledContinues_;
    LabeledBlockMap        labeledBreaks_;
    LabeledBlockMap        labeledContinues_;

    unsigned               heapExpressionDepth_;

  public:
    FunctionCompiler(ModuleCompiler& m, ParseNode* fn, LifoAlloc& lifo)
      : m_(m),
        lifo_(lifo),
        fn_(fn),
        locals_(m.cx()),
        varInitializers_(m.cx()),
        alloc_(nullptr),
        graph_(nullptr),
        info_(nullptr),
        mirGen_(nullptr),
        curBlock_(nullptr),
        loopStack_(m.cx()),
        breakableStack_(m.cx()),
        unlabeledBreaks_(m.cx()),
        unlabeledContinues_(m.cx()),
        labeledBreaks_(m.cx()),
        labeledContinues_(m.cx()),
        heapExpressionDepth_(0)
    {}

    ModuleCompiler &    m() const      { return m_; }
    TempAllocator &     alloc() const  { return *alloc_; }
    LifoAlloc &         lifo() const   { return lifo_; }
    ParseNode *         fn() const     { return fn_; }
    ExclusiveContext *  cx() const     { return m_.cx(); }
    const AsmJSModule & module() const { return m_.module(); }

    bool init()
    {
        return locals_.init() &&
               unlabeledBreaks_.init() &&
               unlabeledContinues_.init() &&
               labeledBreaks_.init() &&
               labeledContinues_.init();
    }

    bool fail(ParseNode* pn, const char* str)
    {
        return m_.fail(pn, str);
    }

    bool failf(ParseNode* pn, const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        m_.failfVA(pn, fmt, ap);
        va_end(ap);
        return false;
    }

    bool failName(ParseNode* pn, const char* fmt, PropertyName* name)
    {
        return m_.failName(pn, fmt, name);
    }

    ~FunctionCompiler()
    {
#ifdef DEBUG
        if (!m().hasError() && cx()->isJSContext() && !cx()->asJSContext()->isExceptionPending()) {
            MOZ_ASSERT(loopStack_.empty());
            MOZ_ASSERT(unlabeledBreaks_.empty());
            MOZ_ASSERT(unlabeledContinues_.empty());
            MOZ_ASSERT(labeledBreaks_.empty());
            MOZ_ASSERT(labeledContinues_.empty());
            MOZ_ASSERT(inDeadCode());
        }
#endif
    }

    /***************************************************** Local scope setup */

    bool addFormal(ParseNode* pn, PropertyName* name, VarType type)
    {
        LocalMap::AddPtr p = locals_.lookupForAdd(name);
        if (p)
            return failName(pn, "duplicate local name '%s' not allowed", name);
        return locals_.add(p, name, Local(type, locals_.count()));
    }

    bool addVariable(ParseNode* pn, PropertyName* name, const AsmJSNumLit& init)
    {
        LocalMap::AddPtr p = locals_.lookupForAdd(name);
        if (p)
            return failName(pn, "duplicate local name '%s' not allowed", name);
        if (!locals_.add(p, name, Local(VarType::Of(init), locals_.count())))
            return false;
        return varInitializers_.append(init);
    }

    bool prepareToEmitMIR(const VarTypeVector& argTypes)
    {
        MOZ_ASSERT(locals_.count() == argTypes.length() + varInitializers_.length());

        alloc_  = lifo_.new_<TempAllocator>(&lifo_);
        jitContext_.emplace(m_.cx(), alloc_);

        graph_  = lifo_.new_<MIRGraph>(alloc_);
        info_   = lifo_.new_<CompileInfo>(locals_.count());
        const OptimizationInfo* optimizationInfo = js_IonOptimizations.get(Optimization_AsmJS);
        const JitCompileOptions options;
        mirGen_ = lifo_.new_<MIRGenerator>(CompileCompartment::get(cx()->compartment()),
                                           options, alloc_,
                                           graph_, info_, optimizationInfo);

        if (!newBlock(/* pred = */ nullptr, &curBlock_, fn_))
            return false;

        for (ABIArgTypeIter i(argTypes); !i.done(); i++) {
            MAsmJSParameter* ins = MAsmJSParameter::New(alloc(), *i, i.mirType());
            curBlock_->add(ins);
            curBlock_->initSlot(info().localSlot(i.index()), ins);
            if (!mirGen_->ensureBallast())
                return false;
        }
        unsigned firstLocalSlot = argTypes.length();
        for (unsigned i = 0; i < varInitializers_.length(); i++) {
            AsmJSNumLit& lit = varInitializers_[i];
            MIRType type = Type::Of(lit).toMIRType();

            MInstruction* ins;
            if (lit.isSimd())
               ins = MSimdConstant::New(alloc(), lit.simdValue(), type);
            else
               ins = MConstant::NewAsmJS(alloc(), lit.scalarValue(), type);

            curBlock_->add(ins);
            curBlock_->initSlot(info().localSlot(firstLocalSlot + i), ins);
            if (!mirGen_->ensureBallast())
                return false;
        }
        maybeAddInterruptCheck(fn_);
        return true;
    }

    /******************************* For consistency of returns in a function */

    bool hasAlreadyReturned() const {
        return alreadyReturned_.isSome();
    }

    RetType returnedType() const {
        return *alreadyReturned_;
    }

    void setReturnedType(RetType retType) {
        alreadyReturned_.emplace(retType);
    }

    /************************* Read-only interface (after local scope setup) */

    MIRGenerator & mirGen() const     { MOZ_ASSERT(mirGen_); return *mirGen_; }
    MIRGraph &     mirGraph() const   { MOZ_ASSERT(graph_); return *graph_; }
    CompileInfo &  info() const       { MOZ_ASSERT(info_); return *info_; }

    const Local* lookupLocal(PropertyName* name) const
    {
        if (LocalMap::Ptr p = locals_.lookup(name))
            return &p->value();
        return nullptr;
    }

    MDefinition* getLocalDef(const Local& local)
    {
        if (inDeadCode())
            return nullptr;
        return curBlock_->getSlot(info().localSlot(local.slot));
    }

    const ModuleCompiler::Global* lookupGlobal(PropertyName* name) const
    {
        if (locals_.has(name))
            return nullptr;
        return m_.lookupGlobal(name);
    }

    bool supportsSimd() const {
        return m_.supportsSimd();
    }

    /*************************************************************************/

    void enterHeapExpression() {
        heapExpressionDepth_++;
    }
    void leaveHeapExpression() {
        MOZ_ASSERT(heapExpressionDepth_ > 0);
        heapExpressionDepth_--;
    }
    bool canCall() const {
        return heapExpressionDepth_ == 0 || !m_.hasChangeHeap();
    }

    /***************************** Code generation (after local scope setup) */

    MDefinition* constant(const AsmJSNumLit& lit)
    {
        if (inDeadCode())
            return nullptr;

        MInstruction* constant;
        if (lit.isSimd())
            constant = MSimdConstant::New(alloc(), lit.simdValue(), Type::Of(lit).toMIRType());
        else
            constant = MConstant::NewAsmJS(alloc(), lit.scalarValue(), Type::Of(lit).toMIRType());

        curBlock_->add(constant);
        return constant;
    }

    MDefinition* constant(Value v, Type t)
    {
        if (inDeadCode())
            return nullptr;
        MConstant* constant = MConstant::NewAsmJS(alloc(), v, t.toMIRType());
        curBlock_->add(constant);
        return constant;
    }

    template <class T>
    MDefinition* unary(MDefinition* op)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), op);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* unary(MDefinition* op, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), op, type);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* binary(MDefinition* lhs, MDefinition* rhs)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::New(alloc(), lhs, rhs);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* binary(MDefinition* lhs, MDefinition* rhs, MIRType type)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), lhs, rhs, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* unarySimd(MDefinition* input, MSimdUnaryArith::Operation op, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(input->type()) && input->type() == type);
        MInstruction* ins = MSimdUnaryArith::NewAsmJS(alloc(), input, op, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* binarySimd(MDefinition* lhs, MDefinition* rhs, MSimdBinaryArith::Operation op,
                            MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(lhs->type()) && rhs->type() == lhs->type());
        MOZ_ASSERT(lhs->type() == type);
        MSimdBinaryArith* ins = MSimdBinaryArith::NewAsmJS(alloc(), lhs, rhs, op, type);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* binarySimd(MDefinition* lhs, MDefinition* rhs, MSimdBinaryBitwise::Operation op,
                            MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(lhs->type()) && rhs->type() == lhs->type());
        MOZ_ASSERT(lhs->type() == type);
        MSimdBinaryBitwise* ins = MSimdBinaryBitwise::NewAsmJS(alloc(), lhs, rhs, op, type);
        curBlock_->add(ins);
        return ins;
    }

    template<class T>
    MDefinition* binarySimd(MDefinition* lhs, MDefinition* rhs, typename T::Operation op)
    {
        if (inDeadCode())
            return nullptr;

        T* ins = T::NewAsmJS(alloc(), lhs, rhs, op);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* swizzleSimd(MDefinition* vector, int32_t X, int32_t Y, int32_t Z, int32_t W,
                             MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MSimdSwizzle* ins = MSimdSwizzle::NewAsmJS(alloc(), vector, type, X, Y, Z, W);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* shuffleSimd(MDefinition* lhs, MDefinition* rhs, int32_t X, int32_t Y,
                             int32_t Z, int32_t W, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MInstruction* ins = MSimdShuffle::NewAsmJS(alloc(), lhs, rhs, type, X, Y, Z, W);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* insertElementSimd(MDefinition* vec, MDefinition* val, SimdLane lane, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(vec->type()) && vec->type() == type);
        MOZ_ASSERT(!IsSimdType(val->type()));
        MSimdInsertElement* ins = MSimdInsertElement::NewAsmJS(alloc(), vec, val, type, lane);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* selectSimd(MDefinition* mask, MDefinition* lhs, MDefinition* rhs, MIRType type,
                            bool isElementWise)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(mask->type()));
        MOZ_ASSERT(mask->type() == MIRType_Int32x4);
        MOZ_ASSERT(IsSimdType(lhs->type()) && rhs->type() == lhs->type());
        MOZ_ASSERT(lhs->type() == type);
        MSimdSelect* ins = MSimdSelect::NewAsmJS(alloc(), mask, lhs, rhs, type, isElementWise);
        curBlock_->add(ins);
        return ins;
    }

    template<class T>
    MDefinition* convertSimd(MDefinition* vec, MIRType from, MIRType to)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(from) && IsSimdType(to) && from != to);
        T* ins = T::NewAsmJS(alloc(), vec, from, to);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* splatSimd(MDefinition* v, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(type));
        MSimdSplatX4* ins = MSimdSplatX4::New(alloc(), type, v);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* minMax(MDefinition* lhs, MDefinition* rhs, MIRType type, bool isMax) {
        if (inDeadCode())
            return nullptr;
        MMinMax* ins = MMinMax::New(alloc(), lhs, rhs, type, isMax);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* mul(MDefinition* lhs, MDefinition* rhs, MIRType type, MMul::Mode mode)
    {
        if (inDeadCode())
            return nullptr;
        MMul* ins = MMul::New(alloc(), lhs, rhs, type, mode);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* div(MDefinition* lhs, MDefinition* rhs, MIRType type, bool unsignd)
    {
        if (inDeadCode())
            return nullptr;
        MDiv* ins = MDiv::NewAsmJS(alloc(), lhs, rhs, type, unsignd);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* mod(MDefinition* lhs, MDefinition* rhs, MIRType type, bool unsignd)
    {
        if (inDeadCode())
            return nullptr;
        MMod* ins = MMod::NewAsmJS(alloc(), lhs, rhs, type, unsignd);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* bitwise(MDefinition* lhs, MDefinition* rhs)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), lhs, rhs);
        curBlock_->add(ins);
        return ins;
    }

    template <class T>
    MDefinition* bitwise(MDefinition* op)
    {
        if (inDeadCode())
            return nullptr;
        T* ins = T::NewAsmJS(alloc(), op);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* compare(MDefinition* lhs, MDefinition* rhs, JSOp op, MCompare::CompareType type)
    {
        if (inDeadCode())
            return nullptr;
        MCompare* ins = MCompare::NewAsmJS(alloc(), lhs, rhs, op, type);
        curBlock_->add(ins);
        return ins;
    }

    void assign(const Local& local, MDefinition* def)
    {
        if (inDeadCode())
            return;
        curBlock_->setSlot(info().localSlot(local.slot), def);
    }

    MDefinition* loadHeap(Scalar::Type accessType, MDefinition* ptr, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK && !m().usesSignalHandlersForOOB();
        MOZ_ASSERT(!Scalar::isSimdType(accessType), "SIMD loads should use loadSimdHeap");
        MAsmJSLoadHeap* load = MAsmJSLoadHeap::New(alloc(), accessType, ptr, needsBoundsCheck);
        curBlock_->add(load);
        return load;
    }

    MDefinition* loadSimdHeap(Scalar::Type accessType, MDefinition* ptr, NeedsBoundsCheck chk,
                              unsigned numElems)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK && !m().usesSignalHandlersForOOB();
        MOZ_ASSERT(Scalar::isSimdType(accessType), "loadSimdHeap can only load from a SIMD view");
        Label* outOfBoundsLabel = &m().onOutOfBoundsLabel();
        MAsmJSLoadHeap* load = MAsmJSLoadHeap::New(alloc(), accessType, ptr, needsBoundsCheck,
                                                   outOfBoundsLabel, numElems);
        curBlock_->add(load);
        return load;
    }

    void storeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK && !m().usesSignalHandlersForOOB();
        MOZ_ASSERT(!Scalar::isSimdType(accessType), "SIMD stores should use loadSimdHeap");
        MAsmJSStoreHeap* store = MAsmJSStoreHeap::New(alloc(), accessType, ptr, v, needsBoundsCheck);
        curBlock_->add(store);
    }

    void storeSimdHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v,
                       NeedsBoundsCheck chk, unsigned numElems)
    {
        if (inDeadCode())
            return;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK && !m().usesSignalHandlersForOOB();
        MOZ_ASSERT(Scalar::isSimdType(accessType), "storeSimdHeap can only load from a SIMD view");
        Label* outOfBoundsLabel = &m().onOutOfBoundsLabel();
        MAsmJSStoreHeap* store = MAsmJSStoreHeap::New(alloc(), accessType, ptr, v, needsBoundsCheck,
                                                      outOfBoundsLabel, numElems);
        curBlock_->add(store);
    }

    void memoryBarrier(MemoryBarrierBits type)
    {
        if (inDeadCode())
            return;
        MMemoryBarrier* ins = MMemoryBarrier::New(alloc(), type);
        curBlock_->add(ins);
    }

    MDefinition* atomicLoadHeap(Scalar::Type accessType, MDefinition* ptr, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK && !m().usesSignalHandlersForOOB();
        MAsmJSLoadHeap* load = MAsmJSLoadHeap::New(alloc(), accessType, ptr, needsBoundsCheck,
                                                   /* outOfBoundsLabel = */ nullptr,
                                                   /* numElems */ 0,
                                                   MembarBeforeLoad, MembarAfterLoad);
        curBlock_->add(load);
        return load;
    }

    void atomicStoreHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* v, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK && !m().usesSignalHandlersForOOB();
        MAsmJSStoreHeap* store = MAsmJSStoreHeap::New(alloc(), accessType, ptr, v, needsBoundsCheck,
                                                      /* outOfBoundsLabel = */ nullptr,
                                                      /* numElems = */ 0,
                                                      MembarBeforeStore, MembarAfterStore);
        curBlock_->add(store);
    }

    MDefinition* atomicCompareExchangeHeap(Scalar::Type accessType, MDefinition* ptr, MDefinition* oldv,
                                           MDefinition* newv, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSCompareExchangeHeap* cas =
            MAsmJSCompareExchangeHeap::New(alloc(), accessType, ptr, oldv, newv, needsBoundsCheck);
        curBlock_->add(cas);
        return cas;
    }

    MDefinition* atomicBinopHeap(js::jit::AtomicOp op, Scalar::Type accessType, MDefinition* ptr,
                                 MDefinition* v, NeedsBoundsCheck chk)
    {
        if (inDeadCode())
            return nullptr;

        bool needsBoundsCheck = chk == NEEDS_BOUNDS_CHECK;
        MAsmJSAtomicBinopHeap* binop =
            MAsmJSAtomicBinopHeap::New(alloc(), op, accessType, ptr, v, needsBoundsCheck);
        curBlock_->add(binop);
        return binop;
    }

    MDefinition* loadGlobalVar(const ModuleCompiler::Global& global)
    {
        if (inDeadCode())
            return nullptr;

        MIRType type = global.varOrConstType().toMIRType();

        unsigned globalDataOffset;
        if (IsSimdType(type))
            globalDataOffset = module().globalSimdVarIndexToGlobalDataOffset(global.varOrConstIndex());
        else
            globalDataOffset = module().globalScalarVarIndexToGlobalDataOffset(global.varOrConstIndex());

        MAsmJSLoadGlobalVar* load = MAsmJSLoadGlobalVar::New(alloc(), type, globalDataOffset,
                                                             global.isConst());
        curBlock_->add(load);
        return load;
    }

    void storeGlobalVar(const ModuleCompiler::Global& global, MDefinition* v)
    {
        if (inDeadCode())
            return;
        MOZ_ASSERT(!global.isConst());

        unsigned globalDataOffset;
        if (IsSimdType(v->type()))
            globalDataOffset = module().globalSimdVarIndexToGlobalDataOffset(global.varOrConstIndex());
        else
            globalDataOffset = module().globalScalarVarIndexToGlobalDataOffset(global.varOrConstIndex());

        curBlock_->add(MAsmJSStoreGlobalVar::New(alloc(), globalDataOffset, v));
    }

    void maybeAddInterruptCheck(ParseNode* pn)
    {
        if (inDeadCode())
            return;

        if (m().usesSignalHandlersForInterrupt())
            return;

        unsigned lineno = 0, column = 0;
        m().tokenStream().srcCoords.lineNumAndColumnIndex(pn->pn_pos.begin, &lineno, &column);
        CallSiteDesc callDesc(lineno, column, CallSiteDesc::Relative);
        curBlock_->add(MAsmJSInterruptCheck::New(alloc(), &m().syncInterruptLabel(), callDesc));
    }

    MDefinition* extractSimdElement(SimdLane lane, MDefinition* base, MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(base->type()));
        MOZ_ASSERT(!IsSimdType(type));
        MSimdExtractElement* ins = MSimdExtractElement::NewAsmJS(alloc(), base, type, lane);
        curBlock_->add(ins);
        return ins;
    }

    MDefinition* extractSignMask(MDefinition* base)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(base->type()));
        MSimdSignMask* ins = MSimdSignMask::NewAsmJS(alloc(), base);
        curBlock_->add(ins);
        return ins;
    }

    template<typename T>
    MDefinition* constructSimd(MDefinition* x, MDefinition* y, MDefinition* z, MDefinition* w,
                               MIRType type)
    {
        if (inDeadCode())
            return nullptr;

        MOZ_ASSERT(IsSimdType(type));
        T* ins = T::NewAsmJS(alloc(), type, x, y, z, w);
        curBlock_->add(ins);
        return ins;
    }

    /***************************************************************** Calls */

    // The IonMonkey backend maintains a single stack offset (from the stack
    // pointer to the base of the frame) by adding the total amount of spill
    // space required plus the maximum stack required for argument passing.
    // Since we do not use IonMonkey's MPrepareCall/MPassArg/MCall, we must
    // manually accumulate, for the entire function, the maximum required stack
    // space for argument passing. (This is passed to the CodeGenerator via
    // MIRGenerator::maxAsmJSStackArgBytes.) Naively, this would just be the
    // maximum of the stack space required for each individual call (as
    // determined by the call ABI). However, as an optimization, arguments are
    // stored to the stack immediately after evaluation (to decrease live
    // ranges and reduce spilling). This introduces the complexity that,
    // between evaluating an argument and making the call, another argument
    // evaluation could perform a call that also needs to store to the stack.
    // When this occurs childClobbers_ = true and the parent expression's
    // arguments are stored above the maximum depth clobbered by a child
    // expression.

    class Call
    {
        ParseNode* node_;
        ABIArgGenerator abi_;
        uint32_t prevMaxStackBytes_;
        uint32_t maxChildStackBytes_;
        uint32_t spIncrement_;
        Signature sig_;
        MAsmJSCall::Args regArgs_;
        Vector<MAsmJSPassStackArg*> stackArgs_;
        bool childClobbers_;

        friend class FunctionCompiler;

      public:
        Call(FunctionCompiler& f, ParseNode* callNode, RetType retType)
          : node_(callNode),
            prevMaxStackBytes_(0),
            maxChildStackBytes_(0),
            spIncrement_(0),
            sig_(f.m().lifo(), retType),
            regArgs_(f.cx()),
            stackArgs_(f.cx()),
            childClobbers_(false)
        { }
        Signature& sig() { return sig_; }
        const Signature& sig() const { return sig_; }
    };

    void startCallArgs(Call* call)
    {
        if (inDeadCode())
            return;
        call->prevMaxStackBytes_ = mirGen().resetAsmJSMaxStackArgBytes();
    }

    bool passArg(MDefinition* argDef, VarType type, Call* call)
    {
        if (!call->sig().appendArg(type))
            return false;

        if (inDeadCode())
            return true;

        uint32_t childStackBytes = mirGen().resetAsmJSMaxStackArgBytes();
        call->maxChildStackBytes_ = Max(call->maxChildStackBytes_, childStackBytes);
        if (childStackBytes > 0 && !call->stackArgs_.empty())
            call->childClobbers_ = true;

        ABIArg arg = call->abi_.next(type.toMIRType());
        if (arg.kind() == ABIArg::Stack) {
            MAsmJSPassStackArg* mir = MAsmJSPassStackArg::New(alloc(), arg.offsetFromArgBase(),
                                                              argDef);
            curBlock_->add(mir);
            if (!call->stackArgs_.append(mir))
                return false;
        } else {
            if (!call->regArgs_.append(MAsmJSCall::Arg(arg.reg(), argDef)))
                return false;
        }
        return true;
    }

    void finishCallArgs(Call* call)
    {
        if (inDeadCode())
            return;
        uint32_t parentStackBytes = call->abi_.stackBytesConsumedSoFar();
        uint32_t newStackBytes;
        if (call->childClobbers_) {
            call->spIncrement_ = AlignBytes(call->maxChildStackBytes_, AsmJSStackAlignment);
            for (unsigned i = 0; i < call->stackArgs_.length(); i++)
                call->stackArgs_[i]->incrementOffset(call->spIncrement_);
            newStackBytes = Max(call->prevMaxStackBytes_,
                                call->spIncrement_ + parentStackBytes);
        } else {
            call->spIncrement_ = 0;
            newStackBytes = Max(call->prevMaxStackBytes_,
                                Max(call->maxChildStackBytes_, parentStackBytes));
        }
        mirGen_->setAsmJSMaxStackArgBytes(newStackBytes);
    }

  private:
    bool callPrivate(MAsmJSCall::Callee callee, const Call& call, MIRType returnType, MDefinition** def)
    {
        if (inDeadCode()) {
            *def = nullptr;
            return true;
        }

        uint32_t line, column;
        m_.tokenStream().srcCoords.lineNumAndColumnIndex(call.node_->pn_pos.begin, &line, &column);

        CallSiteDesc::Kind kind = CallSiteDesc::Kind(-1);  // initialize to silence GCC warning
        switch (callee.which()) {
          case MAsmJSCall::Callee::Internal: kind = CallSiteDesc::Relative; break;
          case MAsmJSCall::Callee::Dynamic:  kind = CallSiteDesc::Register; break;
          case MAsmJSCall::Callee::Builtin:  kind = CallSiteDesc::Register; break;
        }

        MAsmJSCall* ins = MAsmJSCall::New(alloc(), CallSiteDesc(line, column, kind), callee,
                                          call.regArgs_, returnType, call.spIncrement_);
        if (!ins)
            return false;

        curBlock_->add(ins);
        *def = ins;
        return true;
    }

  public:
    bool internalCall(const ModuleCompiler::Func& func, const Call& call, MDefinition** def)
    {
        MIRType returnType = func.sig().retType().toMIRType();
        return callPrivate(MAsmJSCall::Callee(&func.entry()), call, returnType, def);
    }

    bool funcPtrCall(const ModuleCompiler::FuncPtrTable& table, MDefinition* index,
                     const Call& call, MDefinition** def)
    {
        if (inDeadCode()) {
            *def = nullptr;
            return true;
        }

        MConstant* mask = MConstant::New(alloc(), Int32Value(table.mask()));
        curBlock_->add(mask);
        MBitAnd* maskedIndex = MBitAnd::NewAsmJS(alloc(), index, mask);
        curBlock_->add(maskedIndex);
        MAsmJSLoadFuncPtr* ptrFun = MAsmJSLoadFuncPtr::New(alloc(), table.globalDataOffset(), maskedIndex);
        curBlock_->add(ptrFun);

        MIRType returnType = table.sig().retType().toMIRType();
        return callPrivate(MAsmJSCall::Callee(ptrFun), call, returnType, def);
    }

    bool ffiCall(unsigned exitIndex, const Call& call, MIRType returnType, MDefinition** def)
    {
        if (inDeadCode()) {
            *def = nullptr;
            return true;
        }

        JS_STATIC_ASSERT(offsetof(AsmJSModule::ExitDatum, exit) == 0);
        unsigned globalDataOffset = module().exitIndexToGlobalDataOffset(exitIndex);

        MAsmJSLoadFFIFunc* ptrFun = MAsmJSLoadFFIFunc::New(alloc(), globalDataOffset);
        curBlock_->add(ptrFun);

        return callPrivate(MAsmJSCall::Callee(ptrFun), call, returnType, def);
    }

    bool builtinCall(AsmJSImmKind builtin, const Call& call, MIRType returnType, MDefinition** def)
    {
        return callPrivate(MAsmJSCall::Callee(builtin), call, returnType, def);
    }

    /*********************************************** Control flow generation */

    inline bool inDeadCode() const {
        return curBlock_ == nullptr;
    }

    void returnExpr(MDefinition* expr)
    {
        if (inDeadCode())
            return;
        MAsmJSReturn* ins = MAsmJSReturn::New(alloc(), expr);
        curBlock_->end(ins);
        curBlock_ = nullptr;
    }

    void returnVoid()
    {
        if (inDeadCode())
            return;
        MAsmJSVoidReturn* ins = MAsmJSVoidReturn::New(alloc());
        curBlock_->end(ins);
        curBlock_ = nullptr;
    }

    bool branchAndStartThen(MDefinition* cond, MBasicBlock** thenBlock, MBasicBlock** elseBlock,
                            ParseNode* thenPn, ParseNode* elsePn)
    {
        if (inDeadCode())
            return true;

        bool hasThenBlock = *thenBlock != nullptr;
        bool hasElseBlock = *elseBlock != nullptr;

        if (!hasThenBlock && !newBlock(curBlock_, thenBlock, thenPn))
            return false;
        if (!hasElseBlock && !newBlock(curBlock_, elseBlock, thenPn))
            return false;

        curBlock_->end(MTest::New(alloc(), cond, *thenBlock, *elseBlock));

        // Only add as a predecessor if newBlock hasn't been called (as it does it for us)
        if (hasThenBlock && !(*thenBlock)->addPredecessor(alloc(), curBlock_))
            return false;
        if (hasElseBlock && !(*elseBlock)->addPredecessor(alloc(), curBlock_))
            return false;

        curBlock_ = *thenBlock;
        mirGraph().moveBlockToEnd(curBlock_);
        return true;
    }

    void assertCurrentBlockIs(MBasicBlock* block) {
        if (inDeadCode())
            return;
        MOZ_ASSERT(curBlock_ == block);
    }

    bool appendThenBlock(BlockVector* thenBlocks)
    {
        if (inDeadCode())
            return true;
        return thenBlocks->append(curBlock_);
    }

    bool joinIf(const BlockVector& thenBlocks, MBasicBlock* joinBlock)
    {
        if (!joinBlock)
            return true;
        MOZ_ASSERT_IF(curBlock_, thenBlocks.back() == curBlock_);
        for (size_t i = 0; i < thenBlocks.length(); i++) {
            thenBlocks[i]->end(MGoto::New(alloc(), joinBlock));
            if (!joinBlock->addPredecessor(alloc(), thenBlocks[i]))
                return false;
        }
        curBlock_ = joinBlock;
        mirGraph().moveBlockToEnd(curBlock_);
        return true;
    }

    void switchToElse(MBasicBlock* elseBlock)
    {
        if (!elseBlock)
            return;
        curBlock_ = elseBlock;
        mirGraph().moveBlockToEnd(curBlock_);
    }

    bool joinIfElse(const BlockVector& thenBlocks, ParseNode* pn)
    {
        if (inDeadCode() && thenBlocks.empty())
            return true;
        MBasicBlock* pred = curBlock_ ? curBlock_ : thenBlocks[0];
        MBasicBlock* join;
        if (!newBlock(pred, &join, pn))
            return false;
        if (curBlock_)
            curBlock_->end(MGoto::New(alloc(), join));
        for (size_t i = 0; i < thenBlocks.length(); i++) {
            thenBlocks[i]->end(MGoto::New(alloc(), join));
            if (pred == curBlock_ || i > 0) {
                if (!join->addPredecessor(alloc(), thenBlocks[i]))
                    return false;
            }
        }
        curBlock_ = join;
        return true;
    }

    void pushPhiInput(MDefinition* def)
    {
        if (inDeadCode())
            return;
        MOZ_ASSERT(curBlock_->stackDepth() == info().firstStackSlot());
        curBlock_->push(def);
    }

    MDefinition* popPhiOutput()
    {
        if (inDeadCode())
            return nullptr;
        MOZ_ASSERT(curBlock_->stackDepth() == info().firstStackSlot() + 1);
        return curBlock_->pop();
    }

    bool startPendingLoop(ParseNode* pn, MBasicBlock** loopEntry, ParseNode* bodyStmt)
    {
        if (!loopStack_.append(pn) || !breakableStack_.append(pn))
            return false;
        MOZ_ASSERT_IF(curBlock_, curBlock_->loopDepth() == loopStack_.length() - 1);
        if (inDeadCode()) {
            *loopEntry = nullptr;
            return true;
        }
        *loopEntry = MBasicBlock::NewAsmJS(mirGraph(), info(), curBlock_,
                                           MBasicBlock::PENDING_LOOP_HEADER);
        if (!*loopEntry)
            return false;
        mirGraph().addBlock(*loopEntry);
        noteBasicBlockPosition(*loopEntry, bodyStmt);
        (*loopEntry)->setLoopDepth(loopStack_.length());
        curBlock_->end(MGoto::New(alloc(), *loopEntry));
        curBlock_ = *loopEntry;
        maybeAddInterruptCheck(pn);
        return true;
    }

    bool branchAndStartLoopBody(MDefinition* cond, MBasicBlock** afterLoop, ParseNode* bodyPn, ParseNode* afterPn)
    {
        if (inDeadCode()) {
            *afterLoop = nullptr;
            return true;
        }
        MOZ_ASSERT(curBlock_->loopDepth() > 0);
        MBasicBlock* body;
        if (!newBlock(curBlock_, &body, bodyPn))
            return false;
        if (cond->isConstant() && cond->toConstant()->valueToBoolean()) {
            *afterLoop = nullptr;
            curBlock_->end(MGoto::New(alloc(), body));
        } else {
            if (!newBlockWithDepth(curBlock_, curBlock_->loopDepth() - 1, afterLoop, afterPn))
                return false;
            curBlock_->end(MTest::New(alloc(), cond, body, *afterLoop));
        }
        curBlock_ = body;
        return true;
    }

  private:
    ParseNode* popLoop()
    {
        ParseNode* pn = loopStack_.popCopy();
        MOZ_ASSERT(!unlabeledContinues_.has(pn));
        breakableStack_.popBack();
        return pn;
    }

  public:
    bool closeLoop(MBasicBlock* loopEntry, MBasicBlock* afterLoop)
    {
        ParseNode* pn = popLoop();
        if (!loopEntry) {
            MOZ_ASSERT(!afterLoop);
            MOZ_ASSERT(inDeadCode());
            MOZ_ASSERT(!unlabeledBreaks_.has(pn));
            return true;
        }
        MOZ_ASSERT(loopEntry->loopDepth() == loopStack_.length() + 1);
        MOZ_ASSERT_IF(afterLoop, afterLoop->loopDepth() == loopStack_.length());
        if (curBlock_) {
            MOZ_ASSERT(curBlock_->loopDepth() == loopStack_.length() + 1);
            curBlock_->end(MGoto::New(alloc(), loopEntry));
            if (!loopEntry->setBackedgeAsmJS(curBlock_))
                return false;
        }
        curBlock_ = afterLoop;
        if (curBlock_)
            mirGraph().moveBlockToEnd(curBlock_);
        return bindUnlabeledBreaks(pn);
    }

    bool branchAndCloseDoWhileLoop(MDefinition* cond, MBasicBlock* loopEntry, ParseNode* afterLoopStmt)
    {
        ParseNode* pn = popLoop();
        if (!loopEntry) {
            MOZ_ASSERT(inDeadCode());
            MOZ_ASSERT(!unlabeledBreaks_.has(pn));
            return true;
        }
        MOZ_ASSERT(loopEntry->loopDepth() == loopStack_.length() + 1);
        if (curBlock_) {
            MOZ_ASSERT(curBlock_->loopDepth() == loopStack_.length() + 1);
            if (cond->isConstant()) {
                if (cond->toConstant()->valueToBoolean()) {
                    curBlock_->end(MGoto::New(alloc(), loopEntry));
                    if (!loopEntry->setBackedgeAsmJS(curBlock_))
                        return false;
                    curBlock_ = nullptr;
                } else {
                    MBasicBlock* afterLoop;
                    if (!newBlock(curBlock_, &afterLoop, afterLoopStmt))
                        return false;
                    curBlock_->end(MGoto::New(alloc(), afterLoop));
                    curBlock_ = afterLoop;
                }
            } else {
                MBasicBlock* afterLoop;
                if (!newBlock(curBlock_, &afterLoop, afterLoopStmt))
                    return false;
                curBlock_->end(MTest::New(alloc(), cond, loopEntry, afterLoop));
                if (!loopEntry->setBackedgeAsmJS(curBlock_))
                    return false;
                curBlock_ = afterLoop;
            }
        }
        return bindUnlabeledBreaks(pn);
    }

    bool bindContinues(ParseNode* pn, const LabelVector* maybeLabels)
    {
        bool createdJoinBlock = false;
        if (UnlabeledBlockMap::Ptr p = unlabeledContinues_.lookup(pn)) {
            if (!bindBreaksOrContinues(&p->value(), &createdJoinBlock, pn))
                return false;
            unlabeledContinues_.remove(p);
        }
        return bindLabeledBreaksOrContinues(maybeLabels, &labeledContinues_, &createdJoinBlock, pn);
    }

    bool bindLabeledBreaks(const LabelVector* maybeLabels, ParseNode* pn)
    {
        bool createdJoinBlock = false;
        return bindLabeledBreaksOrContinues(maybeLabels, &labeledBreaks_, &createdJoinBlock, pn);
    }

    bool addBreak(PropertyName* maybeLabel) {
        if (maybeLabel)
            return addBreakOrContinue(maybeLabel, &labeledBreaks_);
        return addBreakOrContinue(breakableStack_.back(), &unlabeledBreaks_);
    }

    bool addContinue(PropertyName* maybeLabel) {
        if (maybeLabel)
            return addBreakOrContinue(maybeLabel, &labeledContinues_);
        return addBreakOrContinue(loopStack_.back(), &unlabeledContinues_);
    }

    bool startSwitch(ParseNode* pn, MDefinition* expr, int32_t low, int32_t high,
                     MBasicBlock** switchBlock)
    {
        if (!breakableStack_.append(pn))
            return false;
        if (inDeadCode()) {
            *switchBlock = nullptr;
            return true;
        }
        curBlock_->end(MTableSwitch::New(alloc(), expr, low, high));
        *switchBlock = curBlock_;
        curBlock_ = nullptr;
        return true;
    }

    bool startSwitchCase(MBasicBlock* switchBlock, MBasicBlock** next, ParseNode* pn)
    {
        if (!switchBlock) {
            *next = nullptr;
            return true;
        }
        if (!newBlock(switchBlock, next, pn))
            return false;
        if (curBlock_) {
            curBlock_->end(MGoto::New(alloc(), *next));
            if (!(*next)->addPredecessor(alloc(), curBlock_))
                return false;
        }
        curBlock_ = *next;
        return true;
    }

    bool startSwitchDefault(MBasicBlock* switchBlock, BlockVector* cases, MBasicBlock** defaultBlock, ParseNode* pn)
    {
        if (!startSwitchCase(switchBlock, defaultBlock, pn))
            return false;
        if (!*defaultBlock)
            return true;
        mirGraph().moveBlockToEnd(*defaultBlock);
        return true;
    }

    bool joinSwitch(MBasicBlock* switchBlock, const BlockVector& cases, MBasicBlock* defaultBlock)
    {
        ParseNode* pn = breakableStack_.popCopy();
        if (!switchBlock)
            return true;
        MTableSwitch* mir = switchBlock->lastIns()->toTableSwitch();
        size_t defaultIndex = mir->addDefault(defaultBlock);
        for (unsigned i = 0; i < cases.length(); i++) {
            if (!cases[i])
                mir->addCase(defaultIndex);
            else
                mir->addCase(mir->addSuccessor(cases[i]));
        }
        if (curBlock_) {
            MBasicBlock* next;
            if (!newBlock(curBlock_, &next, pn))
                return false;
            curBlock_->end(MGoto::New(alloc(), next));
            curBlock_ = next;
        }
        return bindUnlabeledBreaks(pn);
    }

    /*************************************************************************/

    MIRGenerator* extractMIR()
    {
        MOZ_ASSERT(mirGen_ != nullptr);
        MIRGenerator* mirGen = mirGen_;
        mirGen_ = nullptr;
        return mirGen;
    }

    /*************************************************************************/
  private:
    void noteBasicBlockPosition(MBasicBlock* blk, ParseNode* pn)
    {
#if defined(JS_ION_PERF) || defined(DEBUG)
        if (pn) {
            unsigned line = 0U, column = 0U;
            m().tokenStream().srcCoords.lineNumAndColumnIndex(pn->pn_pos.begin, &line, &column);
            blk->setLineno(line);
            blk->setColumnIndex(column);
        }
#endif
    }

    bool newBlockWithDepth(MBasicBlock* pred, unsigned loopDepth, MBasicBlock** block, ParseNode* pn)
    {
        *block = MBasicBlock::NewAsmJS(mirGraph(), info(), pred, MBasicBlock::NORMAL);
        if (!*block)
            return false;
        noteBasicBlockPosition(*block, pn);
        mirGraph().addBlock(*block);
        (*block)->setLoopDepth(loopDepth);
        return true;
    }

    bool newBlock(MBasicBlock* pred, MBasicBlock** block, ParseNode* pn)
    {
        return newBlockWithDepth(pred, loopStack_.length(), block, pn);
    }

    bool bindBreaksOrContinues(BlockVector* preds, bool* createdJoinBlock, ParseNode* pn)
    {
        for (unsigned i = 0; i < preds->length(); i++) {
            MBasicBlock* pred = (*preds)[i];
            if (*createdJoinBlock) {
                pred->end(MGoto::New(alloc(), curBlock_));
                if (!curBlock_->addPredecessor(alloc(), pred))
                    return false;
            } else {
                MBasicBlock* next;
                if (!newBlock(pred, &next, pn))
                    return false;
                pred->end(MGoto::New(alloc(), next));
                if (curBlock_) {
                    curBlock_->end(MGoto::New(alloc(), next));
                    if (!next->addPredecessor(alloc(), curBlock_))
                        return false;
                }
                curBlock_ = next;
                *createdJoinBlock = true;
            }
            MOZ_ASSERT(curBlock_->begin() == curBlock_->end());
            if (!mirGen_->ensureBallast())
                return false;
        }
        preds->clear();
        return true;
    }

    bool bindLabeledBreaksOrContinues(const LabelVector* maybeLabels, LabeledBlockMap* map,
                                      bool* createdJoinBlock, ParseNode* pn)
    {
        if (!maybeLabels)
            return true;
        const LabelVector& labels = *maybeLabels;
        for (unsigned i = 0; i < labels.length(); i++) {
            if (LabeledBlockMap::Ptr p = map->lookup(labels[i])) {
                if (!bindBreaksOrContinues(&p->value(), createdJoinBlock, pn))
                    return false;
                map->remove(p);
            }
            if (!mirGen_->ensureBallast())
                return false;
        }
        return true;
    }

    template <class Key, class Map>
    bool addBreakOrContinue(Key key, Map* map)
    {
        if (inDeadCode())
            return true;
        typename Map::AddPtr p = map->lookupForAdd(key);
        if (!p) {
            BlockVector empty(m().cx());
            if (!map->add(p, key, Move(empty)))
                return false;
        }
        if (!p->value().append(curBlock_))
            return false;
        curBlock_ = nullptr;
        return true;
    }

    bool bindUnlabeledBreaks(ParseNode* pn)
    {
        bool createdJoinBlock = false;
        if (UnlabeledBlockMap::Ptr p = unlabeledBreaks_.lookup(pn)) {
            if (!bindBreaksOrContinues(&p->value(), &createdJoinBlock, pn))
                return false;
            unlabeledBreaks_.remove(p);
        }
        return true;
    }
};

} /* anonymous namespace */

/*****************************************************************************/
// asm.js type-checking and code-generation algorithm

static bool
CheckIdentifier(ModuleCompiler& m, ParseNode* usepn, PropertyName* name)
{
    if (name == m.cx()->names().arguments || name == m.cx()->names().eval)
        return m.failName(usepn, "'%s' is not an allowed identifier", name);
    return true;
}

static bool
CheckModuleLevelName(ModuleCompiler& m, ParseNode* usepn, PropertyName* name)
{
    if (!CheckIdentifier(m, usepn, name))
        return false;

    if (name == m.moduleFunctionName() ||
        name == m.module().globalArgumentName() ||
        name == m.module().importArgumentName() ||
        name == m.module().bufferArgumentName() ||
        m.lookupGlobal(name))
    {
        return m.failName(usepn, "duplicate name '%s' not allowed", name);
    }

    return true;
}

static bool
CheckFunctionHead(ModuleCompiler& m, ParseNode* fn)
{
    JSFunction* fun = FunctionObject(fn);
    if (fun->hasRest())
        return m.fail(fn, "rest args not allowed");
    if (fun->isExprClosure())
        return m.fail(fn, "expression closures not allowed");
    if (fn->pn_funbox->hasDestructuringArgs)
        return m.fail(fn, "destructuring args not allowed");
    return true;
}

static bool
CheckArgument(ModuleCompiler& m, ParseNode* arg, PropertyName** name)
{
    if (!IsDefinition(arg))
        return m.fail(arg, "duplicate argument name not allowed");

    if (arg->pn_dflags & PND_DEFAULT)
        return m.fail(arg, "default arguments not allowed");

    if (!CheckIdentifier(m, arg, arg->name()))
        return false;

    *name = arg->name();
    return true;
}

static bool
CheckModuleArgument(ModuleCompiler& m, ParseNode* arg, PropertyName** name)
{
    if (!CheckArgument(m, arg, name))
        return false;

    if (!CheckModuleLevelName(m, arg, *name))
        return false;

    return true;
}

static bool
CheckModuleArguments(ModuleCompiler& m, ParseNode* fn)
{
    unsigned numFormals;
    ParseNode* arg1 = FunctionArgsList(fn, &numFormals);
    ParseNode* arg2 = arg1 ? NextNode(arg1) : nullptr;
    ParseNode* arg3 = arg2 ? NextNode(arg2) : nullptr;

    if (numFormals > 3)
        return m.fail(fn, "asm.js modules takes at most 3 argument");

    PropertyName* arg1Name = nullptr;
    if (numFormals >= 1 && !CheckModuleArgument(m, arg1, &arg1Name))
        return false;
    m.initGlobalArgumentName(arg1Name);

    PropertyName* arg2Name = nullptr;
    if (numFormals >= 2 && !CheckModuleArgument(m, arg2, &arg2Name))
        return false;
    m.initImportArgumentName(arg2Name);

    PropertyName* arg3Name = nullptr;
    if (numFormals >= 3 && !CheckModuleArgument(m, arg3, &arg3Name))
        return false;
    m.initBufferArgumentName(arg3Name);

    return true;
}

static bool
CheckPrecedingStatements(ModuleCompiler& m, ParseNode* stmtList)
{
    MOZ_ASSERT(stmtList->isKind(PNK_STATEMENTLIST));

    ParseNode* stmt = ListHead(stmtList);
    for (unsigned i = 0, n = ListLength(stmtList); i < n; i++) {
        if (!IsIgnoredDirective(m.cx(), stmt))
            return m.fail(stmt, "invalid asm.js statement");
    }

    return true;
}

static bool
CheckGlobalVariableInitConstant(ModuleCompiler& m, PropertyName* varName, ParseNode* initNode,
                                bool isConst)
{
    AsmJSNumLit literal = ExtractNumericLiteral(m, initNode);
    if (!literal.hasType())
        return m.fail(initNode, "global initializer is out of representable integer range");

    return m.addGlobalVarInit(varName, literal, isConst);
}

static bool
CheckTypeAnnotation(ModuleCompiler& m, ParseNode* coercionNode, AsmJSCoercion* coercion,
                    ParseNode** coercedExpr = nullptr)
{
    switch (coercionNode->getKind()) {
      case PNK_BITOR: {
        ParseNode* rhs = BitwiseRight(coercionNode);
        uint32_t i;
        if (!IsLiteralInt(m, rhs, &i) || i != 0)
            return m.fail(rhs, "must use |0 for argument/return coercion");
        *coercion = AsmJS_ToInt32;
        if (coercedExpr)
            *coercedExpr = BitwiseLeft(coercionNode);
        return true;
      }
      case PNK_POS: {
        *coercion = AsmJS_ToNumber;
        if (coercedExpr)
            *coercedExpr = UnaryKid(coercionNode);
        return true;
      }
      case PNK_CALL: {
        if (IsCoercionCall(m, coercionNode, coercion, coercedExpr))
            return true;
      }
      default:;
    }

    return m.fail(coercionNode, "must be of the form +x, fround(x), simdType(x) or x|0");
}

static bool
CheckGlobalVariableImportExpr(ModuleCompiler& m, PropertyName* varName, AsmJSCoercion coercion,
                              ParseNode* coercedExpr, bool isConst)
{
    if (!coercedExpr->isKind(PNK_DOT))
        return m.failName(coercedExpr, "invalid import expression for global '%s'", varName);

    ParseNode* base = DotBase(coercedExpr);
    PropertyName* field = DotMember(coercedExpr);

    PropertyName* importName = m.module().importArgumentName();
    if (!importName)
        return m.fail(coercedExpr, "cannot import without an asm.js foreign parameter");
    if (!IsUseOfName(base, importName))
        return m.failName(coercedExpr, "base of import expression must be '%s'", importName);

    return m.addGlobalVarImport(varName, field, coercion, isConst);
}

static bool
CheckGlobalVariableInitImport(ModuleCompiler& m, PropertyName* varName, ParseNode* initNode,
                              bool isConst)
{
    AsmJSCoercion coercion;
    ParseNode* coercedExpr;
    if (!CheckTypeAnnotation(m, initNode, &coercion, &coercedExpr))
        return false;
    return CheckGlobalVariableImportExpr(m, varName, coercion, coercedExpr, isConst);
}

static bool
IsArrayViewCtorName(ModuleCompiler& m, PropertyName* name, Scalar::Type* type, bool* shared)
{
    JSAtomState& names = m.cx()->names();
    *shared = false;
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
    } else if (name == names.SharedInt8Array) {
        *shared = true;
        *type = Scalar::Int8;
    } else if (name == names.SharedUint8Array) {
        *shared = true;
        *type = Scalar::Uint8;
    } else if (name == names.SharedInt16Array) {
        *shared = true;
        *type = Scalar::Int16;
    } else if (name == names.SharedUint16Array) {
        *shared = true;
        *type = Scalar::Uint16;
    } else if (name == names.SharedInt32Array) {
        *shared = true;
        *type = Scalar::Int32;
    } else if (name == names.SharedUint32Array) {
        *shared = true;
        *type = Scalar::Uint32;
    } else if (name == names.SharedFloat32Array) {
        *shared = true;
        *type = Scalar::Float32;
    } else if (name == names.SharedFloat64Array) {
        *shared = true;
        *type = Scalar::Float64;
    } else {
        return false;
    }
    return true;
}

static bool
CheckNewArrayViewArgs(ModuleCompiler& m, ParseNode* ctorExpr, PropertyName* bufferName)
{
    ParseNode* bufArg = NextNode(ctorExpr);
    if (!bufArg || NextNode(bufArg) != nullptr)
        return m.fail(ctorExpr, "array view constructor takes exactly one argument");

    if (!IsUseOfName(bufArg, bufferName))
        return m.failName(bufArg, "argument to array view constructor must be '%s'", bufferName);

    return true;
}

static bool
CheckNewArrayView(ModuleCompiler& m, PropertyName* varName, ParseNode* newExpr)
{
    PropertyName* globalName = m.module().globalArgumentName();
    if (!globalName)
        return m.fail(newExpr, "cannot create array view without an asm.js global parameter");

    PropertyName* bufferName = m.module().bufferArgumentName();
    if (!bufferName)
        return m.fail(newExpr, "cannot create array view without an asm.js heap parameter");

    ParseNode* ctorExpr = ListHead(newExpr);

    PropertyName* field;
    Scalar::Type type;
    bool shared = false;
    if (ctorExpr->isKind(PNK_DOT)) {
        ParseNode* base = DotBase(ctorExpr);

        if (!IsUseOfName(base, globalName))
            return m.failName(base, "expecting '%s.*Array", globalName);

        field = DotMember(ctorExpr);
        if (!IsArrayViewCtorName(m, field, &type, &shared))
            return m.fail(ctorExpr, "could not match typed array name");
    } else {
        if (!ctorExpr->isKind(PNK_NAME))
            return m.fail(ctorExpr, "expecting name of imported array view constructor");

        PropertyName* globalName = ctorExpr->name();
        const ModuleCompiler::Global* global = m.lookupGlobal(globalName);
        if (!global)
            return m.failName(ctorExpr, "%s not found in module global scope", globalName);

        if (global->which() != ModuleCompiler::Global::ArrayViewCtor)
            return m.failName(ctorExpr, "%s must be an imported array view constructor", globalName);

        field = nullptr;
        type = global->viewType();
    }

    if (!CheckNewArrayViewArgs(m, ctorExpr, bufferName))
        return false;

    if (!m.module().isValidViewSharedness(shared))
        return m.failName(ctorExpr, "%s has different sharedness than previous view constructors", globalName);

    return m.addArrayView(varName, type, field, shared);
}

static bool
IsSimdTypeName(ModuleCompiler& m, PropertyName* name, AsmJSSimdType* type)
{
    if (name == m.cx()->names().int32x4) {
        *type = AsmJSSimdType_int32x4;
        return true;
    }
    if (name == m.cx()->names().float32x4) {
        *type = AsmJSSimdType_float32x4;
        return true;
    }
    return false;
}

static bool
IsSimdValidOperationType(AsmJSSimdType type, AsmJSSimdOperation op)
{
    switch (op) {
#define CASE(op) case AsmJSSimdOperation_##op:
      FOREACH_COMMONX4_SIMD_OP(CASE)
        return true;
      FOREACH_INT32X4_SIMD_OP(CASE)
        return type == AsmJSSimdType_int32x4;
      FOREACH_FLOAT32X4_SIMD_OP(CASE)
        return type == AsmJSSimdType_float32x4;
#undef CASE
    }
    return false;
}

static bool
CheckGlobalMathImport(ModuleCompiler& m, ParseNode* initNode, PropertyName* varName,
                      PropertyName* field)
{
    // Math builtin, with the form glob.Math.[[builtin]]
    ModuleCompiler::MathBuiltin mathBuiltin;
    if (!m.lookupStandardLibraryMathName(field, &mathBuiltin))
        return m.failName(initNode, "'%s' is not a standard Math builtin", field);

    switch (mathBuiltin.kind) {
      case ModuleCompiler::MathBuiltin::Function:
        return m.addMathBuiltinFunction(varName, mathBuiltin.u.func, field);
      case ModuleCompiler::MathBuiltin::Constant:
        return m.addMathBuiltinConstant(varName, mathBuiltin.u.cst, field);
      default:
        break;
    }
    MOZ_CRASH("unexpected or uninitialized math builtin type");
}

static bool
CheckGlobalAtomicsImport(ModuleCompiler& m, ParseNode* initNode, PropertyName* varName,
                         PropertyName* field)
{
    // Atomics builtin, with the form glob.Atomics.[[builtin]]
    AsmJSAtomicsBuiltinFunction func;
    if (!m.lookupStandardLibraryAtomicsName(field, &func))
        return m.failName(initNode, "'%s' is not a standard Atomics builtin", field);

    return m.addAtomicsBuiltinFunction(varName, func, field);
}

static bool
CheckGlobalSimdImport(ModuleCompiler& m, ParseNode* initNode, PropertyName* varName,
                      PropertyName* field)
{
    if (!m.supportsSimd())
        return m.fail(initNode, "SIMD is not supported on this platform");

    // SIMD constructor, with the form glob.SIMD.[[type]]
    AsmJSSimdType simdType;
    if (!IsSimdTypeName(m, field, &simdType))
        return m.failName(initNode, "'%s' is not a standard SIMD type", field);
    return m.addSimdCtor(varName, simdType, field);
}

static bool
CheckGlobalSimdOperationImport(ModuleCompiler& m, const ModuleCompiler::Global* global,
                               ParseNode* initNode, PropertyName* varName, PropertyName* ctorVarName,
                               PropertyName* opName)
{
    AsmJSSimdType simdType = global->simdCtorType();
    AsmJSSimdOperation simdOp;
    if (!m.lookupStandardSimdOpName(opName, &simdOp))
        return m.failName(initNode, "'%s' is not a standard SIMD operation", opName);
    if (!IsSimdValidOperationType(simdType, simdOp))
        return m.failName(initNode, "'%s' is not an operation supported by the SIMD type", opName);
    return m.addSimdOperation(varName, simdType, simdOp, ctorVarName, opName);
}

static bool
CheckGlobalDotImport(ModuleCompiler& m, PropertyName* varName, ParseNode* initNode)
{
    ParseNode* base = DotBase(initNode);
    PropertyName* field = DotMember(initNode);

    if (base->isKind(PNK_DOT)) {
        ParseNode* global = DotBase(base);
        PropertyName* mathOrAtomicsOrSimd = DotMember(base);

        PropertyName* globalName = m.module().globalArgumentName();
        if (!globalName)
            return m.fail(base, "import statement requires the module have a stdlib parameter");

        if (!IsUseOfName(global, globalName)) {
            if (global->isKind(PNK_DOT)) {
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

    if (!base->isKind(PNK_NAME))
        return m.fail(base, "expected name of variable or parameter");

    if (base->name() == m.module().globalArgumentName()) {
        if (field == m.cx()->names().NaN)
            return m.addGlobalConstant(varName, GenericNaN(), field);
        if (field == m.cx()->names().Infinity)
            return m.addGlobalConstant(varName, PositiveInfinity<double>(), field);
        if (field == m.cx()->names().byteLength)
            return m.addByteLength(varName);

        Scalar::Type type;
        bool shared = false;
        if (IsArrayViewCtorName(m, field, &type, &shared)) {
            if (!m.module().isValidViewSharedness(shared))
                return m.failName(initNode, "'%s' has different sharedness than previous view constructors", field);
            return m.addArrayViewCtor(varName, type, field, shared);
        }

        return m.failName(initNode, "'%s' is not a standard constant or typed array name", field);
    }

    if (base->name() == m.module().importArgumentName())
        return m.addFFI(varName, field);

    const ModuleCompiler::Global* global = m.lookupGlobal(base->name());
    if (!global)
        return m.failName(initNode, "%s not found in module global scope", base->name());

    if (!global->isSimdCtor())
        return m.failName(base, "expecting SIMD constructor name, got %s", field);

    return CheckGlobalSimdOperationImport(m, global, initNode, varName, base->name(), field);
}

static bool
CheckModuleGlobal(ModuleCompiler& m, ParseNode* var, bool isConst)
{
    if (!IsDefinition(var))
        return m.fail(var, "import variable names must be unique");

    if (!CheckModuleLevelName(m, var, var->name()))
        return false;

    ParseNode* initNode = MaybeDefinitionInitializer(var);
    if (!initNode)
        return m.fail(var, "module import needs initializer");

    if (IsNumericLiteral(m, initNode))
        return CheckGlobalVariableInitConstant(m, var->name(), initNode, isConst);

    if (initNode->isKind(PNK_BITOR) || initNode->isKind(PNK_POS) || initNode->isKind(PNK_CALL))
        return CheckGlobalVariableInitImport(m, var->name(), initNode, isConst);

    if (initNode->isKind(PNK_NEW))
        return CheckNewArrayView(m, var->name(), initNode);

    if (initNode->isKind(PNK_DOT))
        return CheckGlobalDotImport(m, var->name(), initNode);

    return m.fail(initNode, "unsupported import expression");
}

static bool
CheckModuleProcessingDirectives(ModuleCompiler& m)
{
    TokenStream& ts = m.parser().tokenStream;
    while (true) {
        bool matched;
        if (!ts.matchToken(&matched, TOK_STRING))
            return false;
        if (!matched)
            return true;

        if (!IsIgnoredDirectiveName(m.cx(), ts.currentToken().atom()))
            return m.fail(nullptr, "unsupported processing directive");

        if (!ts.matchToken(&matched, TOK_SEMI))
            return false;
        if (!matched)
            return m.fail(nullptr, "expected semicolon after string literal");
    }
}

static bool
CheckModuleGlobals(ModuleCompiler& m)
{
    while (true) {
        ParseNode* varStmt;
        if (!ParseVarOrConstStatement(m.parser(), &varStmt))
            return false;
        if (!varStmt)
            break;
        for (ParseNode* var = VarListHead(varStmt); var; var = NextNode(var)) {
            if (!CheckModuleGlobal(m, var, varStmt->isKind(PNK_CONST)))
                return false;
        }
    }

    return true;
}

static bool
ArgFail(FunctionCompiler& f, PropertyName* argName, ParseNode* stmt)
{
    return f.failName(stmt, "expecting argument type declaration for '%s' of the "
                      "form 'arg = arg|0' or 'arg = +arg' or 'arg = fround(arg)'", argName);
}

static bool
CheckArgumentType(FunctionCompiler& f, ParseNode* stmt, PropertyName* name, VarType* type)
{
    if (!stmt || !IsExpressionStatement(stmt))
        return ArgFail(f, name, stmt ? stmt : f.fn());

    ParseNode* initNode = ExpressionStatementExpr(stmt);
    if (!initNode || !initNode->isKind(PNK_ASSIGN))
        return ArgFail(f, name, stmt);

    ParseNode* argNode = BinaryLeft(initNode);
    ParseNode* coercionNode = BinaryRight(initNode);

    if (!IsUseOfName(argNode, name))
        return ArgFail(f, name, stmt);

    ParseNode* coercedExpr;
    AsmJSCoercion coercion;
    if (!CheckTypeAnnotation(f.m(), coercionNode, &coercion, &coercedExpr))
        return false;

    if (!IsUseOfName(coercedExpr, name))
        return ArgFail(f, name, stmt);

    *type = VarType(coercion);
    return true;
}

static bool
CheckProcessingDirectives(ModuleCompiler& m, ParseNode** stmtIter)
{
    ParseNode* stmt = *stmtIter;

    while (stmt && IsIgnoredDirective(m.cx(), stmt))
        stmt = NextNode(stmt);

    *stmtIter = stmt;
    return true;
}

static bool
CheckArguments(FunctionCompiler& f, ParseNode** stmtIter, VarTypeVector* argTypes)
{
    ParseNode* stmt = *stmtIter;

    unsigned numFormals;
    ParseNode* argpn = FunctionArgsList(f.fn(), &numFormals);

    for (unsigned i = 0; i < numFormals; i++, argpn = NextNode(argpn), stmt = NextNode(stmt)) {
        PropertyName* name;
        if (!CheckArgument(f.m(), argpn, &name))
            return false;

        VarType type;
        if (!CheckArgumentType(f, stmt, name, &type))
            return false;

        if (!argTypes->append(type))
            return false;

        if (!f.addFormal(argpn, name, type))
            return false;
    }

    *stmtIter = stmt;
    return true;
}

static bool
IsLiteralOrConst(FunctionCompiler& f, ParseNode* pn, AsmJSNumLit* lit)
{
    if (pn->isKind(PNK_NAME)) {
        const ModuleCompiler::Global* global = f.lookupGlobal(pn->name());
        if (!global || global->which() != ModuleCompiler::Global::ConstantLiteral)
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
CheckFinalReturn(FunctionCompiler& f, ParseNode* lastNonEmptyStmt)
{
    if (!f.hasAlreadyReturned()) {
        f.setReturnedType(RetType::Void);
        f.returnVoid();
        return true;
    }

    if (!lastNonEmptyStmt->isKind(PNK_RETURN)) {
        if (f.returnedType() != RetType::Void)
            return f.fail(lastNonEmptyStmt, "void incompatible with previous return type");

        f.returnVoid();
        return true;
    }

    return true;
}

static bool
CheckVariable(FunctionCompiler& f, ParseNode* var)
{
    if (!IsDefinition(var))
        return f.fail(var, "local variable names must not restate argument names");

    PropertyName* name = var->name();

    if (!CheckIdentifier(f.m(), var, name))
        return false;

    ParseNode* initNode = MaybeDefinitionInitializer(var);
    if (!initNode)
        return f.failName(var, "var '%s' needs explicit type declaration via an initial value", name);

    AsmJSNumLit lit;
    if (!IsLiteralOrConst(f, initNode, &lit))
        return f.failName(var, "var '%s' initializer must be literal or const literal", name);

    if (!lit.hasType())
        return f.failName(var, "var '%s' initializer out of range", name);

    return f.addVariable(var, name, lit);
}

static bool
CheckVariables(FunctionCompiler& f, ParseNode** stmtIter)
{
    ParseNode* stmt = *stmtIter;

    for (; stmt && stmt->isKind(PNK_VAR); stmt = NextNonEmptyStatement(stmt)) {
        for (ParseNode* var = VarListHead(stmt); var; var = NextNode(var)) {
            if (!CheckVariable(f, var))
                return false;
        }
    }

    *stmtIter = stmt;
    return true;
}

static bool
CheckExpr(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type);

static bool
CheckNumericLiteral(FunctionCompiler& f, ParseNode* num, MDefinition** def, Type* type)
{
    AsmJSNumLit literal = ExtractNumericLiteral(f.m(), num);
    if (!literal.hasType())
        return f.fail(num, "numeric literal out of representable integer range");

    *type = Type::Of(literal);
    *def = f.constant(literal);
    return true;
}

static bool
CheckVarRef(FunctionCompiler& f, ParseNode* varRef, MDefinition** def, Type* type)
{
    PropertyName* name = varRef->name();

    if (const FunctionCompiler::Local* local = f.lookupLocal(name)) {
        *def = f.getLocalDef(*local);
        *type = local->type.toType();
        return true;
    }

    if (const ModuleCompiler::Global* global = f.lookupGlobal(name)) {
        switch (global->which()) {
          case ModuleCompiler::Global::ConstantLiteral:
            *def = f.constant(global->constLiteralValue());
            *type = global->varOrConstType();
            break;
          case ModuleCompiler::Global::ConstantImport:
          case ModuleCompiler::Global::Variable:
            *def = f.loadGlobalVar(*global);
            *type = global->varOrConstType();
            break;
          case ModuleCompiler::Global::Function:
          case ModuleCompiler::Global::FFI:
          case ModuleCompiler::Global::MathBuiltinFunction:
          case ModuleCompiler::Global::AtomicsBuiltinFunction:
          case ModuleCompiler::Global::FuncPtrTable:
          case ModuleCompiler::Global::ArrayView:
          case ModuleCompiler::Global::ArrayViewCtor:
          case ModuleCompiler::Global::SimdCtor:
          case ModuleCompiler::Global::SimdOperation:
          case ModuleCompiler::Global::ByteLength:
          case ModuleCompiler::Global::ChangeHeap:
            return f.failName(varRef, "'%s' may not be accessed by ordinary expressions", name);
        }
        return true;
    }

    return f.failName(varRef, "'%s' not found in local or asm.js module scope", name);
}

static inline bool
IsLiteralOrConstInt(FunctionCompiler& f, ParseNode* pn, uint32_t* u32)
{
    AsmJSNumLit lit;
    if (!IsLiteralOrConst(f, pn, &lit))
        return false;

    return IsLiteralInt(lit, u32);
}

static bool
FoldMaskedArrayIndex(FunctionCompiler& f, ParseNode** indexExpr, int32_t* mask,
                     NeedsBoundsCheck* needsBoundsCheck)
{
    MOZ_ASSERT((*indexExpr)->isKind(PNK_BITAND));

    ParseNode* indexNode = BitwiseLeft(*indexExpr);
    ParseNode* maskNode = BitwiseRight(*indexExpr);

    uint32_t mask2;
    if (IsLiteralOrConstInt(f, maskNode, &mask2)) {
        // Flag the access to skip the bounds check if the mask ensures that an 'out of
        // bounds' access can not occur based on the current heap length constraint.
        if (mask2 == 0) {
            *needsBoundsCheck = NO_BOUNDS_CHECK;
        } else {
            uint32_t minHeap = f.m().minHeapLength();
            uint32_t minHeapZeroes = CountLeadingZeroes32(minHeap - 1);
            uint32_t maskZeroes = CountLeadingZeroes32(mask2);
            if ((minHeapZeroes < maskZeroes) ||
                (IsPowerOfTwo(minHeap) && minHeapZeroes == maskZeroes))
            {
                *needsBoundsCheck = NO_BOUNDS_CHECK;
            }
        }
        *mask &= mask2;
        *indexExpr = indexNode;
        return true;
    }

    return false;
}

static bool
CheckArrayAccess(FunctionCompiler& f, ParseNode* viewName, ParseNode* indexExpr,
                 Scalar::Type* viewType, MDefinition** def, NeedsBoundsCheck* needsBoundsCheck)
{
    *needsBoundsCheck = NEEDS_BOUNDS_CHECK;

    if (!viewName->isKind(PNK_NAME))
        return f.fail(viewName, "base of array access must be a typed array view name");

    const ModuleCompiler::Global* global = f.lookupGlobal(viewName->name());
    if (!global || !global->isAnyArrayView())
        return f.fail(viewName, "base of array access must be a typed array view name");

    *viewType = global->viewType();

    uint32_t index;
    if (IsLiteralOrConstInt(f, indexExpr, &index)) {
        uint64_t byteOffset = uint64_t(index) << TypedArrayShift(*viewType);
        if (byteOffset > INT32_MAX)
            return f.fail(indexExpr, "constant index out of range");

        unsigned elementSize = TypedArrayElemSize(*viewType);
        if (!f.m().tryRequireHeapLengthToBeAtLeast(byteOffset + elementSize)) {
            return f.failf(indexExpr, "constant index outside heap size range declared by the "
                                      "change-heap function (0x%x - 0x%x)",
                                      f.m().minHeapLength(), f.m().module().maxHeapLength());
        }

        *needsBoundsCheck = NO_BOUNDS_CHECK;
        *def = f.constant(Int32Value(byteOffset), Type::Int);
        return true;
    }

    // Mask off the low bits to account for the clearing effect of a right shift
    // followed by the left shift implicit in the array access. E.g., H32[i>>2]
    // loses the low two bits.
    int32_t mask = ~(TypedArrayElemSize(*viewType) - 1);

    MDefinition* pointerDef;
    if (indexExpr->isKind(PNK_RSH)) {
        ParseNode* shiftAmountNode = BitwiseRight(indexExpr);

        uint32_t shift;
        if (!IsLiteralInt(f.m(), shiftAmountNode, &shift))
            return f.failf(shiftAmountNode, "shift amount must be constant");

        unsigned requiredShift = TypedArrayShift(*viewType);
        if (shift != requiredShift)
            return f.failf(shiftAmountNode, "shift amount must be %u", requiredShift);

        ParseNode* pointerNode = BitwiseLeft(indexExpr);

        if (pointerNode->isKind(PNK_BITAND))
            FoldMaskedArrayIndex(f, &pointerNode, &mask, needsBoundsCheck);

        f.enterHeapExpression();

        Type pointerType;
        if (!CheckExpr(f, pointerNode, &pointerDef, &pointerType))
            return false;

        f.leaveHeapExpression();

        if (!pointerType.isIntish())
            return f.failf(indexExpr, "%s is not a subtype of int", pointerType.toChars());
    } else {
        if (TypedArrayShift(*viewType) != 0)
            return f.fail(indexExpr, "index expression isn't shifted; must be an Int8/Uint8 access");

        MOZ_ASSERT(mask == -1);
        bool folded = false;

        if (indexExpr->isKind(PNK_BITAND))
            folded = FoldMaskedArrayIndex(f, &indexExpr, &mask, needsBoundsCheck);

        f.enterHeapExpression();

        Type pointerType;
        if (!CheckExpr(f, indexExpr, &pointerDef, &pointerType))
            return false;

        f.leaveHeapExpression();

        if (folded) {
            if (!pointerType.isIntish())
                return f.failf(indexExpr, "%s is not a subtype of intish", pointerType.toChars());
        } else {
            if (!pointerType.isInt())
                return f.failf(indexExpr, "%s is not a subtype of int", pointerType.toChars());
        }
    }

    // Don't generate the mask op if there is no need for it which could happen for
    // a shift of zero.
    if (mask == -1)
        *def = pointerDef;
    else
        *def = f.bitwise<MBitAnd>(pointerDef, f.constant(Int32Value(mask), Type::Int));

    return true;
}

static bool
CheckLoadArray(FunctionCompiler& f, ParseNode* elem, MDefinition** def, Type* type)
{
    Scalar::Type viewType;
    MDefinition* pointerDef;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckArrayAccess(f, ElemBase(elem), ElemIndex(elem), &viewType, &pointerDef, &needsBoundsCheck))
        return false;

    *def = f.loadHeap(viewType, pointerDef, needsBoundsCheck);
    *type = TypedArrayLoadType(viewType);
    return true;
}

static bool
CheckDotAccess(FunctionCompiler& f, ParseNode* elem, MDefinition** def, Type* type)
{
    MOZ_ASSERT(elem->isKind(PNK_DOT));

    ParseNode* base = DotBase(elem);
    MDefinition* baseDef;
    Type baseType;
    if (!CheckExpr(f, base, &baseDef, &baseType))
        return false;
    if (!baseType.isSimd())
        return f.failf(base, "expected SIMD type, got %s", baseType.toChars());

    ModuleCompiler& m = f.m();
    PropertyName* field = DotMember(elem);

    SimdLane lane;
    JSAtomState& names = m.cx()->names();

    if (field == names.signMask) {
        *type = Type::Signed;
        *def = f.extractSignMask(baseDef);
        return true;
    }

    if (field == names.x)
        lane = LaneX;
    else if (field == names.y)
        lane = LaneY;
    else if (field == names.z)
        lane = LaneZ;
    else if (field == names.w)
        lane = LaneW;
    else
        return f.fail(base, "dot access field must be a lane name (x, y, z, w) or signMask");

    switch (baseType.simdType()) {
      case AsmJSSimdType_int32x4:   *type = Type::Signed; break;
      case AsmJSSimdType_float32x4: *type = Type::Float;  break;
    }

    *def = f.extractSimdElement(lane, baseDef, type->toMIRType());
    return true;
}

static bool
CheckStoreArray(FunctionCompiler& f, ParseNode* lhs, ParseNode* rhs, MDefinition** def, Type* type)
{
    Scalar::Type viewType;
    MDefinition* pointerDef;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckArrayAccess(f, ElemBase(lhs), ElemIndex(lhs), &viewType, &pointerDef, &needsBoundsCheck))
        return false;

    f.enterHeapExpression();

    MDefinition* rhsDef;
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    f.leaveHeapExpression();

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
        if (rhsType.isMaybeDouble())
            rhsDef = f.unary<MToFloat32>(rhsDef);
        else if (!rhsType.isFloatish())
            return f.failf(lhs, "%s is not a subtype of double? or floatish", rhsType.toChars());
        break;
      case Scalar::Float64:
        if (rhsType.isMaybeFloat())
            rhsDef = f.unary<MToDouble>(rhsDef);
        else if (!rhsType.isMaybeDouble())
            return f.failf(lhs, "%s is not a subtype of float? or double?", rhsType.toChars());
        break;
      default:
        MOZ_CRASH("Unexpected view type");
    }

    f.storeHeap(viewType, pointerDef, rhsDef, needsBoundsCheck);

    *def = rhsDef;
    *type = rhsType;
    return true;
}

static bool
CheckAssignName(FunctionCompiler& f, ParseNode* lhs, ParseNode* rhs, MDefinition** def, Type* type)
{
    Rooted<PropertyName*> name(f.cx(), lhs->name());

    MDefinition* rhsDef;
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    if (const FunctionCompiler::Local* lhsVar = f.lookupLocal(name)) {
        if (!(rhsType <= lhsVar->type)) {
            return f.failf(lhs, "%s is not a subtype of %s",
                           rhsType.toChars(), lhsVar->type.toType().toChars());
        }
        f.assign(*lhsVar, rhsDef);
    } else if (const ModuleCompiler::Global* global = f.lookupGlobal(name)) {
        if (global->which() != ModuleCompiler::Global::Variable)
            return f.failName(lhs, "'%s' is not a mutable variable", name);
        if (!(rhsType <= global->varOrConstType())) {
            return f.failf(lhs, "%s is not a subtype of %s",
                           rhsType.toChars(), global->varOrConstType().toChars());
        }
        f.storeGlobalVar(*global, rhsDef);
    } else {
        return f.failName(lhs, "'%s' not found in local or asm.js module scope", name);
    }

    *def = rhsDef;
    *type = rhsType;
    return true;
}

static bool
CheckAssign(FunctionCompiler& f, ParseNode* assign, MDefinition** def, Type* type)
{
    MOZ_ASSERT(assign->isKind(PNK_ASSIGN));

    ParseNode* lhs = BinaryLeft(assign);
    ParseNode* rhs = BinaryRight(assign);

    if (lhs->getKind() == PNK_ELEM)
        return CheckStoreArray(f, lhs, rhs, def, type);

    if (lhs->getKind() == PNK_NAME)
        return CheckAssignName(f, lhs, rhs, def, type);

    return f.fail(assign, "left-hand side of assignment must be a variable or array access");
}

static bool
CheckMathIMul(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 2)
        return f.fail(call, "Math.imul must be passed 2 arguments");

    ParseNode* lhs = CallArgList(call);
    ParseNode* rhs = NextNode(lhs);

    MDefinition* lhsDef;
    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsDef, &lhsType))
        return false;

    MDefinition* rhsDef;
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    if (!lhsType.isIntish())
        return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
    if (!rhsType.isIntish())
        return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());

    *def = f.mul(lhsDef, rhsDef, MIRType_Int32, MMul::Integer);
    *type = Type::Signed;
    return true;
}

static bool
CheckMathClz32(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.clz32 must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    MDefinition* argDef;
    Type argType;
    if (!CheckExpr(f, arg, &argDef, &argType))
        return false;

    if (!argType.isIntish())
        return f.failf(arg, "%s is not a subtype of intish", argType.toChars());

    *def = f.unary<MClz>(argDef);
    *type = Type::Fixnum;
    return true;
}

static bool
CheckMathAbs(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.abs must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    MDefinition* argDef;
    Type argType;
    if (!CheckExpr(f, arg, &argDef, &argType))
        return false;

    if (argType.isSigned()) {
        *def = f.unary<MAbs>(argDef, MIRType_Int32);
        *type = Type::Unsigned;
        return true;
    }

    if (argType.isMaybeDouble()) {
        *def = f.unary<MAbs>(argDef, MIRType_Double);
        *type = Type::Double;
        return true;
    }

    if (argType.isMaybeFloat()) {
        *def = f.unary<MAbs>(argDef, MIRType_Float32);
        *type = Type::Floatish;
        return true;
    }

    return f.failf(call, "%s is not a subtype of signed, float? or double?", argType.toChars());
}

static bool
CheckMathSqrt(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 1)
        return f.fail(call, "Math.sqrt must be passed 1 argument");

    ParseNode* arg = CallArgList(call);

    MDefinition* argDef;
    Type argType;
    if (!CheckExpr(f, arg, &argDef, &argType))
        return false;

    if (argType.isMaybeDouble()) {
        *def = f.unary<MSqrt>(argDef, MIRType_Double);
        *type = Type::Double;
        return true;
    }

    if (argType.isMaybeFloat()) {
        *def = f.unary<MSqrt>(argDef, MIRType_Float32);
        *type = Type::Floatish;
        return true;
    }

    return f.failf(call, "%s is neither a subtype of double? nor float?", argType.toChars());
}

static bool
CheckMathMinMax(FunctionCompiler& f, ParseNode* callNode, MDefinition** def, bool isMax, Type* type)
{
    if (CallArgListLength(callNode) < 2)
        return f.fail(callNode, "Math.min/max must be passed at least 2 arguments");

    ParseNode* firstArg = CallArgList(callNode);
    MDefinition* firstDef;
    Type firstType;
    if (!CheckExpr(f, firstArg, &firstDef, &firstType))
        return false;

    if (firstType.isMaybeDouble()) {
        *type = Type::Double;
        firstType = Type::MaybeDouble;
    } else if (firstType.isMaybeFloat()) {
        *type = Type::Float;
        firstType = Type::MaybeFloat;
    } else if (firstType.isSigned()) {
        *type = Type::Signed;
        firstType = Type::Signed;
    } else {
        return f.failf(firstArg, "%s is not a subtype of double?, float? or int",
                       firstType.toChars());
    }

    MDefinition* lastDef = firstDef;
    ParseNode* nextArg = NextNode(firstArg);
    for (unsigned i = 1; i < CallArgListLength(callNode); i++, nextArg = NextNode(nextArg)) {
        MDefinition* nextDef;
        Type nextType;
        if (!CheckExpr(f, nextArg, &nextDef, &nextType))
            return false;

        if (!(nextType <= firstType))
            return f.failf(nextArg, "%s is not a subtype of %s", nextType.toChars(), firstType.toChars());

        lastDef = f.minMax(lastDef, nextDef, firstType.toMIRType(), isMax);
    }

    *def = lastDef;
    return true;
}

static bool
CheckSharedArrayAtomicAccess(FunctionCompiler& f, ParseNode* viewName, ParseNode* indexExpr,
                             Scalar::Type* viewType, MDefinition** pointerDef,
                             NeedsBoundsCheck* needsBoundsCheck)
{
    if (!CheckArrayAccess(f, viewName, indexExpr, viewType, pointerDef, needsBoundsCheck))
        return false;

    // Atomic accesses may be made on shared integer arrays only.

    // The global will be sane, CheckArrayAccess checks it.
    const ModuleCompiler::Global* global = f.lookupGlobal(viewName->name());
    if (global->which() != ModuleCompiler::Global::ArrayView || !f.m().module().isSharedView())
        return f.fail(viewName, "base of array access must be a shared typed array view name");

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

    return true;
}

static bool
CheckAtomicsFence(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 0)
        return f.fail(call, "Atomics.fence must be passed 0 arguments");

    f.memoryBarrier(MembarFull);
    *type = Type::Void;
    return true;
}

static bool
CheckAtomicsLoad(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 2)
        return f.fail(call, "Atomics.load must be passed 2 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);

    Scalar::Type viewType;
    MDefinition* pointerDef;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType, &pointerDef, &needsBoundsCheck))
        return false;

    *def = f.atomicLoadHeap(viewType, pointerDef, needsBoundsCheck);
    *type = Type::Signed;
    return true;
}

static bool
CheckAtomicsStore(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics.store must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Scalar::Type viewType;
    MDefinition* pointerDef;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType, &pointerDef, &needsBoundsCheck))
        return false;

    MDefinition* rhsDef;
    Type rhsType;
    if (!CheckExpr(f, valueArg, &rhsDef, &rhsType))
        return false;

    if (!rhsType.isIntish())
        return f.failf(arrayArg, "%s is not a subtype of intish", rhsType.toChars());

    f.atomicStoreHeap(viewType, pointerDef, rhsDef, needsBoundsCheck);

    *def = rhsDef;
    *type = Type::Signed;
    return true;
}

static bool
CheckAtomicsBinop(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type, js::jit::AtomicOp op)
{
    if (CallArgListLength(call) != 3)
        return f.fail(call, "Atomics binary operator must be passed 3 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* valueArg = NextNode(indexArg);

    Scalar::Type viewType;
    MDefinition* pointerDef;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType, &pointerDef, &needsBoundsCheck))
        return false;

    MDefinition* valueArgDef;
    Type valueArgType;
    if (!CheckExpr(f, valueArg, &valueArgDef, &valueArgType))
        return false;

    if (!valueArgType.isIntish())
        return f.failf(valueArg, "%s is not a subtype of intish", valueArgType.toChars());

    *def = f.atomicBinopHeap(op, viewType, pointerDef, valueArgDef, needsBoundsCheck);
    *type = Type::Signed;
    return true;
}

static bool
CheckAtomicsCompareExchange(FunctionCompiler& f, ParseNode* call, MDefinition** def, Type* type)
{
    if (CallArgListLength(call) != 4)
        return f.fail(call, "Atomics.compareExchange must be passed 4 arguments");

    ParseNode* arrayArg = CallArgList(call);
    ParseNode* indexArg = NextNode(arrayArg);
    ParseNode* oldValueArg = NextNode(indexArg);
    ParseNode* newValueArg = NextNode(oldValueArg);

    Scalar::Type viewType;
    MDefinition* pointerDef;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckSharedArrayAtomicAccess(f, arrayArg, indexArg, &viewType, &pointerDef, &needsBoundsCheck))
        return false;

    MDefinition* oldValueArgDef;
    Type oldValueArgType;
    if (!CheckExpr(f, oldValueArg, &oldValueArgDef, &oldValueArgType))
        return false;

    MDefinition* newValueArgDef;
    Type newValueArgType;
    if (!CheckExpr(f, newValueArg, &newValueArgDef, &newValueArgType))
        return false;

    if (!oldValueArgType.isIntish())
        return f.failf(oldValueArg, "%s is not a subtype of intish", oldValueArgType.toChars());

    if (!newValueArgType.isIntish())
        return f.failf(newValueArg, "%s is not a subtype of intish", newValueArgType.toChars());

    *def = f.atomicCompareExchangeHeap(viewType, pointerDef, oldValueArgDef, newValueArgDef,
                                       needsBoundsCheck);
    *type = Type::Signed;
    return true;
}

static bool
CheckAtomicsBuiltinCall(FunctionCompiler& f, ParseNode* callNode, AsmJSAtomicsBuiltinFunction func,
                        MDefinition** resultDef, Type* resultType)
{
    switch (func) {
      case AsmJSAtomicsBuiltin_compareExchange:
        return CheckAtomicsCompareExchange(f, callNode, resultDef, resultType);
      case AsmJSAtomicsBuiltin_load:
        return CheckAtomicsLoad(f, callNode, resultDef, resultType);
      case AsmJSAtomicsBuiltin_store:
        return CheckAtomicsStore(f, callNode, resultDef, resultType);
      case AsmJSAtomicsBuiltin_fence:
        return CheckAtomicsFence(f, callNode, resultDef, resultType);
      case AsmJSAtomicsBuiltin_add:
        return CheckAtomicsBinop(f, callNode, resultDef, resultType, AtomicFetchAddOp);
      case AsmJSAtomicsBuiltin_sub:
        return CheckAtomicsBinop(f, callNode, resultDef, resultType, AtomicFetchSubOp);
      case AsmJSAtomicsBuiltin_and:
        return CheckAtomicsBinop(f, callNode, resultDef, resultType, AtomicFetchAndOp);
      case AsmJSAtomicsBuiltin_or:
        return CheckAtomicsBinop(f, callNode, resultDef, resultType, AtomicFetchOrOp);
      case AsmJSAtomicsBuiltin_xor:
        return CheckAtomicsBinop(f, callNode, resultDef, resultType, AtomicFetchXorOp);
      default:
        MOZ_CRASH("unexpected atomicsBuiltin function");
    }
}

typedef bool (*CheckArgType)(FunctionCompiler& f, ParseNode* argNode, Type type);

static bool
CheckCallArgs(FunctionCompiler& f, ParseNode* callNode, CheckArgType checkArg,
              FunctionCompiler::Call* call)
{
    f.startCallArgs(call);

    ParseNode* argNode = CallArgList(callNode);
    for (unsigned i = 0; i < CallArgListLength(callNode); i++, argNode = NextNode(argNode)) {
        MDefinition* def;
        Type type;
        if (!CheckExpr(f, argNode, &def, &type))
            return false;

        if (!checkArg(f, argNode, type))
            return false;

        if (!f.passArg(def, VarType::FromCheckedType(type), call))
            return false;
    }

    f.finishCallArgs(call);
    return true;
}

static bool
CheckSignatureAgainstExisting(ModuleCompiler& m, ParseNode* usepn, const Signature& sig,
                              const Signature& existing)
{
    if (sig.args().length() != existing.args().length()) {
        return m.failf(usepn, "incompatible number of arguments (%u here vs. %u before)",
                       sig.args().length(), existing.args().length());
    }

    for (unsigned i = 0; i < sig.args().length(); i++) {
        if (sig.arg(i) != existing.arg(i)) {
            return m.failf(usepn, "incompatible type for argument %u: (%s here vs. %s before)",
                           i, sig.arg(i).toType().toChars(), existing.arg(i).toType().toChars());
        }
    }

    if (sig.retType() != existing.retType()) {
        return m.failf(usepn, "%s incompatible with previous return of type %s",
                       sig.retType().toType().toChars(), existing.retType().toType().toChars());
    }

    MOZ_ASSERT(sig == existing);
    return true;
}

static bool
CheckFunctionSignature(ModuleCompiler& m, ParseNode* usepn, Signature&& sig, PropertyName* name,
                       ModuleCompiler::Func** func)
{
    ModuleCompiler::Func* existing = m.lookupFunction(name);
    if (!existing) {
        if (!CheckModuleLevelName(m, usepn, name))
            return false;
        return m.addFunction(name, Move(sig), func);
    }

    if (!CheckSignatureAgainstExisting(m, usepn, sig, existing->sig()))
        return false;

    *func = existing;
    return true;
}

static bool
CheckIsVarType(FunctionCompiler& f, ParseNode* argNode, Type type)
{
    if (!type.isVarType())
        return f.failf(argNode, "%s is not a subtype of int, float or double", type.toChars());
    return true;
}

static bool
CheckInternalCall(FunctionCompiler& f, ParseNode* callNode, PropertyName* calleeName,
                  RetType retType, MDefinition** def, Type* type)
{
    FunctionCompiler::Call call(f, callNode, retType);

    if (!CheckCallArgs(f, callNode, CheckIsVarType, &call))
        return false;

    ModuleCompiler::Func* callee;
    if (!CheckFunctionSignature(f.m(), callNode, Move(call.sig()), calleeName, &callee))
        return false;

    if (!f.internalCall(*callee, call, def))
        return false;

    *type = retType.toType();
    return true;
}

static bool
CheckFuncPtrTableAgainstExisting(ModuleCompiler& m, ParseNode* usepn,
                                 PropertyName* name, Signature&& sig, unsigned mask,
                                 ModuleCompiler::FuncPtrTable** tableOut)
{
    if (const ModuleCompiler::Global* existing = m.lookupGlobal(name)) {
        if (existing->which() != ModuleCompiler::Global::FuncPtrTable)
            return m.failName(usepn, "'%s' is not a function-pointer table", name);

        ModuleCompiler::FuncPtrTable& table = m.funcPtrTable(existing->funcPtrTableIndex());
        if (mask != table.mask())
            return m.failf(usepn, "mask does not match previous value (%u)", table.mask());

        if (!CheckSignatureAgainstExisting(m, usepn, sig, table.sig()))
            return false;

        *tableOut = &table;
        return true;
    }

    if (!CheckModuleLevelName(m, usepn, name))
        return false;

    return m.addFuncPtrTable(name, Move(sig), mask, tableOut);
}

static bool
CheckFuncPtrCall(FunctionCompiler& f, ParseNode* callNode, RetType retType, MDefinition** def, Type* type)
{
    ParseNode* callee = CallCallee(callNode);
    ParseNode* tableNode = ElemBase(callee);
    ParseNode* indexExpr = ElemIndex(callee);

    if (!tableNode->isKind(PNK_NAME))
        return f.fail(tableNode, "expecting name of function-pointer array");

    PropertyName* name = tableNode->name();
    if (const ModuleCompiler::Global* existing = f.lookupGlobal(name)) {
        if (existing->which() != ModuleCompiler::Global::FuncPtrTable)
            return f.failName(tableNode, "'%s' is not the name of a function-pointer array", name);
    }

    if (!indexExpr->isKind(PNK_BITAND))
        return f.fail(indexExpr, "function-pointer table index expression needs & mask");

    ParseNode* indexNode = BitwiseLeft(indexExpr);
    ParseNode* maskNode = BitwiseRight(indexExpr);

    uint32_t mask;
    if (!IsLiteralInt(f.m(), maskNode, &mask) || mask == UINT32_MAX || !IsPowerOfTwo(mask + 1))
        return f.fail(maskNode, "function-pointer table index mask value must be a power of two minus 1");

    MDefinition* indexDef;
    Type indexType;
    if (!CheckExpr(f, indexNode, &indexDef, &indexType))
        return false;

    if (!indexType.isIntish())
        return f.failf(indexNode, "%s is not a subtype of intish", indexType.toChars());

    FunctionCompiler::Call call(f, callNode, retType);

    if (!CheckCallArgs(f, callNode, CheckIsVarType, &call))
        return false;

    ModuleCompiler::FuncPtrTable* table;
    if (!CheckFuncPtrTableAgainstExisting(f.m(), tableNode, name, Move(call.sig()), mask, &table))
        return false;

    if (!f.funcPtrCall(*table, indexDef, call, def))
        return false;

    *type = retType.toType();
    return true;
}

static bool
CheckIsExternType(FunctionCompiler& f, ParseNode* argNode, Type type)
{
    if (!type.isExtern())
        return f.failf(argNode, "%s is not a subtype of extern", type.toChars());
    return true;
}

static bool
CheckFFICall(FunctionCompiler& f, ParseNode* callNode, unsigned ffiIndex, RetType retType,
             MDefinition** def, Type* type)
{
    PropertyName* calleeName = CallCallee(callNode)->name();

    if (retType == RetType::Float)
        return f.fail(callNode, "FFI calls can't return float");
    if (retType.toType().isSimd())
        return f.fail(callNode, "FFI calls can't return SIMD values");

    FunctionCompiler::Call call(f, callNode, retType);
    if (!CheckCallArgs(f, callNode, CheckIsExternType, &call))
        return false;

    unsigned exitIndex;
    if (!f.m().addExit(ffiIndex, calleeName, Move(call.sig()), &exitIndex))
        return false;

    if (!f.ffiCall(exitIndex, call, retType.toMIRType(), def))
        return false;

    *type = retType.toType();
    return true;
}

static bool
CheckFloatCoercionArg(FunctionCompiler& f, ParseNode* inputNode, Type inputType,
                      MDefinition* inputDef, MDefinition** def)
{
    if (inputType.isMaybeDouble() || inputType.isSigned()) {
        *def = f.unary<MToFloat32>(inputDef);
        return true;
    }
    if (inputType.isUnsigned()) {
        *def = f.unary<MAsmJSUnsignedToFloat32>(inputDef);
        return true;
    }
    if (inputType.isFloatish()) {
        *def = inputDef;
        return true;
    }

    return f.failf(inputNode, "%s is not a subtype of signed, unsigned, double? or floatish",
                   inputType.toChars());
}

static bool
CheckCoercedCall(FunctionCompiler& f, ParseNode* call, RetType retType, MDefinition** def, Type* type);

static bool
CheckCoercionArg(FunctionCompiler& f, ParseNode* arg, AsmJSCoercion expected, MDefinition** def,
                 Type* type)
{
    RetType retType(expected);
    if (arg->isKind(PNK_CALL))
        return CheckCoercedCall(f, arg, retType, def, type);

    MDefinition* argDef;
    Type argType;
    if (!CheckExpr(f, arg, &argDef, &argType))
        return false;

    switch (expected) {
      case AsmJS_FRound:
        if (!CheckFloatCoercionArg(f, arg, argType, argDef, def))
            return false;
        break;
      case AsmJS_ToInt32x4:
        if (!argType.isInt32x4())
            return f.fail(arg, "argument to SIMD int32x4 coercion isn't int32x4");
        *def = argDef;
        break;
      case AsmJS_ToFloat32x4:
        if (!argType.isFloat32x4())
            return f.fail(arg, "argument to SIMD float32x4 coercion isn't float32x4");
        *def = argDef;
        break;
      case AsmJS_ToInt32:
      case AsmJS_ToNumber:
        MOZ_CRASH("not call coercions");
    }

    *type = retType.toType();
    return true;
}

static bool
CheckMathFRound(FunctionCompiler& f, ParseNode* callNode, MDefinition** def, Type* type)
{
    if (CallArgListLength(callNode) != 1)
        return f.fail(callNode, "Math.fround must be passed 1 argument");

    ParseNode* argNode = CallArgList(callNode);
    MDefinition* argDef;
    Type argType;
    if (!CheckCoercionArg(f, argNode, AsmJS_FRound, &argDef, &argType))
        return false;

    MOZ_ASSERT(argType == Type::Float);
    *def = argDef;
    *type = Type::Float;
    return true;
}

static bool
CheckMathBuiltinCall(FunctionCompiler& f, ParseNode* callNode, AsmJSMathBuiltinFunction func,
                     MDefinition** def, Type* type)
{
    unsigned arity = 0;
    AsmJSImmKind doubleCallee, floatCallee;
    switch (func) {
      case AsmJSMathBuiltin_imul:   return CheckMathIMul(f, callNode, def, type);
      case AsmJSMathBuiltin_clz32:  return CheckMathClz32(f, callNode, def, type);
      case AsmJSMathBuiltin_abs:    return CheckMathAbs(f, callNode, def, type);
      case AsmJSMathBuiltin_sqrt:   return CheckMathSqrt(f, callNode, def, type);
      case AsmJSMathBuiltin_fround: return CheckMathFRound(f, callNode, def, type);
      case AsmJSMathBuiltin_min:    return CheckMathMinMax(f, callNode, def, /* isMax = */ false, type);
      case AsmJSMathBuiltin_max:    return CheckMathMinMax(f, callNode, def, /* isMax = */ true, type);
      case AsmJSMathBuiltin_ceil:   arity = 1; doubleCallee = AsmJSImm_CeilD;  floatCallee = AsmJSImm_CeilF;   break;
      case AsmJSMathBuiltin_floor:  arity = 1; doubleCallee = AsmJSImm_FloorD; floatCallee = AsmJSImm_FloorF;  break;
      case AsmJSMathBuiltin_sin:    arity = 1; doubleCallee = AsmJSImm_SinD;   floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_cos:    arity = 1; doubleCallee = AsmJSImm_CosD;   floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_tan:    arity = 1; doubleCallee = AsmJSImm_TanD;   floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_asin:   arity = 1; doubleCallee = AsmJSImm_ASinD;  floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_acos:   arity = 1; doubleCallee = AsmJSImm_ACosD;  floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_atan:   arity = 1; doubleCallee = AsmJSImm_ATanD;  floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_exp:    arity = 1; doubleCallee = AsmJSImm_ExpD;   floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_log:    arity = 1; doubleCallee = AsmJSImm_LogD;   floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_pow:    arity = 2; doubleCallee = AsmJSImm_PowD;   floatCallee = AsmJSImm_Limit; break;
      case AsmJSMathBuiltin_atan2:  arity = 2; doubleCallee = AsmJSImm_ATan2D; floatCallee = AsmJSImm_Limit; break;
      default: MOZ_CRASH("unexpected mathBuiltin function");
    }

    unsigned actualArity = CallArgListLength(callNode);
    if (actualArity != arity)
        return f.failf(callNode, "call passed %u arguments, expected %u", actualArity, arity);

    Type firstType;
    MDefinition* firstArg;
    ParseNode* argNode = CallArgList(callNode);
    if (!CheckExpr(f, argNode, &firstArg, &firstType))
        return false;

    if (!firstType.isMaybeFloat() && !firstType.isMaybeDouble())
        return f.fail(argNode, "arguments to math call should be a subtype of double? or float?");

    bool opIsDouble = firstType.isMaybeDouble();
    if (!opIsDouble && floatCallee == AsmJSImm_Limit)
        return f.fail(callNode, "math builtin cannot be used as float");

    FunctionCompiler::Call call(f, callNode, RetType::Double);
    f.startCallArgs(&call);

    VarType varType = opIsDouble ? VarType::Double : VarType::Float;
    if (!f.passArg(firstArg, varType, &call))
        return false;

    if (arity == 2) {
        Type secondType;
        MDefinition* secondArg;
        argNode = NextNode(argNode);
        if (!CheckExpr(f, argNode, &secondArg, &secondType))
            return false;

        if (firstType.isMaybeDouble() && !secondType.isMaybeDouble())
            return f.fail(argNode, "both arguments to math builtin call should be the same type");
        if (firstType.isMaybeFloat() && !secondType.isMaybeFloat())
            return f.fail(argNode, "both arguments to math builtin call should be the same type");

        if (!f.passArg(secondArg, varType, &call))
            return false;
    }

    f.finishCallArgs(&call);

    AsmJSImmKind callee = opIsDouble ? doubleCallee : floatCallee;
    if (!f.builtinCall(callee, call, varType.toMIRType(), def))
        return false;

    *type = opIsDouble ? Type::Double : Type::Floatish;
    return true;
}

typedef Vector<MDefinition*, 4, SystemAllocPolicy> DefinitionVector;

namespace {
// Include CheckSimdCallArgs in unnamed namespace to avoid MSVC name lookup bug.

template<class CheckArgOp>
static bool
CheckSimdCallArgs(FunctionCompiler& f, ParseNode* call, unsigned expectedArity,
                  const CheckArgOp& checkArg, DefinitionVector* defs)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != expectedArity)
        return f.failf(call, "expected %u arguments to SIMD call, got %u", expectedArity, numArgs);

    DefinitionVector& argDefs = *defs;
    if (!argDefs.resize(numArgs))
        return false;

    ParseNode* arg = CallArgList(call);
    for (size_t i = 0; i < numArgs; i++, arg = NextNode(arg)) {
        MOZ_ASSERT(!!arg);

        Type argType;
        if (!CheckExpr(f, arg, &argDefs[i], &argType))
            return false;
        if (!checkArg(f, arg, i, argType, &argDefs[i]))
            return false;
    }

    return true;
}

class CheckArgIsSubtypeOf
{
    Type formalType_;

  public:
    explicit CheckArgIsSubtypeOf(AsmJSSimdType t) : formalType_(t) {}

    bool operator()(FunctionCompiler& f, ParseNode* arg, unsigned argIndex, Type actualType,
                    MDefinition** def) const
    {
        if (!(actualType <= formalType_)) {
            return f.failf(arg, "%s is not a subtype of %s", actualType.toChars(),
                           formalType_.toChars());
        }
        return true;
    }
};

static inline Type
SimdToCoercedScalarType(AsmJSSimdType t)
{
    switch (t) {
      case AsmJSSimdType_int32x4:
        return Type::Intish;
      case AsmJSSimdType_float32x4:
        return Type::Floatish;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected SIMD type");
}

class CheckSimdScalarArgs
{
    AsmJSSimdType simdType_;
    Type formalType_;

  public:
    explicit CheckSimdScalarArgs(AsmJSSimdType simdType)
      : simdType_(simdType), formalType_(SimdToCoercedScalarType(simdType))
    {}

    bool operator()(FunctionCompiler& f, ParseNode* arg, unsigned argIndex, Type actualType,
                    MDefinition** def) const
    {
        if (!(actualType <= formalType_)) {
            // As a special case, accept doublelit arguments to float32x4 ops by
            // re-emitting them as float32 constants.
            if (simdType_ != AsmJSSimdType_float32x4 || !actualType.isDoubleLit()) {
                return f.failf(arg, "%s is not a subtype of %s%s",
                               actualType.toChars(), formalType_.toChars(),
                               simdType_ == AsmJSSimdType_float32x4 ? " or doublelit" : "");
            }

            AsmJSNumLit doubleLit = ExtractNumericLiteral(f.m(), arg);
            MOZ_ASSERT(doubleLit.which() == AsmJSNumLit::Double);
            *def = f.constant(doubleLit.scalarValue(), Type::Float);
        }
        return true;
    }
};

class CheckSimdSelectArgs
{
    Type formalType_;

  public:
    explicit CheckSimdSelectArgs(AsmJSSimdType t) : formalType_(t) {}

    bool operator()(FunctionCompiler& f, ParseNode* arg, unsigned argIndex, Type actualType,
                    MDefinition** def) const
    {
        if (argIndex == 0) {
            // First argument of select is an int32x4 mask.
            if (!(actualType <= Type::Int32x4))
                return f.failf(arg, "%s is not a subtype of Int32x4", actualType.toChars());
            return true;
        }

        if (!(actualType <= formalType_)) {
            return f.failf(arg, "%s is not a subtype of %s", actualType.toChars(),
                           formalType_.toChars());
        }
        return true;
    }
};

class CheckSimdVectorScalarArgs
{
    AsmJSSimdType formalSimdType_;

  public:
    explicit CheckSimdVectorScalarArgs(AsmJSSimdType t) : formalSimdType_(t) {}

    bool operator()(FunctionCompiler& f, ParseNode* arg, unsigned argIndex, Type actualType,
                    MDefinition** def) const
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
        return CheckSimdScalarArgs(formalSimdType_)(f, arg, argIndex, actualType, def);
    }
};

} // anonymous namespace

static inline bool
CheckSimdUnary(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType,
               MSimdUnaryArith::Operation op, MDefinition** def, Type* type)
{
    DefinitionVector defs;
    if (!CheckSimdCallArgs(f, call, 1, CheckArgIsSubtypeOf(opType), &defs))
        return false;
    *type = opType;
    *def = f.unarySimd(defs[0], op, type->toMIRType());
    return true;
}

template<class OpEnum>
static inline bool
CheckSimdBinary(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType, OpEnum op,
                MDefinition** def, Type* type)
{
    DefinitionVector argDefs;
    if (!CheckSimdCallArgs(f, call, 2, CheckArgIsSubtypeOf(opType), &argDefs))
        return false;
    *type = opType;
    *def = f.binarySimd(argDefs[0], argDefs[1], op, type->toMIRType());
    return true;
}

template<>
inline bool
CheckSimdBinary<MSimdBinaryComp::Operation>(FunctionCompiler& f, ParseNode* call,
                                            AsmJSSimdType opType, MSimdBinaryComp::Operation op,
                                            MDefinition** def, Type* type)
{
    DefinitionVector argDefs;
    if (!CheckSimdCallArgs(f, call, 2, CheckArgIsSubtypeOf(opType), &argDefs))
        return false;
    *type = Type::Int32x4;
    *def = f.binarySimd<MSimdBinaryComp>(argDefs[0], argDefs[1], op);
    return true;
}

template<>
inline bool
CheckSimdBinary<MSimdShift::Operation>(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType,
                                       MSimdShift::Operation op, MDefinition** def, Type* type)
{
    DefinitionVector argDefs;
    if (!CheckSimdCallArgs(f, call, 2, CheckSimdVectorScalarArgs(opType), &argDefs))
        return false;
    *type = Type::Int32x4;
    *def = f.binarySimd<MSimdShift>(argDefs[0], argDefs[1], op);
    return true;
}

static bool
CheckSimdWith(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType, SimdLane lane,
              MDefinition** def, Type* type)
{
    DefinitionVector defs;
    if (!CheckSimdCallArgs(f, call, 2, CheckSimdVectorScalarArgs(opType), &defs))
        return false;
    *type = opType;
    *def = f.insertElementSimd(defs[0], defs[1], lane, type->toMIRType());
    return true;
}

namespace {
// Include CheckSimdCast in unnamed namespace to avoid MSVC name lookup bug (due to the use of Type).

template<class T>
static bool
CheckSimdCast(FunctionCompiler& f, ParseNode* call, AsmJSSimdType fromType, AsmJSSimdType toType,
              MDefinition** def, Type* type)
{
    DefinitionVector defs;
    if (!CheckSimdCallArgs(f, call, 1, CheckArgIsSubtypeOf(fromType), &defs))
        return false;
    *type = toType;
    *def = f.convertSimd<T>(defs[0], Type(fromType).toMIRType(), type->toMIRType());
    return true;
}

}

static bool
CheckSimdShuffleSelectors(FunctionCompiler& f, ParseNode* lane, int32_t lanes[4], uint32_t maxLane)
{
    for (unsigned i = 0; i < 4; i++, lane = NextNode(lane)) {
        uint32_t u32;
        if (!IsLiteralInt(f.m(), lane, &u32))
            return f.failf(lane, "lane selector should be a constant integer literal");
        if (u32 >= maxLane)
            return f.failf(lane, "lane selector should be less than %u", maxLane);
        lanes[i] = int32_t(u32);
    }
    return true;
}

static bool
CheckSimdSwizzle(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType, MDefinition** def,
                 Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 5)
        return f.failf(call, "expected 5 arguments to SIMD swizzle, got %u", numArgs);

    Type retType = opType;
    ParseNode* vec = CallArgList(call);
    MDefinition* vecDef;
    Type vecType;
    if (!CheckExpr(f, vec, &vecDef, &vecType))
        return false;
    if (!(vecType <= retType))
        return f.failf(vec, "%s is not a subtype of %s", vecType.toChars(), retType.toChars());

    int32_t lanes[4];
    if (!CheckSimdShuffleSelectors(f, NextNode(vec), lanes, 4))
        return false;

    *def = f.swizzleSimd(vecDef, lanes[0], lanes[1], lanes[2], lanes[3], retType.toMIRType());
    *type = retType;
    return true;
}

static bool
CheckSimdShuffle(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType, MDefinition** def,
                 Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 6)
        return f.failf(call, "expected 6 arguments to SIMD shuffle, got %u", numArgs);

    Type retType = opType;
    ParseNode* arg = CallArgList(call);
    MDefinition* vecs[2];
    for (unsigned i = 0; i < 2; i++, arg = NextNode(arg)) {
        Type type;
        if (!CheckExpr(f, arg, &vecs[i], &type))
            return false;
        if (!(type <= retType))
            return f.failf(arg, "%s is not a subtype of %s", type.toChars(), retType.toChars());
    }

    int32_t lanes[4];
    if (!CheckSimdShuffleSelectors(f, arg, lanes, 8))
        return false;

    *def = f.shuffleSimd(vecs[0], vecs[1], lanes[0], lanes[1], lanes[2], lanes[3],
                         retType.toMIRType());
    *type = retType;
    return true;
}

static bool
CheckSimdLoadStoreArgs(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType,
                       unsigned numElems, Scalar::Type* viewType, MDefinition** index,
                       NeedsBoundsCheck* needsBoundsCheck)
{
    ParseNode* view = CallArgList(call);
    if (!view->isKind(PNK_NAME))
        return f.fail(view, "expected Uint8Array view as SIMD.*.load/store first argument");

    const ModuleCompiler::Global* global = f.lookupGlobal(view->name());
    if (!global ||
        global->which() != ModuleCompiler::Global::ArrayView ||
        global->viewType() != Scalar::Uint8)
    {
        return f.fail(view, "expected Uint8Array view as SIMD.*.load/store first argument");
    }

    *needsBoundsCheck = NEEDS_BOUNDS_CHECK;

    switch (opType) {
      case AsmJSSimdType_int32x4:   *viewType = Scalar::Int32x4;   break;
      case AsmJSSimdType_float32x4: *viewType = Scalar::Float32x4; break;
    }

    ParseNode* indexExpr = NextNode(view);
    uint32_t indexLit;
    if (IsLiteralOrConstInt(f, indexExpr, &indexLit)) {
        if (indexLit > INT32_MAX)
            return f.fail(indexExpr, "constant index out of range");

        if (!f.m().tryRequireHeapLengthToBeAtLeast(indexLit + Simd128DataSize)) {
            return f.failf(indexExpr, "constant index outside heap size range declared by the "
                                      "change-heap function (0x%x - 0x%x)",
                                      f.m().minHeapLength(), f.m().module().maxHeapLength());
        }

        *needsBoundsCheck = NO_BOUNDS_CHECK;
        *index = f.constant(Int32Value(indexLit), Type::Int);
        return true;
    }

    f.enterHeapExpression();

    Type indexType;
    if (!CheckExpr(f, indexExpr, index, &indexType))
        return false;
    if (!indexType.isIntish())
        return f.failf(indexExpr, "%s is not a subtype of intish", indexType.toChars());

    f.leaveHeapExpression();

    return true;
}

static bool
CheckSimdLoad(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType,
              unsigned numElems, MDefinition** def, Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 2)
        return f.failf(call, "expected 2 arguments to SIMD load, got %u", numArgs);

    Scalar::Type viewType;
    MDefinition* index;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckSimdLoadStoreArgs(f, call, opType, numElems, &viewType, &index, &needsBoundsCheck))
        return false;

    *def = f.loadSimdHeap(viewType, index, needsBoundsCheck, numElems);
    *type = opType;
    return true;
}

static bool
CheckSimdStore(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType,
               unsigned numElems, MDefinition** def, Type* type)
{
    unsigned numArgs = CallArgListLength(call);
    if (numArgs != 3)
        return f.failf(call, "expected 3 arguments to SIMD store, got %u", numArgs);

    Scalar::Type viewType;
    MDefinition* index;
    NeedsBoundsCheck needsBoundsCheck;
    if (!CheckSimdLoadStoreArgs(f, call, opType, numElems, &viewType, &index, &needsBoundsCheck))
        return false;

    Type retType = opType;
    ParseNode* vecExpr = NextNode(NextNode(CallArgList(call)));
    MDefinition* vec;
    Type vecType;
    if (!CheckExpr(f, vecExpr, &vec, &vecType))
        return false;
    if (!(vecType <= retType))
        return f.failf(vecExpr, "%s is not a subtype of %s", vecType.toChars(), retType.toChars());

    f.storeSimdHeap(viewType, index, vec, needsBoundsCheck, numElems);
    *def = vec;
    *type = vecType;
    return true;
}

static bool
CheckSimdSelect(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType, bool isElementWise,
                MDefinition** def, Type* type)
{
    DefinitionVector defs;
    if (!CheckSimdCallArgs(f, call, 3, CheckSimdSelectArgs(opType), &defs))
        return false;
    *type = opType;
    *def = f.selectSimd(defs[0], defs[1], defs[2], type->toMIRType(), isElementWise);
    return true;
}

static bool
CheckSimdCheck(FunctionCompiler& f, ParseNode* call, AsmJSSimdType opType, MDefinition** def,
               Type* type)
{
    AsmJSCoercion coercion;
    ParseNode* argNode;
    if (!IsCoercionCall(f.m(), call, &coercion, &argNode))
        return f.failf(call, "expected 1 argument in call to check");
    return CheckCoercionArg(f, argNode, coercion, def, type);
}

static bool
CheckSimdOperationCall(FunctionCompiler& f, ParseNode* call, const ModuleCompiler::Global* global,
                       MDefinition** def, Type* type)
{
    MOZ_ASSERT(global->isSimdOperation());

    AsmJSSimdType opType = global->simdOperationType();

    switch (global->simdOperation()) {
      case AsmJSSimdOperation_check:
        return CheckSimdCheck(f, call, opType, def, type);

#define OP_CHECK_CASE_LIST_(OP)                                                         \
      case AsmJSSimdOperation_##OP:                                                     \
        return CheckSimdBinary(f, call, opType, MSimdBinaryArith::Op_##OP, def, type);
      ARITH_COMMONX4_SIMD_OP(OP_CHECK_CASE_LIST_)
      ARITH_FLOAT32X4_SIMD_OP(OP_CHECK_CASE_LIST_)
#undef OP_CHECK_CASE_LIST_

      case AsmJSSimdOperation_lessThan:
        return CheckSimdBinary(f, call, opType, MSimdBinaryComp::lessThan, def, type);
      case AsmJSSimdOperation_lessThanOrEqual:
        return CheckSimdBinary(f, call, opType, MSimdBinaryComp::lessThanOrEqual, def, type);
      case AsmJSSimdOperation_equal:
        return CheckSimdBinary(f, call, opType, MSimdBinaryComp::equal, def, type);
      case AsmJSSimdOperation_notEqual:
        return CheckSimdBinary(f, call, opType, MSimdBinaryComp::notEqual, def, type);
      case AsmJSSimdOperation_greaterThan:
        return CheckSimdBinary(f, call, opType, MSimdBinaryComp::greaterThan, def, type);
      case AsmJSSimdOperation_greaterThanOrEqual:
        return CheckSimdBinary(f, call, opType, MSimdBinaryComp::greaterThanOrEqual, def, type);

      case AsmJSSimdOperation_and:
        return CheckSimdBinary(f, call, opType, MSimdBinaryBitwise::and_, def, type);
      case AsmJSSimdOperation_or:
        return CheckSimdBinary(f, call, opType, MSimdBinaryBitwise::or_, def, type);
      case AsmJSSimdOperation_xor:
        return CheckSimdBinary(f, call, opType, MSimdBinaryBitwise::xor_, def, type);

      case AsmJSSimdOperation_withX:
        return CheckSimdWith(f, call, opType, SimdLane::LaneX, def, type);
      case AsmJSSimdOperation_withY:
        return CheckSimdWith(f, call, opType, SimdLane::LaneY, def, type);
      case AsmJSSimdOperation_withZ:
        return CheckSimdWith(f, call, opType, SimdLane::LaneZ, def, type);
      case AsmJSSimdOperation_withW:
        return CheckSimdWith(f, call, opType, SimdLane::LaneW, def, type);

      case AsmJSSimdOperation_fromInt32x4:
        return CheckSimdCast<MSimdConvert>(f, call, AsmJSSimdType_int32x4, opType, def, type);
      case AsmJSSimdOperation_fromInt32x4Bits:
        return CheckSimdCast<MSimdReinterpretCast>(f, call, AsmJSSimdType_int32x4, opType, def, type);
      case AsmJSSimdOperation_fromFloat32x4:
        return CheckSimdCast<MSimdConvert>(f, call, AsmJSSimdType_float32x4, opType, def, type);
      case AsmJSSimdOperation_fromFloat32x4Bits:
        return CheckSimdCast<MSimdReinterpretCast>(f, call, AsmJSSimdType_float32x4, opType, def, type);

      case AsmJSSimdOperation_shiftLeftByScalar:
        return CheckSimdBinary(f, call, opType, MSimdShift::lsh, def, type);
      case AsmJSSimdOperation_shiftRightArithmeticByScalar:
        return CheckSimdBinary(f, call, opType, MSimdShift::rsh, def, type);
      case AsmJSSimdOperation_shiftRightLogicalByScalar:
        return CheckSimdBinary(f, call, opType, MSimdShift::ursh, def, type);

      case AsmJSSimdOperation_abs:
        return CheckSimdUnary(f, call, opType, MSimdUnaryArith::abs, def, type);
      case AsmJSSimdOperation_neg:
        return CheckSimdUnary(f, call, opType, MSimdUnaryArith::neg, def, type);
      case AsmJSSimdOperation_not:
        return CheckSimdUnary(f, call, opType, MSimdUnaryArith::not_, def, type);
      case AsmJSSimdOperation_sqrt:
        return CheckSimdUnary(f, call, opType, MSimdUnaryArith::sqrt, def, type);
      case AsmJSSimdOperation_reciprocal:
        return CheckSimdUnary(f, call, opType, MSimdUnaryArith::reciprocal, def, type);
      case AsmJSSimdOperation_reciprocalSqrt:
        return CheckSimdUnary(f, call, opType, MSimdUnaryArith::reciprocalSqrt, def, type);

      case AsmJSSimdOperation_swizzle:
        return CheckSimdSwizzle(f, call, opType, def, type);
      case AsmJSSimdOperation_shuffle:
        return CheckSimdShuffle(f, call, opType, def, type);

      case AsmJSSimdOperation_load:
        return CheckSimdLoad(f, call, opType, 4, def, type);
      case AsmJSSimdOperation_loadX:
        return CheckSimdLoad(f, call, opType, 1, def, type);
      case AsmJSSimdOperation_loadXY:
        return CheckSimdLoad(f, call, opType, 2, def, type);
      case AsmJSSimdOperation_loadXYZ:
        return CheckSimdLoad(f, call, opType, 3, def, type);
      case AsmJSSimdOperation_store:
        return CheckSimdStore(f, call, opType, 4, def, type);
      case AsmJSSimdOperation_storeX:
        return CheckSimdStore(f, call, opType, 1, def, type);
      case AsmJSSimdOperation_storeXY:
        return CheckSimdStore(f, call, opType, 2, def, type);
      case AsmJSSimdOperation_storeXYZ:
        return CheckSimdStore(f, call, opType, 3, def, type);

      case AsmJSSimdOperation_bitselect:
        return CheckSimdSelect(f, call, opType, /*isElementWise */ false, def, type);
      case AsmJSSimdOperation_select:
        return CheckSimdSelect(f, call, opType, /*isElementWise */ true, def, type);

      case AsmJSSimdOperation_splat: {
        DefinitionVector defs;
        if (!CheckSimdCallArgs(f, call, 1, CheckSimdScalarArgs(opType), &defs))
            return false;
        *type = opType;
        *def = f.splatSimd(defs[0], type->toMIRType());
        return true;
      }
    }
    MOZ_CRASH("unexpected simd operation in CheckSimdOperationCall");
}

static bool
CheckSimdCtorCall(FunctionCompiler& f, ParseNode* call, const ModuleCompiler::Global* global,
                  MDefinition** def, Type* type)
{
    MOZ_ASSERT(call->isKind(PNK_CALL));

    AsmJSSimdType simdType = global->simdCtorType();
    unsigned length = SimdTypeToLength(simdType);
    DefinitionVector defs;
    if (!CheckSimdCallArgs(f, call, length, CheckSimdScalarArgs(simdType), &defs))
        return false;

    // This code will need to be generalized when we handle float64x2
    MOZ_ASSERT(length == 4);
    *type = simdType;
    *def = f.constructSimd<MSimdValueX4>(defs[0], defs[1], defs[2], defs[3], type->toMIRType());
    return true;
}

static bool
CheckUncoercedCall(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_CALL));

    const ModuleCompiler::Global* global;
    if (IsCallToGlobal(f.m(), expr, &global)) {
        if (global->isMathFunction())
            return CheckMathBuiltinCall(f, expr, global->mathBuiltinFunction(), def, type);
        if (global->isAtomicsFunction())
            return CheckAtomicsBuiltinCall(f, expr, global->atomicsBuiltinFunction(), def, type);
        if (global->isSimdCtor())
            return CheckSimdCtorCall(f, expr, global, def, type);
        if (global->isSimdOperation())
            return CheckSimdOperationCall(f, expr, global, def, type);
    }

    return f.fail(expr, "all function calls must either be calls to standard lib math functions, "
                        "ignored (via f(); or comma-expression), coerced to signed (via f()|0), "
                        "coerced to float (via fround(f())) or coerced to double (via +f())");
}

static bool
CoerceResult(FunctionCompiler& f, ParseNode* expr, RetType expected, MDefinition* result,
             Type resultType, MDefinition** def, Type* type)
{
    switch (expected.which()) {
      case RetType::Void: {
        *def = nullptr;
        *type = Type::Void;
        return true;
      }

      case RetType::Signed: {
        if (!resultType.isIntish())
            return f.failf(expr, "%s is not a subtype of intish", resultType.toChars());
        *def = result;
        *type = Type::Signed;
        return true;
      }

      case RetType::Double: {
        *type = Type::Double;
        if (resultType.isMaybeDouble()) {
            *def = result;
            return true;
        }
        if (resultType.isMaybeFloat() || resultType.isSigned()) {
            *def = f.unary<MToDouble>(result);
            return true;
        }
        if (resultType.isUnsigned()) {
            *def = f.unary<MAsmJSUnsignedToDouble>(result);
            return true;
        }
        return f.failf(expr, "%s is not a subtype of double?, float?, signed or unsigned",
                       resultType.toChars());
      }

      case RetType::Float: {
        if (!CheckFloatCoercionArg(f, expr, resultType, result, def))
            return false;
        *type = Type::Float;
        return true;
      }

      case RetType::Int32x4: {
        if (!resultType.isInt32x4())
            return f.failf(expr, "%s is not a subtype of int32x4", resultType.toChars());
        *def = result;
        *type = Type::Int32x4;
        return true;
      }

      case RetType::Float32x4: {
        if (!resultType.isFloat32x4())
            return f.failf(expr, "%s is not a subtype of float32x4", resultType.toChars());
        *def = result;
        *type = Type::Float32x4;
        return true;
      }
    }

    return true;
}

static bool
CheckCoercedMathBuiltinCall(FunctionCompiler& f, ParseNode* callNode, AsmJSMathBuiltinFunction func,
                            RetType retType, MDefinition** def, Type* type)
{
    MDefinition* resultDef;
    Type resultType;
    if (!CheckMathBuiltinCall(f, callNode, func, &resultDef, &resultType))
        return false;
    return CoerceResult(f, callNode, retType, resultDef, resultType, def, type);
}

static bool
CheckCoercedSimdCall(FunctionCompiler& f, ParseNode* call, const ModuleCompiler::Global* global,
                     RetType retType, MDefinition** def, Type* type)
{
    if (global->isSimdCtor()) {
        if (!CheckSimdCtorCall(f, call, global, def, type))
            return false;
    } else {
        MOZ_ASSERT(global->isSimdOperation());
        if (!CheckSimdOperationCall(f, call, global, def, type))
            return false;
    }

    MOZ_ASSERT(type->isSimd());
    return CoerceResult(f, call, retType, *def, *type, def, type);
}

static bool
CheckCoercedAtomicsBuiltinCall(FunctionCompiler& f, ParseNode* callNode,
                               AsmJSAtomicsBuiltinFunction func, RetType retType,
                               MDefinition** resultDef, Type* resultType)
{
    MDefinition* operand;
    Type actualRetType;
    if (!CheckAtomicsBuiltinCall(f, callNode, func, &operand, &actualRetType))
        return false;
    return CoerceResult(f, callNode, retType, operand, actualRetType, resultDef, resultType);
}

static bool
CheckCoercedCall(FunctionCompiler& f, ParseNode* call, RetType retType, MDefinition** def, Type* type)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    if (!f.canCall()) {
        return f.fail(call, "call expressions may not be nested inside heap expressions when "
                            "the module contains a change-heap function");
    }

    if (IsNumericLiteral(f.m(), call)) {
        AsmJSNumLit literal = ExtractNumericLiteral(f.m(), call);
        MDefinition* result = f.constant(literal);
        return CoerceResult(f, call, retType, result, Type::Of(literal), def, type);
    }

    ParseNode* callee = CallCallee(call);

    if (callee->isKind(PNK_ELEM))
        return CheckFuncPtrCall(f, call, retType, def, type);

    if (!callee->isKind(PNK_NAME))
        return f.fail(callee, "unexpected callee expression type");

    PropertyName* calleeName = callee->name();

    if (const ModuleCompiler::Global* global = f.lookupGlobal(calleeName)) {
        switch (global->which()) {
          case ModuleCompiler::Global::FFI:
            return CheckFFICall(f, call, global->ffiIndex(), retType, def, type);
          case ModuleCompiler::Global::MathBuiltinFunction:
            return CheckCoercedMathBuiltinCall(f, call, global->mathBuiltinFunction(), retType, def, type);
          case ModuleCompiler::Global::AtomicsBuiltinFunction:
            return CheckCoercedAtomicsBuiltinCall(f, call, global->atomicsBuiltinFunction(), retType, def, type);
          case ModuleCompiler::Global::ConstantLiteral:
          case ModuleCompiler::Global::ConstantImport:
          case ModuleCompiler::Global::Variable:
          case ModuleCompiler::Global::FuncPtrTable:
          case ModuleCompiler::Global::ArrayView:
          case ModuleCompiler::Global::ArrayViewCtor:
          case ModuleCompiler::Global::ByteLength:
          case ModuleCompiler::Global::ChangeHeap:
            return f.failName(callee, "'%s' is not callable function", callee->name());
          case ModuleCompiler::Global::SimdCtor:
          case ModuleCompiler::Global::SimdOperation:
            return CheckCoercedSimdCall(f, call, global, retType, def, type);
          case ModuleCompiler::Global::Function:
            break;
        }
    }

    return CheckInternalCall(f, call, calleeName, retType, def, type);
}

static bool
CheckPos(FunctionCompiler& f, ParseNode* pos, MDefinition** def, Type* type)
{
    MOZ_ASSERT(pos->isKind(PNK_POS));
    ParseNode* operand = UnaryKid(pos);

    if (operand->isKind(PNK_CALL))
        return CheckCoercedCall(f, operand, RetType::Double, def, type);

    MDefinition* operandDef;
    Type operandType;
    if (!CheckExpr(f, operand, &operandDef, &operandType))
        return false;

    return CoerceResult(f, operand, RetType::Double, operandDef, operandType, def, type);
}

static bool
CheckNot(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_NOT));
    ParseNode* operand = UnaryKid(expr);

    MDefinition* operandDef;
    Type operandType;
    if (!CheckExpr(f, operand, &operandDef, &operandType))
        return false;

    if (!operandType.isInt())
        return f.failf(operand, "%s is not a subtype of int", operandType.toChars());

    *def = f.unary<MNot>(operandDef);
    *type = Type::Int;
    return true;
}

static bool
CheckNeg(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_NEG));
    ParseNode* operand = UnaryKid(expr);

    MDefinition* operandDef;
    Type operandType;
    if (!CheckExpr(f, operand, &operandDef, &operandType))
        return false;

    if (operandType.isInt()) {
        *def = f.unary<MAsmJSNeg>(operandDef, MIRType_Int32);
        *type = Type::Intish;
        return true;
    }

    if (operandType.isMaybeDouble()) {
        *def = f.unary<MAsmJSNeg>(operandDef, MIRType_Double);
        *type = Type::Double;
        return true;
    }

    if (operandType.isMaybeFloat()) {
        *def = f.unary<MAsmJSNeg>(operandDef, MIRType_Float32);
        *type = Type::Floatish;
        return true;
    }

    return f.failf(operand, "%s is not a subtype of int, float? or double?", operandType.toChars());
}

static bool
CheckCoerceToInt(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_BITNOT));
    ParseNode* operand = UnaryKid(expr);

    MDefinition* operandDef;
    Type operandType;
    if (!CheckExpr(f, operand, &operandDef, &operandType))
        return false;

    if (operandType.isMaybeDouble() || operandType.isMaybeFloat()) {
        *def = f.unary<MTruncateToInt32>(operandDef);
        *type = Type::Signed;
        return true;
    }

    if (!operandType.isIntish())
        return f.failf(operand, "%s is not a subtype of double?, float? or intish", operandType.toChars());

    *def = operandDef;
    *type = Type::Signed;
    return true;
}

static bool
CheckBitNot(FunctionCompiler& f, ParseNode* neg, MDefinition** def, Type* type)
{
    MOZ_ASSERT(neg->isKind(PNK_BITNOT));
    ParseNode* operand = UnaryKid(neg);

    if (operand->isKind(PNK_BITNOT))
        return CheckCoerceToInt(f, operand, def, type);

    MDefinition* operandDef;
    Type operandType;
    if (!CheckExpr(f, operand, &operandDef, &operandType))
        return false;

    if (!operandType.isIntish())
        return f.failf(operand, "%s is not a subtype of intish", operandType.toChars());

    *def = f.bitwise<MBitNot>(operandDef);
    *type = Type::Signed;
    return true;
}

static bool
CheckComma(FunctionCompiler& f, ParseNode* comma, MDefinition** def, Type* type)
{
    MOZ_ASSERT(comma->isKind(PNK_COMMA));
    ParseNode* operands = ListHead(comma);

    ParseNode* pn = operands;
    for (; NextNode(pn); pn = NextNode(pn)) {
        MDefinition* _1;
        Type _2;
        if (pn->isKind(PNK_CALL)) {
            if (!CheckCoercedCall(f, pn, RetType::Void, &_1, &_2))
                return false;
        } else {
            if (!CheckExpr(f, pn, &_1, &_2))
                return false;
        }
    }

    if (!CheckExpr(f, pn, def, type))
        return false;

    return true;
}

static bool
CheckConditional(FunctionCompiler& f, ParseNode* ternary, MDefinition** def, Type* type)
{
    MOZ_ASSERT(ternary->isKind(PNK_CONDITIONAL));
    ParseNode* cond = TernaryKid1(ternary);
    ParseNode* thenExpr = TernaryKid2(ternary);
    ParseNode* elseExpr = TernaryKid3(ternary);

    MDefinition* condDef;
    Type condType;
    if (!CheckExpr(f, cond, &condDef, &condType))
        return false;

    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    MBasicBlock* thenBlock = nullptr, *elseBlock = nullptr;
    if (!f.branchAndStartThen(condDef, &thenBlock, &elseBlock, thenExpr, elseExpr))
        return false;

    MDefinition* thenDef;
    Type thenType;
    if (!CheckExpr(f, thenExpr, &thenDef, &thenType))
        return false;

    BlockVector thenBlocks(f.cx());
    if (!f.appendThenBlock(&thenBlocks))
        return false;

    f.pushPhiInput(thenDef);
    f.switchToElse(elseBlock);

    MDefinition* elseDef;
    Type elseType;
    if (!CheckExpr(f, elseExpr, &elseDef, &elseType))
        return false;

    f.pushPhiInput(elseDef);

    if (thenType.isInt() && elseType.isInt()) {
        *type = Type::Int;
    } else if (thenType.isDouble() && elseType.isDouble()) {
        *type = Type::Double;
    } else if (thenType.isFloat() && elseType.isFloat()) {
        *type = Type::Float;
    } else if (elseType.isSimd() && thenType <= elseType && elseType <= thenType) {
        *type = thenType;
    } else {
        return f.failf(ternary, "then/else branches of conditional must both produce int, float, "
                       "double or SIMD types, current types are %s and %s",
                       thenType.toChars(), elseType.toChars());
    }

    if (!f.joinIfElse(thenBlocks, elseExpr))
        return false;

    *def = f.popPhiOutput();
    return true;
}

static bool
IsValidIntMultiplyConstant(ModuleCompiler& m, ParseNode* expr)
{
    if (!IsNumericLiteral(m, expr))
        return false;

    AsmJSNumLit literal = ExtractNumericLiteral(m, expr);
    switch (literal.which()) {
      case AsmJSNumLit::Fixnum:
      case AsmJSNumLit::NegativeInt:
        if (abs(literal.toInt32()) < (1<<20))
            return true;
        return false;
      case AsmJSNumLit::BigUnsigned:
      case AsmJSNumLit::Double:
      case AsmJSNumLit::Float:
      case AsmJSNumLit::OutOfRangeInt:
      case AsmJSNumLit::Int32x4:
      case AsmJSNumLit::Float32x4:
        return false;
    }

    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("Bad literal");
}

static bool
CheckMultiply(FunctionCompiler& f, ParseNode* star, MDefinition** def, Type* type)
{
    MOZ_ASSERT(star->isKind(PNK_STAR));
    ParseNode* lhs = MultiplyLeft(star);
    ParseNode* rhs = MultiplyRight(star);

    MDefinition* lhsDef;
    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsDef, &lhsType))
        return false;

    MDefinition* rhsDef;
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    if (lhsType.isInt() && rhsType.isInt()) {
        if (!IsValidIntMultiplyConstant(f.m(), lhs) && !IsValidIntMultiplyConstant(f.m(), rhs))
            return f.fail(star, "one arg to int multiply must be a small (-2^20, 2^20) int literal");
        *def = f.mul(lhsDef, rhsDef, MIRType_Int32, MMul::Integer);
        *type = Type::Intish;
        return true;
    }

    if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *def = f.mul(lhsDef, rhsDef, MIRType_Double, MMul::Normal);
        *type = Type::Double;
        return true;
    }

    if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        *def = f.mul(lhsDef, rhsDef, MIRType_Float32, MMul::Normal);
        *type = Type::Floatish;
        return true;
    }

    return f.fail(star, "multiply operands must be both int, both double? or both float?");
}

static bool
CheckAddOrSub(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type,
              unsigned* numAddOrSubOut = nullptr)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    MOZ_ASSERT(expr->isKind(PNK_ADD) || expr->isKind(PNK_SUB));
    ParseNode* lhs = AddSubLeft(expr);
    ParseNode* rhs = AddSubRight(expr);

    MDefinition* lhsDef, *rhsDef;
    Type lhsType, rhsType;
    unsigned lhsNumAddOrSub, rhsNumAddOrSub;

    if (lhs->isKind(PNK_ADD) || lhs->isKind(PNK_SUB)) {
        if (!CheckAddOrSub(f, lhs, &lhsDef, &lhsType, &lhsNumAddOrSub))
            return false;
        if (lhsType == Type::Intish)
            lhsType = Type::Int;
    } else {
        if (!CheckExpr(f, lhs, &lhsDef, &lhsType))
            return false;
        lhsNumAddOrSub = 0;
    }

    if (rhs->isKind(PNK_ADD) || rhs->isKind(PNK_SUB)) {
        if (!CheckAddOrSub(f, rhs, &rhsDef, &rhsType, &rhsNumAddOrSub))
            return false;
        if (rhsType == Type::Intish)
            rhsType = Type::Int;
    } else {
        if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
            return false;
        rhsNumAddOrSub = 0;
    }

    unsigned numAddOrSub = lhsNumAddOrSub + rhsNumAddOrSub + 1;
    if (numAddOrSub > (1<<20))
        return f.fail(expr, "too many + or - without intervening coercion");

    if (lhsType.isInt() && rhsType.isInt()) {
        *def = expr->isKind(PNK_ADD)
               ? f.binary<MAdd>(lhsDef, rhsDef, MIRType_Int32)
               : f.binary<MSub>(lhsDef, rhsDef, MIRType_Int32);
        *type = Type::Intish;
    } else if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *def = expr->isKind(PNK_ADD)
               ? f.binary<MAdd>(lhsDef, rhsDef, MIRType_Double)
               : f.binary<MSub>(lhsDef, rhsDef, MIRType_Double);
        *type = Type::Double;
    } else if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        *def = expr->isKind(PNK_ADD)
               ? f.binary<MAdd>(lhsDef, rhsDef, MIRType_Float32)
               : f.binary<MSub>(lhsDef, rhsDef, MIRType_Float32);
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
CheckDivOrMod(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type)
{
    MOZ_ASSERT(expr->isKind(PNK_DIV) || expr->isKind(PNK_MOD));

    ParseNode* lhs = DivOrModLeft(expr);
    ParseNode* rhs = DivOrModRight(expr);

    MDefinition* lhsDef, *rhsDef;
    Type lhsType, rhsType;
    if (!CheckExpr(f, lhs, &lhsDef, &lhsType))
        return false;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    if (lhsType.isMaybeDouble() && rhsType.isMaybeDouble()) {
        *def = expr->isKind(PNK_DIV)
               ? f.div(lhsDef, rhsDef, MIRType_Double, /* unsignd = */ false)
               : f.mod(lhsDef, rhsDef, MIRType_Double, /* unsignd = */ false);
        *type = Type::Double;
        return true;
    }

    if (lhsType.isMaybeFloat() && rhsType.isMaybeFloat()) {
        if (expr->isKind(PNK_DIV))
            *def = f.div(lhsDef, rhsDef, MIRType_Float32, /* unsignd = */ false);
        else
            return f.fail(expr, "modulo cannot receive float arguments");
        *type = Type::Floatish;
        return true;
    }

    if (lhsType.isSigned() && rhsType.isSigned()) {
        if (expr->isKind(PNK_DIV))
            *def = f.div(lhsDef, rhsDef, MIRType_Int32, /* unsignd = */ false);
        else
            *def = f.mod(lhsDef, rhsDef, MIRType_Int32, /* unsignd = */ false);
        *type = Type::Intish;
        return true;
    }

    if (lhsType.isUnsigned() && rhsType.isUnsigned()) {
        if (expr->isKind(PNK_DIV))
            *def = f.div(lhsDef, rhsDef, MIRType_Int32, /* unsignd = */ true);
        else
            *def = f.mod(lhsDef, rhsDef, MIRType_Int32, /* unsignd = */ true);
        *type = Type::Intish;
        return true;
    }

    return f.failf(expr, "arguments to / or %% must both be double?, float?, signed, or unsigned; "
                   "%s and %s are given", lhsType.toChars(), rhsType.toChars());
}

static bool
CheckComparison(FunctionCompiler& f, ParseNode* comp, MDefinition** def, Type* type)
{
    MOZ_ASSERT(comp->isKind(PNK_LT) || comp->isKind(PNK_LE) || comp->isKind(PNK_GT) ||
               comp->isKind(PNK_GE) || comp->isKind(PNK_EQ) || comp->isKind(PNK_NE));

    ParseNode* lhs = ComparisonLeft(comp);
    ParseNode* rhs = ComparisonRight(comp);

    MDefinition* lhsDef, *rhsDef;
    Type lhsType, rhsType;
    if (!CheckExpr(f, lhs, &lhsDef, &lhsType))
        return false;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    if ((lhsType.isSigned() && rhsType.isSigned()) || (lhsType.isUnsigned() && rhsType.isUnsigned())) {
        MCompare::CompareType compareType = (lhsType.isUnsigned() && rhsType.isUnsigned())
                                            ? MCompare::Compare_UInt32
                                            : MCompare::Compare_Int32;
        *def = f.compare(lhsDef, rhsDef, comp->getOp(), compareType);
        *type = Type::Int;
        return true;
    }

    if (lhsType.isDouble() && rhsType.isDouble()) {
        *def = f.compare(lhsDef, rhsDef, comp->getOp(), MCompare::Compare_Double);
        *type = Type::Int;
        return true;
    }

    if (lhsType.isFloat() && rhsType.isFloat()) {
        *def = f.compare(lhsDef, rhsDef, comp->getOp(), MCompare::Compare_Float32);
        *type = Type::Int;
        return true;
    }

    return f.failf(comp, "arguments to a comparison must both be signed, unsigned, floats or doubles; "
                   "%s and %s are given", lhsType.toChars(), rhsType.toChars());
}

static bool
CheckBitwise(FunctionCompiler& f, ParseNode* bitwise, MDefinition** def, Type* type)
{
    ParseNode* lhs = BitwiseLeft(bitwise);
    ParseNode* rhs = BitwiseRight(bitwise);

    int32_t identityElement;
    bool onlyOnRight;
    switch (bitwise->getKind()) {
      case PNK_BITOR:  identityElement = 0;  onlyOnRight = false; *type = Type::Signed;   break;
      case PNK_BITAND: identityElement = -1; onlyOnRight = false; *type = Type::Signed;   break;
      case PNK_BITXOR: identityElement = 0;  onlyOnRight = false; *type = Type::Signed;   break;
      case PNK_LSH:    identityElement = 0;  onlyOnRight = true;  *type = Type::Signed;   break;
      case PNK_RSH:    identityElement = 0;  onlyOnRight = true;  *type = Type::Signed;   break;
      case PNK_URSH:   identityElement = 0;  onlyOnRight = true;  *type = Type::Unsigned; break;
      default: MOZ_CRASH("not a bitwise op");
    }

    uint32_t i;
    if (!onlyOnRight && IsLiteralInt(f.m(), lhs, &i) && i == uint32_t(identityElement)) {
        Type rhsType;
        if (!CheckExpr(f, rhs, def, &rhsType))
            return false;
        if (!rhsType.isIntish())
            return f.failf(bitwise, "%s is not a subtype of intish", rhsType.toChars());
        return true;
    }

    if (IsLiteralInt(f.m(), rhs, &i) && i == uint32_t(identityElement)) {
        if (bitwise->isKind(PNK_BITOR) && lhs->isKind(PNK_CALL))
            return CheckCoercedCall(f, lhs, RetType::Signed, def, type);

        Type lhsType;
        if (!CheckExpr(f, lhs, def, &lhsType))
            return false;
        if (!lhsType.isIntish())
            return f.failf(bitwise, "%s is not a subtype of intish", lhsType.toChars());
        return true;
    }

    MDefinition* lhsDef;
    Type lhsType;
    if (!CheckExpr(f, lhs, &lhsDef, &lhsType))
        return false;

    MDefinition* rhsDef;
    Type rhsType;
    if (!CheckExpr(f, rhs, &rhsDef, &rhsType))
        return false;

    if (!lhsType.isIntish())
        return f.failf(lhs, "%s is not a subtype of intish", lhsType.toChars());
    if (!rhsType.isIntish())
        return f.failf(rhs, "%s is not a subtype of intish", rhsType.toChars());

    switch (bitwise->getKind()) {
      case PNK_BITOR:  *def = f.bitwise<MBitOr>(lhsDef, rhsDef); break;
      case PNK_BITAND: *def = f.bitwise<MBitAnd>(lhsDef, rhsDef); break;
      case PNK_BITXOR: *def = f.bitwise<MBitXor>(lhsDef, rhsDef); break;
      case PNK_LSH:    *def = f.bitwise<MLsh>(lhsDef, rhsDef); break;
      case PNK_RSH:    *def = f.bitwise<MRsh>(lhsDef, rhsDef); break;
      case PNK_URSH:   *def = f.bitwise<MUrsh>(lhsDef, rhsDef); break;
      default: MOZ_CRASH("not a bitwise op");
    }

    return true;
}

static bool
CheckExpr(FunctionCompiler& f, ParseNode* expr, MDefinition** def, Type* type)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    if (!f.mirGen().ensureBallast())
        return false;

    if (IsNumericLiteral(f.m(), expr))
        return CheckNumericLiteral(f, expr, def, type);

    switch (expr->getKind()) {
      case PNK_NAME:        return CheckVarRef(f, expr, def, type);
      case PNK_ELEM:        return CheckLoadArray(f, expr, def, type);
      case PNK_DOT:         return CheckDotAccess(f, expr, def, type);
      case PNK_ASSIGN:      return CheckAssign(f, expr, def, type);
      case PNK_POS:         return CheckPos(f, expr, def, type);
      case PNK_NOT:         return CheckNot(f, expr, def, type);
      case PNK_NEG:         return CheckNeg(f, expr, def, type);
      case PNK_BITNOT:      return CheckBitNot(f, expr, def, type);
      case PNK_COMMA:       return CheckComma(f, expr, def, type);
      case PNK_CONDITIONAL: return CheckConditional(f, expr, def, type);
      case PNK_STAR:        return CheckMultiply(f, expr, def, type);
      case PNK_CALL:        return CheckUncoercedCall(f, expr, def, type);

      case PNK_ADD:
      case PNK_SUB:         return CheckAddOrSub(f, expr, def, type);

      case PNK_DIV:
      case PNK_MOD:         return CheckDivOrMod(f, expr, def, type);

      case PNK_LT:
      case PNK_LE:
      case PNK_GT:
      case PNK_GE:
      case PNK_EQ:
      case PNK_NE:          return CheckComparison(f, expr, def, type);

      case PNK_BITOR:
      case PNK_BITAND:
      case PNK_BITXOR:
      case PNK_LSH:
      case PNK_RSH:
      case PNK_URSH:        return CheckBitwise(f, expr, def, type);

      default:;
    }

    return f.fail(expr, "unsupported expression");
}

static bool
CheckStatement(FunctionCompiler& f, ParseNode* stmt, LabelVector* maybeLabels = nullptr);

static bool
CheckExprStatement(FunctionCompiler& f, ParseNode* exprStmt)
{
    MOZ_ASSERT(exprStmt->isKind(PNK_SEMI));
    ParseNode* expr = UnaryKid(exprStmt);

    if (!expr)
        return true;

    MDefinition* _1;
    Type _2;

    if (expr->isKind(PNK_CALL))
        return CheckCoercedCall(f, expr, RetType::Void, &_1, &_2);

    return CheckExpr(f, UnaryKid(exprStmt), &_1, &_2);
}

static bool
CheckWhile(FunctionCompiler& f, ParseNode* whileStmt, const LabelVector* maybeLabels)
{
    MOZ_ASSERT(whileStmt->isKind(PNK_WHILE));
    ParseNode* cond = BinaryLeft(whileStmt);
    ParseNode* body = BinaryRight(whileStmt);

    MBasicBlock* loopEntry;
    if (!f.startPendingLoop(whileStmt, &loopEntry, body))
        return false;

    MDefinition* condDef;
    Type condType;
    if (!CheckExpr(f, cond, &condDef, &condType))
        return false;

    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    MBasicBlock* afterLoop;
    if (!f.branchAndStartLoopBody(condDef, &afterLoop, body, NextNode(whileStmt)))
        return false;

    if (!CheckStatement(f, body))
        return false;

    if (!f.bindContinues(whileStmt, maybeLabels))
        return false;

    return f.closeLoop(loopEntry, afterLoop);
}

static bool
CheckFor(FunctionCompiler& f, ParseNode* forStmt, const LabelVector* maybeLabels)
{
    MOZ_ASSERT(forStmt->isKind(PNK_FOR));
    ParseNode* forHead = BinaryLeft(forStmt);
    ParseNode* body = BinaryRight(forStmt);

    if (!forHead->isKind(PNK_FORHEAD))
        return f.fail(forHead, "unsupported for-loop statement");

    ParseNode* maybeInit = TernaryKid1(forHead);
    ParseNode* maybeCond = TernaryKid2(forHead);
    ParseNode* maybeInc = TernaryKid3(forHead);

    if (maybeInit) {
        MDefinition* _1;
        Type _2;
        if (!CheckExpr(f, maybeInit, &_1, &_2))
            return false;
    }

    MBasicBlock* loopEntry;
    if (!f.startPendingLoop(forStmt, &loopEntry, body))
        return false;

    MDefinition* condDef;
    if (maybeCond) {
        Type condType;
        if (!CheckExpr(f, maybeCond, &condDef, &condType))
            return false;

        if (!condType.isInt())
            return f.failf(maybeCond, "%s is not a subtype of int", condType.toChars());
    } else {
        condDef = f.constant(Int32Value(1), Type::Int);
    }

    MBasicBlock* afterLoop;
    if (!f.branchAndStartLoopBody(condDef, &afterLoop, body, NextNode(forStmt)))
        return false;

    if (!CheckStatement(f, body))
        return false;

    if (!f.bindContinues(forStmt, maybeLabels))
        return false;

    if (maybeInc) {
        MDefinition* _1;
        Type _2;
        if (!CheckExpr(f, maybeInc, &_1, &_2))
            return false;
    }

    return f.closeLoop(loopEntry, afterLoop);
}

static bool
CheckDoWhile(FunctionCompiler& f, ParseNode* whileStmt, const LabelVector* maybeLabels)
{
    MOZ_ASSERT(whileStmt->isKind(PNK_DOWHILE));
    ParseNode* body = BinaryLeft(whileStmt);
    ParseNode* cond = BinaryRight(whileStmt);

    MBasicBlock* loopEntry;
    if (!f.startPendingLoop(whileStmt, &loopEntry, body))
        return false;

    if (!CheckStatement(f, body))
        return false;

    if (!f.bindContinues(whileStmt, maybeLabels))
        return false;

    MDefinition* condDef;
    Type condType;
    if (!CheckExpr(f, cond, &condDef, &condType))
        return false;

    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    return f.branchAndCloseDoWhileLoop(condDef, loopEntry, NextNode(whileStmt));
}

static bool
CheckLabel(FunctionCompiler& f, ParseNode* labeledStmt, LabelVector* maybeLabels)
{
    MOZ_ASSERT(labeledStmt->isKind(PNK_LABEL));
    PropertyName* label = LabeledStatementLabel(labeledStmt);
    ParseNode* stmt = LabeledStatementStatement(labeledStmt);

    if (maybeLabels) {
        if (!maybeLabels->append(label))
            return false;
        if (!CheckStatement(f, stmt, maybeLabels))
            return false;
        return true;
    }

    LabelVector labels(f.cx());
    if (!labels.append(label))
        return false;

    if (!CheckStatement(f, stmt, &labels))
        return false;

    return f.bindLabeledBreaks(&labels, labeledStmt);
}

static bool
CheckLeafCondition(FunctionCompiler& f, ParseNode* cond, ParseNode* thenStmt, ParseNode* elseOrJoinStmt,
                   MBasicBlock** thenBlock, MBasicBlock** elseOrJoinBlock)
{
    MDefinition* condDef;
    Type condType;
    if (!CheckExpr(f, cond, &condDef, &condType))
        return false;
    if (!condType.isInt())
        return f.failf(cond, "%s is not a subtype of int", condType.toChars());

    if (!f.branchAndStartThen(condDef, thenBlock, elseOrJoinBlock, thenStmt, elseOrJoinStmt))
        return false;
    return true;
}

static bool
CheckIfCondition(FunctionCompiler& f, ParseNode* cond, ParseNode* thenStmt, ParseNode* elseOrJoinStmt,
                 MBasicBlock** thenBlock, MBasicBlock** elseOrJoinBlock);

static bool
CheckIfConditional(FunctionCompiler& f, ParseNode* conditional, ParseNode* thenStmt, ParseNode* elseOrJoinStmt,
                   MBasicBlock** thenBlock, MBasicBlock** elseOrJoinBlock)
{
    MOZ_ASSERT(conditional->isKind(PNK_CONDITIONAL));

    // a ? b : c <=> (a && b) || (!a && c)
    // b is always referred to the AND condition, as we need A and B to reach this test,
    // c is always referred as the OR condition, as we reach it if we don't have A.
    ParseNode* cond = TernaryKid1(conditional);
    ParseNode* lhs = TernaryKid2(conditional);
    ParseNode* rhs = TernaryKid3(conditional);

    MBasicBlock* maybeAndTest = nullptr, *maybeOrTest = nullptr;
    MBasicBlock** ifTrueBlock = &maybeAndTest, **ifFalseBlock = &maybeOrTest;
    ParseNode* ifTrueBlockNode = lhs, *ifFalseBlockNode = rhs;

    // Try to spot opportunities for short-circuiting in the AND subpart
    uint32_t andTestLiteral = 0;
    bool skipAndTest = false;

    if (IsLiteralInt(f.m(), lhs, &andTestLiteral)) {
        skipAndTest = true;
        if (andTestLiteral == 0) {
            // (a ? 0 : b) is equivalent to !a && b
            // If a is true, jump to the elseBlock directly
            ifTrueBlock = elseOrJoinBlock;
            ifTrueBlockNode = elseOrJoinStmt;
        } else {
            // (a ? 1 : b) is equivalent to a || b
            // If a is true, jump to the thenBlock directly
            ifTrueBlock = thenBlock;
            ifTrueBlockNode = thenStmt;
        }
    }

    // Try to spot opportunities for short-circuiting in the OR subpart
    uint32_t orTestLiteral = 0;
    bool skipOrTest = false;

    if (IsLiteralInt(f.m(), rhs, &orTestLiteral)) {
        skipOrTest = true;
        if (orTestLiteral == 0) {
            // (a ? b : 0) is equivalent to a && b
            // If a is false, jump to the elseBlock directly
            ifFalseBlock = elseOrJoinBlock;
            ifFalseBlockNode = elseOrJoinStmt;
        } else {
            // (a ? b : 1) is equivalent to !a || b
            // If a is false, jump to the thenBlock directly
            ifFalseBlock = thenBlock;
            ifFalseBlockNode = thenStmt;
        }
    }

    // Pathological cases: a ? 0 : 0 (i.e. false) or a ? 1 : 1 (i.e. true)
    // These cases can't be optimized properly at this point: one of the blocks might be
    // created and won't ever be executed. Furthermore, it introduces inconsistencies in the
    // MIR graph (even if we try to create a block by hand, it will have no predecessor, which
    // breaks graph assumptions). The only way we could optimize it is to do it directly in
    // CheckIf by removing the control flow entirely.
    if (skipOrTest && skipAndTest && (!!orTestLiteral == !!andTestLiteral))
        return CheckLeafCondition(f, conditional, thenStmt, elseOrJoinStmt, thenBlock, elseOrJoinBlock);

    if (!CheckIfCondition(f, cond, ifTrueBlockNode, ifFalseBlockNode, ifTrueBlock, ifFalseBlock))
        return false;
    f.assertCurrentBlockIs(*ifTrueBlock);

    // Add supplementary tests, if needed
    if (!skipAndTest) {
        if (!CheckIfCondition(f, lhs, thenStmt, elseOrJoinStmt, thenBlock, elseOrJoinBlock))
            return false;
        f.assertCurrentBlockIs(*thenBlock);
    }

    if (!skipOrTest) {
        f.switchToElse(*ifFalseBlock);
        if (!CheckIfCondition(f, rhs, thenStmt, elseOrJoinStmt, thenBlock, elseOrJoinBlock))
            return false;
        f.assertCurrentBlockIs(*thenBlock);
    }

    // We might not be on the thenBlock in one case
    if (ifTrueBlock == elseOrJoinBlock) {
        MOZ_ASSERT(skipAndTest && andTestLiteral == 0);
        f.switchToElse(*thenBlock);
    }

    // Check post-conditions
    f.assertCurrentBlockIs(*thenBlock);
    MOZ_ASSERT_IF(!f.inDeadCode(), *thenBlock && *elseOrJoinBlock);
    return true;
}

// Recursive function that checks for a complex condition (formed with ternary
// conditionals) and creates the associated short-circuiting control flow graph.
//
// After a call to CheckCondition, the followings are true:
// - if *thenBlock and *elseOrJoinBlock were non-null on entry, their value is
//   not changed by this function.
// - *thenBlock and *elseOrJoinBlock are non-null on exit.
// - the current block on exit is the *thenBlock.
static bool
CheckIfCondition(FunctionCompiler& f, ParseNode* cond, ParseNode* thenStmt,
                 ParseNode* elseOrJoinStmt, MBasicBlock** thenBlock, MBasicBlock** elseOrJoinBlock)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    if (cond->isKind(PNK_CONDITIONAL))
        return CheckIfConditional(f, cond, thenStmt, elseOrJoinStmt, thenBlock, elseOrJoinBlock);

    // We've reached a leaf, i.e. an atomic condition
    if (!CheckLeafCondition(f, cond, thenStmt, elseOrJoinStmt, thenBlock, elseOrJoinBlock))
        return false;

    // Check post-conditions
    f.assertCurrentBlockIs(*thenBlock);
    MOZ_ASSERT_IF(!f.inDeadCode(), *thenBlock && *elseOrJoinBlock);
    return true;
}

static bool
CheckIf(FunctionCompiler& f, ParseNode* ifStmt)
{
    // Handle if/else-if chains using iteration instead of recursion. This
    // avoids blowing the C stack quota for long if/else-if chains and also
    // creates fewer MBasicBlocks at join points (by creating one join block
    // for the entire if/else-if chain).
    BlockVector thenBlocks(f.cx());

    ParseNode* nextStmt = NextNode(ifStmt);
  recurse:
    MOZ_ASSERT(ifStmt->isKind(PNK_IF));
    ParseNode* cond = TernaryKid1(ifStmt);
    ParseNode* thenStmt = TernaryKid2(ifStmt);
    ParseNode* elseStmt = TernaryKid3(ifStmt);

    MBasicBlock* thenBlock = nullptr, *elseBlock = nullptr;
    ParseNode* elseOrJoinStmt = elseStmt ? elseStmt : nextStmt;

    if (!CheckIfCondition(f, cond, thenStmt, elseOrJoinStmt, &thenBlock, &elseBlock))
        return false;

    if (!CheckStatement(f, thenStmt))
        return false;

    if (!f.appendThenBlock(&thenBlocks))
        return false;

    if (!elseStmt) {
        if (!f.joinIf(thenBlocks, elseBlock))
            return false;
    } else {
        f.switchToElse(elseBlock);

        if (elseStmt->isKind(PNK_IF)) {
            ifStmt = elseStmt;
            goto recurse;
        }

        if (!CheckStatement(f, elseStmt))
            return false;

        if (!f.joinIfElse(thenBlocks, nextStmt))
            return false;
    }

    return true;
}

static bool
CheckCaseExpr(FunctionCompiler& f, ParseNode* caseExpr, int32_t* value)
{
    if (!IsNumericLiteral(f.m(), caseExpr))
        return f.fail(caseExpr, "switch case expression must be an integer literal");

    AsmJSNumLit literal = ExtractNumericLiteral(f.m(), caseExpr);
    switch (literal.which()) {
      case AsmJSNumLit::Fixnum:
      case AsmJSNumLit::NegativeInt:
        *value = literal.toInt32();
        break;
      case AsmJSNumLit::OutOfRangeInt:
      case AsmJSNumLit::BigUnsigned:
        return f.fail(caseExpr, "switch case expression out of integer range");
      case AsmJSNumLit::Double:
      case AsmJSNumLit::Float:
      case AsmJSNumLit::Int32x4:
      case AsmJSNumLit::Float32x4:
        return f.fail(caseExpr, "switch case expression must be an integer literal");
    }

    return true;
}

static bool
CheckDefaultAtEnd(FunctionCompiler& f, ParseNode* stmt)
{
    for (; stmt; stmt = NextNode(stmt)) {
        MOZ_ASSERT(stmt->isKind(PNK_CASE) || stmt->isKind(PNK_DEFAULT));
        if (stmt->isKind(PNK_DEFAULT) && NextNode(stmt) != nullptr)
            return f.fail(stmt, "default label must be at the end");
    }

    return true;
}

static bool
CheckSwitchRange(FunctionCompiler& f, ParseNode* stmt, int32_t* low, int32_t* high,
                 int32_t* tableLength)
{
    if (stmt->isKind(PNK_DEFAULT)) {
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
    for (stmt = NextNode(stmt); stmt && stmt->isKind(PNK_CASE); stmt = NextNode(stmt)) {
        int32_t i = 0;
        if (!CheckCaseExpr(f, CaseExpr(stmt), &i))
            return false;

        *low = Min(*low, i);
        *high = Max(*high, i);
    }

    int64_t i64 = (int64_t(*high) - int64_t(*low)) + 1;
    if (i64 > 4*1024*1024)
        return f.fail(initialStmt, "all switch statements generate tables; this table would be too big");

    *tableLength = int32_t(i64);
    return true;
}

static bool
CheckSwitch(FunctionCompiler& f, ParseNode* switchStmt)
{
    MOZ_ASSERT(switchStmt->isKind(PNK_SWITCH));
    ParseNode* switchExpr = BinaryLeft(switchStmt);
    ParseNode* switchBody = BinaryRight(switchStmt);

    if (!switchBody->isKind(PNK_STATEMENTLIST))
        return f.fail(switchBody, "switch body may not contain 'let' declarations");

    MDefinition* exprDef;
    Type exprType;
    if (!CheckExpr(f, switchExpr, &exprDef, &exprType))
        return false;

    if (!exprType.isSigned())
        return f.failf(switchExpr, "%s is not a subtype of signed", exprType.toChars());

    ParseNode* stmt = ListHead(switchBody);

    if (!CheckDefaultAtEnd(f, stmt))
        return false;

    if (!stmt)
        return true;

    int32_t low = 0, high = 0, tableLength = 0;
    if (!CheckSwitchRange(f, stmt, &low, &high, &tableLength))
        return false;

    BlockVector cases(f.cx());
    if (!cases.resize(tableLength))
        return false;

    MBasicBlock* switchBlock;
    if (!f.startSwitch(switchStmt, exprDef, low, high, &switchBlock))
        return false;

    for (; stmt && stmt->isKind(PNK_CASE); stmt = NextNode(stmt)) {
        int32_t caseValue = ExtractNumericLiteral(f.m(), CaseExpr(stmt)).toInt32();
        unsigned caseIndex = caseValue - low;

        if (cases[caseIndex])
            return f.fail(stmt, "no duplicate case labels");

        if (!f.startSwitchCase(switchBlock, &cases[caseIndex], stmt))
            return false;

        if (!CheckStatement(f, CaseBody(stmt)))
            return false;
    }

    MBasicBlock* defaultBlock;
    if (!f.startSwitchDefault(switchBlock, &cases, &defaultBlock, stmt))
        return false;

    if (stmt && stmt->isKind(PNK_DEFAULT)) {
        if (!CheckStatement(f, CaseBody(stmt)))
            return false;
    }

    return f.joinSwitch(switchBlock, cases, defaultBlock);
}

static bool
CheckReturnType(FunctionCompiler& f, ParseNode* usepn, RetType retType)
{
    if (!f.hasAlreadyReturned()) {
        f.setReturnedType(retType);
        return true;
    }

    if (f.returnedType() != retType) {
        return f.failf(usepn, "%s incompatible with previous return of type %s",
                       retType.toType().toChars(), f.returnedType().toType().toChars());
    }

    return true;
}

static bool
CheckReturn(FunctionCompiler& f, ParseNode* returnStmt)
{
    ParseNode* expr = ReturnExpr(returnStmt);

    if (!expr) {
        if (!CheckReturnType(f, returnStmt, RetType::Void))
            return false;

        f.returnVoid();
        return true;
    }

    MDefinition* def;
    Type type;
    if (!CheckExpr(f, expr, &def, &type))
        return false;

    RetType retType;
    if (type.isSigned())
        retType = RetType::Signed;
    else if (type.isDouble())
        retType = RetType::Double;
    else if (type.isFloat())
        retType = RetType::Float;
    else if (type.isInt32x4())
        retType = RetType::Int32x4;
    else if (type.isFloat32x4())
        retType = RetType::Float32x4;
    else if (type.isVoid())
        retType = RetType::Void;
    else
        return f.failf(expr, "%s is not a valid return type", type.toChars());

    if (!CheckReturnType(f, expr, retType))
        return false;

    if (retType == RetType::Void)
        f.returnVoid();
    else
        f.returnExpr(def);
    return true;
}

static bool
CheckStatementList(FunctionCompiler& f, ParseNode* stmtList)
{
    MOZ_ASSERT(stmtList->isKind(PNK_STATEMENTLIST));

    for (ParseNode* stmt = ListHead(stmtList); stmt; stmt = NextNode(stmt)) {
        if (!CheckStatement(f, stmt))
            return false;
    }

    return true;
}

static bool
CheckStatement(FunctionCompiler& f, ParseNode* stmt, LabelVector* maybeLabels)
{
    JS_CHECK_RECURSION_DONT_REPORT(f.cx(), return f.m().failOverRecursed());

    if (!f.mirGen().ensureBallast())
        return false;

    switch (stmt->getKind()) {
      case PNK_SEMI:          return CheckExprStatement(f, stmt);
      case PNK_WHILE:         return CheckWhile(f, stmt, maybeLabels);
      case PNK_FOR:           return CheckFor(f, stmt, maybeLabels);
      case PNK_DOWHILE:       return CheckDoWhile(f, stmt, maybeLabels);
      case PNK_LABEL:         return CheckLabel(f, stmt, maybeLabels);
      case PNK_IF:            return CheckIf(f, stmt);
      case PNK_SWITCH:        return CheckSwitch(f, stmt);
      case PNK_RETURN:        return CheckReturn(f, stmt);
      case PNK_STATEMENTLIST: return CheckStatementList(f, stmt);
      case PNK_BREAK:         return f.addBreak(LoopControlMaybeLabel(stmt));
      case PNK_CONTINUE:      return f.addContinue(LoopControlMaybeLabel(stmt));
      default:;
    }

    return f.fail(stmt, "unexpected statement kind");
}

static bool
CheckByteLengthCall(ModuleCompiler& m, ParseNode* pn, PropertyName* newBufferName)
{
    if (!pn->isKind(PNK_CALL) || !CallCallee(pn)->isKind(PNK_NAME))
        return m.fail(pn, "expecting call to imported byteLength");

    const ModuleCompiler::Global* global = m.lookupGlobal(CallCallee(pn)->name());
    if (!global || global->which() != ModuleCompiler::Global::ByteLength)
        return m.fail(pn, "expecting call to imported byteLength");

    if (CallArgListLength(pn) != 1 || !IsUseOfName(CallArgList(pn), newBufferName))
        return m.failName(pn, "expecting %s as argument to byteLength call", newBufferName);

    return true;
}

static bool
CheckHeapLengthCondition(ModuleCompiler& m, ParseNode* cond, PropertyName* newBufferName,
                         uint32_t* mask, uint32_t* minLength, uint32_t* maxLength)
{
    if (!cond->isKind(PNK_OR) || !AndOrLeft(cond)->isKind(PNK_OR))
        return m.fail(cond, "expecting byteLength & K || byteLength <= L || byteLength > M");

    ParseNode* cond1 = AndOrLeft(AndOrLeft(cond));
    ParseNode* cond2 = AndOrRight(AndOrLeft(cond));
    ParseNode* cond3 = AndOrRight(cond);

    if (!cond1->isKind(PNK_BITAND))
        return m.fail(cond1, "expecting byteLength & K");

    if (!CheckByteLengthCall(m, BitwiseLeft(cond1), newBufferName))
        return false;

    ParseNode* maskNode = BitwiseRight(cond1);
    if (!IsLiteralInt(m, maskNode, mask))
        return m.fail(maskNode, "expecting integer literal mask");
    if ((*mask & 0xffffff) != 0xffffff)
        return m.fail(maskNode, "mask value must have the bits 0xffffff set");

    if (!cond2->isKind(PNK_LE))
        return m.fail(cond2, "expecting byteLength <= L");

    if (!CheckByteLengthCall(m, RelationalLeft(cond2), newBufferName))
        return false;

    ParseNode* minLengthNode = RelationalRight(cond2);
    uint32_t minLengthExclusive;
    if (!IsLiteralInt(m, minLengthNode, &minLengthExclusive))
        return m.fail(minLengthNode, "expecting integer literal");
    if (minLengthExclusive < 0xffffff || minLengthExclusive == UINT32_MAX)
        return m.fail(minLengthNode, "literal must be >= 0xffffff and < 0xffffffff");

    // Add one to convert from exclusive (the branch rejects if ==) to inclusive.
    *minLength = minLengthExclusive + 1;

    if (!cond3->isKind(PNK_GT))
        return m.fail(cond3, "expecting byteLength > M");

    if (!CheckByteLengthCall(m, RelationalLeft(cond3), newBufferName))
        return false;

    ParseNode* maxLengthNode = RelationalRight(cond3);
    if (!IsLiteralInt(m, maxLengthNode, maxLength))
        return m.fail(maxLengthNode, "expecting integer literal");
    if (*maxLength > 0x80000000)
        return m.fail(maxLengthNode, "literal must be <= 0x80000000");

    if (*maxLength < *minLength)
        return m.fail(maxLengthNode, "maximum length must be greater or equal to minimum length");

    return true;
}

static bool
CheckReturnBoolLiteral(ModuleCompiler& m, ParseNode* stmt, bool retval)
{
    if (!stmt)
        return m.fail(stmt, "expected return statement");

    if (stmt->isKind(PNK_STATEMENTLIST)) {
        stmt = SkipEmptyStatements(ListHead(stmt));
        if (!stmt || NextNonEmptyStatement(stmt))
            return m.fail(stmt, "expected single return statement");
    }

    if (!stmt->isKind(PNK_RETURN))
        return m.fail(stmt, "expected return statement");

    ParseNode* returnExpr = ReturnExpr(stmt);
    if (!returnExpr || !returnExpr->isKind(retval ? PNK_TRUE : PNK_FALSE))
        return m.failf(stmt, "expected 'return %s;'", retval ? "true" : "false");

    return true;
}

static bool
CheckReassignmentTo(ModuleCompiler& m, ParseNode* stmt, PropertyName* lhsName, ParseNode** rhs)
{
    if (!stmt || !stmt->isKind(PNK_SEMI))
        return m.fail(stmt, "missing reassignment");

    ParseNode* assign = UnaryKid(stmt);
    if (!assign || !assign->isKind(PNK_ASSIGN))
        return m.fail(stmt, "missing reassignment");

    ParseNode* lhs = BinaryLeft(assign);
    if (!IsUseOfName(lhs, lhsName))
        return m.failName(lhs, "expecting reassignment of %s", lhsName);

    *rhs = BinaryRight(assign);
    return true;
}

static bool
CheckChangeHeap(ModuleCompiler& m, ParseNode* fn, bool* validated)
{
    MOZ_ASSERT(fn->isKind(PNK_FUNCTION));

    // We don't yet know whether this is a change-heap function.
    // The point at which we know we have a change-heap function is once we see
    // whether the argument is coerced according to the normal asm.js rules. If
    // it is coerced, it's not change-heap and must validate according to normal
    // rules; otherwise it must validate as a change-heap function.
    *validated = false;

    PropertyName* changeHeapName = FunctionName(fn);
    if (!CheckModuleLevelName(m, fn, changeHeapName))
        return false;

    unsigned numFormals;
    ParseNode* arg = FunctionArgsList(fn, &numFormals);
    if (numFormals != 1)
        return true;

    PropertyName* newBufferName;
    if (!CheckArgument(m, arg, &newBufferName))
        return false;

    ParseNode* stmtIter = SkipEmptyStatements(ListHead(FunctionStatementList(fn)));
    if (!stmtIter || !stmtIter->isKind(PNK_IF))
        return true;

    // We can now issue validation failures if we see something that isn't a
    // valid change-heap function.
    *validated = true;

    PropertyName* bufferName = m.module().bufferArgumentName();
    if (!bufferName)
        return m.fail(fn, "to change heaps, the module must have a buffer argument");

    ParseNode* cond = TernaryKid1(stmtIter);
    ParseNode* thenStmt = TernaryKid2(stmtIter);
    if (ParseNode* elseStmt = TernaryKid3(stmtIter))
        return m.fail(elseStmt, "unexpected else statement");

    uint32_t mask, min = 0, max;  // initialize min to silence GCC warning
    if (!CheckHeapLengthCondition(m, cond, newBufferName, &mask, &min, &max))
        return false;

    if (!CheckReturnBoolLiteral(m, thenStmt, false))
        return false;

    stmtIter = NextNonEmptyStatement(stmtIter);

    for (unsigned i = 0; i < m.numArrayViews(); i++, stmtIter = NextNonEmptyStatement(stmtIter)) {
        const ModuleCompiler::ArrayView& view = m.arrayView(i);

        ParseNode* rhs;
        if (!CheckReassignmentTo(m, stmtIter, view.name, &rhs))
            return false;

        if (!rhs->isKind(PNK_NEW))
            return m.failName(rhs, "expecting assignment of new array view to %s", view.name);

        ParseNode* ctorExpr = ListHead(rhs);
        if (!ctorExpr->isKind(PNK_NAME))
            return m.fail(rhs, "expecting name of imported typed array constructor");

        const ModuleCompiler::Global* global = m.lookupGlobal(ctorExpr->name());
        if (!global || global->which() != ModuleCompiler::Global::ArrayViewCtor)
            return m.fail(rhs, "expecting name of imported typed array constructor");
        if (global->viewType() != view.type)
            return m.fail(rhs, "can't change the type of a global view variable");

        if (!CheckNewArrayViewArgs(m, ctorExpr, newBufferName))
            return false;
    }

    ParseNode* rhs;
    if (!CheckReassignmentTo(m, stmtIter, bufferName, &rhs))
        return false;
    if (!IsUseOfName(rhs, newBufferName))
        return m.failName(stmtIter, "expecting assignment of new buffer to %s", bufferName);

    stmtIter = NextNonEmptyStatement(stmtIter);

    if (!CheckReturnBoolLiteral(m, stmtIter, true))
        return false;

    stmtIter = NextNonEmptyStatement(stmtIter);
    if (stmtIter)
        return m.fail(stmtIter, "expecting end of function");

    return m.addChangeHeap(changeHeapName, fn, mask, min, max);
}

static bool
ParseFunction(ModuleCompiler& m, ParseNode** fnOut)
{
    TokenStream& tokenStream = m.tokenStream();

    tokenStream.consumeKnownToken(TOK_FUNCTION);

    RootedPropertyName name(m.cx());

    TokenKind tk;
    if (!tokenStream.getToken(&tk))
        return false;
    if (tk == TOK_NAME) {
        name = tokenStream.currentName();
    } else if (tk == TOK_YIELD) {
        if (!m.parser().checkYieldNameValidity())
            return false;
        name = m.cx()->names().yield;
    } else {
        return false;  // The regular parser will throw a SyntaxError, no need to m.fail.
    }

    ParseNode* fn = m.parser().handler.newFunctionDefinition();
    if (!fn)
        return false;

    // This flows into FunctionBox, so must be tenured.
    RootedFunction fun(m.cx(), NewFunction(m.cx(), NullPtr(), nullptr, 0, JSFunction::INTERPRETED,
                                           m.cx()->global(), name, JSFunction::FinalizeKind,
                                           TenuredObject));
    if (!fun)
        return false;

    AsmJSParseContext* outerpc = m.parser().pc;

    Directives directives(outerpc);
    FunctionBox* funbox = m.parser().newFunctionBox(fn, fun, outerpc, directives, NotGenerator);
    if (!funbox)
        return false;

    Directives newDirectives = directives;
    AsmJSParseContext funpc(&m.parser(), outerpc, fn, funbox, &newDirectives,
                            outerpc->staticLevel + 1, outerpc->blockidGen,
                            /* blockScopeDepth = */ 0);
    if (!funpc.init(tokenStream))
        return false;

    if (!m.parser().functionArgsAndBodyGeneric(fn, fun, Normal, Statement)) {
        if (tokenStream.hadError() || directives == newDirectives)
            return false;

        return m.fail(nullptr, "encountered new directive");
    }

    MOZ_ASSERT(!tokenStream.hadError());
    MOZ_ASSERT(directives == newDirectives);

    outerpc->blockidGen = funpc.blockidGen;
    fn->pn_blockid = outerpc->blockid();

    *fnOut = fn;
    return true;
}

static bool
CheckFunction(ModuleCompiler& m, LifoAlloc& lifo, MIRGenerator** mir, ModuleCompiler::Func** funcOut)
{
    int64_t before = PRMJ_Now();

    // asm.js modules can be quite large when represented as parse trees so pop
    // the backing LifoAlloc after parsing/compiling each function.
    AsmJSParser::Mark mark = m.parser().mark();

    ParseNode* fn = nullptr;  // initialize to silence GCC warning
    if (!ParseFunction(m, &fn))
        return false;

    if (!CheckFunctionHead(m, fn))
        return false;

    if (m.tryOnceToValidateChangeHeap()) {
        bool validated;
        if (!CheckChangeHeap(m, fn, &validated))
            return false;
        if (validated) {
            *mir = nullptr;
            return true;
        }
    }

    FunctionCompiler f(m, fn, lifo);
    if (!f.init())
        return false;

    ParseNode* stmtIter = ListHead(FunctionStatementList(fn));

    if (!CheckProcessingDirectives(m, &stmtIter))
        return false;

    VarTypeVector argTypes(m.lifo());
    if (!CheckArguments(f, &stmtIter, &argTypes))
        return false;

    if (!CheckVariables(f, &stmtIter))
        return false;

    if (!f.prepareToEmitMIR(argTypes))
        return false;

    ParseNode* lastNonEmptyStmt = nullptr;
    for (; stmtIter; stmtIter = NextNode(stmtIter)) {
        if (!CheckStatement(f, stmtIter))
            return false;
        if (!IsEmptyStatement(stmtIter))
            lastNonEmptyStmt = stmtIter;
    }

    if (!CheckFinalReturn(f, lastNonEmptyStmt))
        return false;

    Signature sig(Move(argTypes), f.returnedType());
    ModuleCompiler::Func* func = nullptr;
    if (!CheckFunctionSignature(m, fn, Move(sig), FunctionName(fn), &func))
        return false;

    if (func->defined())
        return m.failName(fn, "function '%s' already defined", FunctionName(fn));

    func->define(m, fn);
    func->accumulateCompileTime((PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC);

    m.parser().release(mark);

    *mir = f.extractMIR();
    (*mir)->initMinAsmJSHeapLength(m.minHeapLength());
    *funcOut = func;
    return true;
}

static bool
GenerateCode(ModuleCompiler& m, ModuleCompiler::Func& func, MIRGenerator& mir, LIRGraph& lir)
{
    int64_t before = PRMJ_Now();

    // A single MacroAssembler is reused for all function compilations so
    // that there is a single linear code segment for each module. To avoid
    // spiking memory, a LifoAllocScope in the caller frees all MIR/LIR
    // after each function is compiled. This method is responsible for cleaning
    // out any dangling pointers that the MacroAssembler may have kept.
    m.masm().resetForNewCodeGenerator(mir.alloc());

    ScopedJSDeletePtr<CodeGenerator> codegen(js_new<CodeGenerator>(&mir, &lir, &m.masm()));
    if (!codegen)
        return false;

    AsmJSFunctionLabels labels(func.entry(), m.stackOverflowLabel());
    if (!codegen->generateAsmJS(&labels))
        return false;

    func.accumulateCompileTime((PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC);

    if (!m.finishGeneratingFunction(func, *codegen, labels))
        return false;

    // Unlike regular IonMonkey, which links and generates a new JitCode for
    // every function, we accumulate all the functions in the module in a
    // single MacroAssembler and link at end. Linking asm.js doesn't require a
    // CodeGenerator so we can destroy it now (via ScopedJSDeletePtr).
    return true;
}

static bool
CheckAllFunctionsDefined(ModuleCompiler& m)
{
    for (unsigned i = 0; i < m.numFunctions(); i++) {
        if (!m.function(i).entry().bound())
            return m.failName(nullptr, "missing definition of function %s", m.function(i).name());
    }

    return true;
}

static bool
CheckFunctionsSequential(ModuleCompiler& m)
{
    // Use a single LifoAlloc to allocate all the temporary compiler IR.
    // All allocated LifoAlloc'd memory is released after compiling each
    // function by the LifoAllocScope inside the loop.
    LifoAlloc lifo(LIFO_ALLOC_PRIMARY_CHUNK_SIZE);

    while (true) {
        TokenKind tk;
        if (!PeekToken(m.parser(), &tk))
            return false;
        if (tk != TOK_FUNCTION)
            break;

        LifoAllocScope scope(&lifo);

        MIRGenerator* mir;
        ModuleCompiler::Func* func;
        if (!CheckFunction(m, lifo, &mir, &func))
            return false;

        // In the case of the change-heap function, no MIR is produced.
        if (!mir)
            continue;

        int64_t before = PRMJ_Now();

        JitContext jcx(m.cx(), &mir->alloc());

        IonSpewNewFunction(&mir->graph(), NullPtr());

        if (!OptimizeMIR(mir))
            return m.failOffset(func->srcBegin(), "internal compiler failure (probably out of memory)");

        LIRGraph* lir = GenerateLIR(mir);
        if (!lir)
            return m.failOffset(func->srcBegin(), "internal compiler failure (probably out of memory)");

        func->accumulateCompileTime((PRMJ_Now() - before) / PRMJ_USEC_PER_MSEC);

        if (!GenerateCode(m, *func, *mir, *lir))
            return false;

        IonSpewEndFunction();
    }

    if (!CheckAllFunctionsDefined(m))
        return false;

    return true;
}

// Currently, only one asm.js parallel compilation is allowed at a time.
// This RAII class attempts to claim this parallel compilation using atomic ops
// on the helper thread state's asmJSCompilationInProgress.
class ParallelCompilationGuard
{
    bool parallelState_;
  public:
    ParallelCompilationGuard() : parallelState_(false) {}
    ~ParallelCompilationGuard() {
        if (parallelState_) {
            MOZ_ASSERT(HelperThreadState().asmJSCompilationInProgress == true);
            HelperThreadState().asmJSCompilationInProgress = false;
        }
    }
    bool claim() {
        MOZ_ASSERT(!parallelState_);
        if (!HelperThreadState().asmJSCompilationInProgress.compareExchange(false, true))
            return false;
        parallelState_ = true;
        return true;
    }
};

static bool
ParallelCompilationEnabled(ExclusiveContext* cx)
{
    // If 'cx' isn't a JSContext, then we are already off the main thread so
    // off-thread compilation must be enabled. However, since there are a fixed
    // number of helper threads and one is already being consumed by this
    // parsing task, ensure that there another free thread to avoid deadlock.
    // (Note: there is at most one thread used for parsing so we don't have to
    // worry about general dining philosophers.)
    if (HelperThreadState().threadCount <= 1 || !CanUseExtraThreads())
        return false;

    if (!cx->isJSContext())
        return true;
    return cx->asJSContext()->runtime()->canUseOffthreadIonCompilation();
}

// State of compilation as tracked and updated by the main thread.
struct ParallelGroupState
{
    Vector<AsmJSParallelTask>& tasks;
    int32_t outstandingJobs; // Good work, jobs!
    uint32_t compiledJobs;

    explicit ParallelGroupState(Vector<AsmJSParallelTask>& tasks)
      : tasks(tasks), outstandingJobs(0), compiledJobs(0)
    { }
};

// Block until a helper-assigned LifoAlloc becomes finished.
static AsmJSParallelTask*
GetFinishedCompilation(ModuleCompiler& m, ParallelGroupState& group)
{
    AutoLockHelperThreadState lock;

    while (!HelperThreadState().asmJSFailed()) {
        if (!HelperThreadState().asmJSFinishedList().empty()) {
            group.outstandingJobs--;
            return HelperThreadState().asmJSFinishedList().popCopy();
        }
        HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);
    }

    return nullptr;
}

static bool
GetUsedTask(ModuleCompiler& m, ParallelGroupState& group, AsmJSParallelTask** outTask)
{
    // Block until a used LifoAlloc becomes available.
    AsmJSParallelTask* task = GetFinishedCompilation(m, group);
    if (!task)
        return false;

    ModuleCompiler::Func& func = *reinterpret_cast<ModuleCompiler::Func*>(task->func);
    func.accumulateCompileTime(task->compileTime);

    {
        // Perform code generation on the main thread.
        JitContext jitContext(m.cx(), &task->mir->alloc());
        if (!GenerateCode(m, func, *task->mir, *task->lir))
            return false;
    }

    group.compiledJobs++;

    // Clear the LifoAlloc for use by another helper.
    TempAllocator& tempAlloc = task->mir->alloc();
    tempAlloc.TempAllocator::~TempAllocator();
    task->lifo.releaseAll();

    *outTask = task;
    return true;
}

static inline bool
GetUnusedTask(ParallelGroupState& group, uint32_t i, AsmJSParallelTask** outTask)
{
    // Since functions are dispatched in order, if fewer than |numLifos| functions
    // have been generated, then the |i'th| LifoAlloc must never have been
    // assigned to a helper thread.
    if (i >= group.tasks.length())
        return false;
    *outTask = &group.tasks[i];
    return true;
}

static bool
CheckFunctionsParallel(ModuleCompiler& m, ParallelGroupState& group)
{
#ifdef DEBUG
    {
        AutoLockHelperThreadState lock;
        MOZ_ASSERT(HelperThreadState().asmJSWorklist().empty());
        MOZ_ASSERT(HelperThreadState().asmJSFinishedList().empty());
    }
#endif
    HelperThreadState().resetAsmJSFailureState();

    AsmJSParallelTask* task = nullptr;
    for (unsigned i = 0;; i++) {
        TokenKind tk;
        if (!PeekToken(m.parser(), &tk))
            return false;
        if (tk != TOK_FUNCTION)
            break;

        if (!task && !GetUnusedTask(group, i, &task) && !GetUsedTask(m, group, &task))
            return false;

        // Generate MIR into the LifoAlloc on the main thread.
        MIRGenerator* mir;
        ModuleCompiler::Func* func;
        if (!CheckFunction(m, task->lifo, &mir, &func))
            return false;

        // In the case of the change-heap function, no MIR is produced.
        if (!mir)
            continue;

        // Perform optimizations and LIR generation on a helper thread.
        task->init(m.cx()->compartment()->runtimeFromAnyThread(), func, mir);
        if (!StartOffThreadAsmJSCompile(m.cx(), task))
            return false;

        group.outstandingJobs++;
        task = nullptr;
    }

    // Block for all outstanding helpers to complete.
    while (group.outstandingJobs > 0) {
        AsmJSParallelTask* ignored = nullptr;
        if (!GetUsedTask(m, group, &ignored))
            return false;
    }

    if (!CheckAllFunctionsDefined(m))
        return false;

    MOZ_ASSERT(group.outstandingJobs == 0);
    MOZ_ASSERT(group.compiledJobs == m.numFunctions());
#ifdef DEBUG
    {
        AutoLockHelperThreadState lock;
        MOZ_ASSERT(HelperThreadState().asmJSWorklist().empty());
        MOZ_ASSERT(HelperThreadState().asmJSFinishedList().empty());
    }
#endif
    MOZ_ASSERT(!HelperThreadState().asmJSFailed());
    return true;
}

static void
CancelOutstandingJobs(ModuleCompiler& m, ParallelGroupState& group)
{
    // This is failure-handling code, so it's not allowed to fail. The problem
    // is that all memory for compilation is stored in LifoAllocs maintained in
    // the scope of CheckFunctions() -- so in order for that function to safely
    // return, and thereby remove the LifoAllocs, none of that memory can be in
    // use or reachable by helpers.

    MOZ_ASSERT(group.outstandingJobs >= 0);
    if (!group.outstandingJobs)
        return;

    AutoLockHelperThreadState lock;

    // From the compiling tasks, eliminate those waiting for helper assignation.
    group.outstandingJobs -= HelperThreadState().asmJSWorklist().length();
    HelperThreadState().asmJSWorklist().clear();

    // From the compiling tasks, eliminate those waiting for codegen.
    group.outstandingJobs -= HelperThreadState().asmJSFinishedList().length();
    HelperThreadState().asmJSFinishedList().clear();

    // Eliminate tasks that failed without adding to the finished list.
    group.outstandingJobs -= HelperThreadState().harvestFailedAsmJSJobs();

    // Any remaining tasks are therefore undergoing active compilation.
    MOZ_ASSERT(group.outstandingJobs >= 0);
    while (group.outstandingJobs > 0) {
        HelperThreadState().wait(GlobalHelperThreadState::CONSUMER);

        group.outstandingJobs -= HelperThreadState().harvestFailedAsmJSJobs();
        group.outstandingJobs -= HelperThreadState().asmJSFinishedList().length();
        HelperThreadState().asmJSFinishedList().clear();
    }

    MOZ_ASSERT(group.outstandingJobs == 0);
    MOZ_ASSERT(HelperThreadState().asmJSWorklist().empty());
    MOZ_ASSERT(HelperThreadState().asmJSFinishedList().empty());
}

static const size_t LIFO_ALLOC_PARALLEL_CHUNK_SIZE = 1 << 12;

static bool
CheckFunctions(ModuleCompiler& m)
{
    // If parallel compilation isn't enabled (not enough cores, disabled by
    // pref, etc) or another thread is currently compiling asm.js in parallel,
    // fall back to sequential compilation. (We could lift the latter
    // constraint by hoisting asmJS* state out of HelperThreadState so multiple
    // concurrent asm.js parallel compilations don't race.)
    ParallelCompilationGuard g;
    if (!ParallelCompilationEnabled(m.cx()) || !g.claim())
        return CheckFunctionsSequential(m);

    JitSpew(JitSpew_IonLogs, "Can't log asm.js script. (Compiled on background thread.)");

    // Saturate all helper threads.
    size_t numParallelJobs = HelperThreadState().maxAsmJSCompilationThreads();

    // Allocate scoped AsmJSParallelTask objects. Each contains a unique
    // LifoAlloc that provides all necessary memory for compilation.
    Vector<AsmJSParallelTask, 0> tasks(m.cx());
    if (!tasks.initCapacity(numParallelJobs))
        return false;

    for (size_t i = 0; i < numParallelJobs; i++)
        tasks.infallibleAppend(LIFO_ALLOC_PARALLEL_CHUNK_SIZE);

    // With compilation memory in-scope, dispatch helper threads.
    ParallelGroupState group(tasks);
    if (!CheckFunctionsParallel(m, group)) {
        CancelOutstandingJobs(m, group);

        // If failure was triggered by a helper thread, report error.
        if (void* maybeFunc = HelperThreadState().maybeAsmJSFailedFunction()) {
            ModuleCompiler::Func* func = reinterpret_cast<ModuleCompiler::Func*>(maybeFunc);
            return m.failOffset(func->srcBegin(), "allocation failure during compilation");
        }

        // Otherwise, the error occurred on the main thread and was already reported.
        return false;
    }
    return true;
}

static bool
CheckFuncPtrTable(ModuleCompiler& m, ParseNode* var)
{
    if (!IsDefinition(var))
        return m.fail(var, "function-pointer table name must be unique");

    ParseNode* arrayLiteral = MaybeDefinitionInitializer(var);
    if (!arrayLiteral || !arrayLiteral->isKind(PNK_ARRAY))
        return m.fail(var, "function-pointer table's initializer must be an array literal");

    unsigned length = ListLength(arrayLiteral);

    if (!IsPowerOfTwo(length))
        return m.failf(arrayLiteral, "function-pointer table length must be a power of 2 (is %u)", length);

    unsigned mask = length - 1;

    ModuleCompiler::FuncPtrVector elems(m.cx());
    const Signature* firstSig = nullptr;

    for (ParseNode* elem = ListHead(arrayLiteral); elem; elem = NextNode(elem)) {
        if (!elem->isKind(PNK_NAME))
            return m.fail(elem, "function-pointer table's elements must be names of functions");

        PropertyName* funcName = elem->name();
        const ModuleCompiler::Func* func = m.lookupFunction(funcName);
        if (!func)
            return m.fail(elem, "function-pointer table's elements must be names of functions");

        if (firstSig) {
            if (*firstSig != func->sig())
                return m.fail(elem, "all functions in table must have same signature");
        } else {
            firstSig = &func->sig();
        }

        if (!elems.append(func))
            return false;
    }

    Signature sig(m.lifo());
    if (!sig.copy(*firstSig))
        return false;

    ModuleCompiler::FuncPtrTable* table;
    if (!CheckFuncPtrTableAgainstExisting(m, var, var->name(), Move(sig), mask, &table))
        return false;

    table->initElems(Move(elems));
    return true;
}

static bool
CheckFuncPtrTables(ModuleCompiler& m)
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
        if (!m.funcPtrTable(i).initialized())
            return m.fail(nullptr, "expecting function-pointer table");
    }

    return true;
}

static bool
CheckModuleExportFunction(ModuleCompiler& m, ParseNode* pn, PropertyName* maybeFieldName = nullptr)
{
    if (!pn->isKind(PNK_NAME))
        return m.fail(pn, "expected name of exported function");

    PropertyName* funcName = pn->name();
    const ModuleCompiler::Global* global = m.lookupGlobal(funcName);
    if (!global)
        return m.failName(pn, "exported function name '%s' not found", funcName);

    if (global->which() == ModuleCompiler::Global::Function)
        return m.addExportedFunction(m.function(global->funcIndex()), maybeFieldName);

    if (global->which() == ModuleCompiler::Global::ChangeHeap)
        return m.addExportedChangeHeap(funcName, *global, maybeFieldName);

    return m.failName(pn, "'%s' is not a function", funcName);
}

static bool
CheckModuleExportObject(ModuleCompiler& m, ParseNode* object)
{
    MOZ_ASSERT(object->isKind(PNK_OBJECT));

    for (ParseNode* pn = ListHead(object); pn; pn = NextNode(pn)) {
        if (!IsNormalObjectField(m.cx(), pn))
            return m.fail(pn, "only normal object properties may be used in the export object literal");

        PropertyName* fieldName = ObjectNormalFieldName(m.cx(), pn);

        ParseNode* initNode = ObjectNormalFieldInitializer(m.cx(), pn);
        if (!initNode->isKind(PNK_NAME))
            return m.fail(initNode, "initializer of exported object literal must be name of function");

        if (!CheckModuleExportFunction(m, initNode, fieldName))
            return false;
    }

    return true;
}

static bool
CheckModuleReturn(ModuleCompiler& m)
{
    TokenKind tk;
    if (!PeekToken(m.parser(), &tk))
        return false;
    if (tk != TOK_RETURN) {
        if (tk == TOK_RC || tk == TOK_EOF)
            return m.fail(nullptr, "expecting return statement");
        return m.fail(nullptr, "invalid asm.js statement");
    }

    ParseNode* returnStmt = m.parser().statement();
    if (!returnStmt)
        return false;

    ParseNode* returnExpr = ReturnExpr(returnStmt);
    if (!returnExpr)
        return m.fail(returnStmt, "export statement must return something");

    if (returnExpr->isKind(PNK_OBJECT)) {
        if (!CheckModuleExportObject(m, returnExpr))
            return false;
    } else {
        if (!CheckModuleExportFunction(m, returnExpr))
            return false;
    }

    // Function statements are not added to the lexical scope in ParseContext
    // (since cx->tempLifoAlloc is marked/released after each function
    // statement) and thus all the identifiers in the return statement will be
    // mistaken as free variables and added to lexdeps. Clear these now.
    m.parser().pc->lexdeps->clear();
    return true;
}

static void
AssertStackAlignment(MacroAssembler& masm, uint32_t alignment, uint32_t addBeforeAssert = 0)
{
    MOZ_ASSERT((sizeof(AsmJSFrame) + masm.framePushed() + addBeforeAssert) % alignment == 0);
    masm.assertStackAlignment(alignment, addBeforeAssert);
}

static unsigned
StackDecrementForCall(MacroAssembler& masm, uint32_t alignment, unsigned bytesToPush)
{
    return StackDecrementForCall(alignment, sizeof(AsmJSFrame) + masm.framePushed(), bytesToPush);
}

template <class VectorT>
static unsigned
StackArgBytes(const VectorT& argTypes)
{
    ABIArgIter<VectorT> iter(argTypes);
    while (!iter.done())
        iter++;
    return iter.stackBytesConsumedSoFar();
}

template <class VectorT>
static unsigned
StackDecrementForCall(MacroAssembler& masm, uint32_t alignment, const VectorT& argTypes,
                      unsigned extraBytes = 0)
{
    return StackDecrementForCall(masm, alignment, StackArgBytes(argTypes) + extraBytes);
}

#if defined(JS_CODEGEN_ARM)
// The ARM system ABI also includes d15 in the non volatile float registers.
// Also exclude lr (a.k.a. r14) as we preserve it manually)
static const RegisterSet NonVolatileRegs =
    RegisterSet(GeneralRegisterSet(Registers::NonVolatileMask&
                                   ~(uint32_t(1) << Registers::lr)),
                FloatRegisterSet(FloatRegisters::NonVolatileMask | (1ULL << FloatRegisters::d15)));
#else
static const RegisterSet NonVolatileRegs =
    RegisterSet(GeneralRegisterSet(Registers::NonVolatileMask),
                FloatRegisterSet(FloatRegisters::NonVolatileMask));
#endif
static const FloatRegisterSet NonVolatileSimdRegs = SupportsSimd ? NonVolatileRegs.fpus()
                                                                 : FloatRegisterSet();

#if defined(JS_CODEGEN_MIPS)
// Mips is using one more double slot due to stack alignment for double values.
// Look at MacroAssembler::PushRegsInMask(RegisterSet set)
static const unsigned FramePushedAfterSave = NonVolatileRegs.gprs().size() * sizeof(intptr_t) +
                                             NonVolatileRegs.fpus().getPushSizeInBytes() +
                                             sizeof(double);
#else
static const unsigned FramePushedAfterSave =
   SupportsSimd ? NonVolatileRegs.gprs().size() * sizeof(intptr_t) +
                  NonVolatileRegs.fpus().size() * Simd128DataSize
                : NonVolatileRegs.gprs().size() * sizeof(intptr_t) +
                  NonVolatileRegs.fpus().getPushSizeInBytes();
#endif
static const unsigned FramePushedForEntrySP = FramePushedAfterSave + sizeof(void*);

static bool
GenerateEntry(ModuleCompiler& m, unsigned exportIndex)
{
    MacroAssembler& masm = m.masm();

    Label begin;
    masm.align(CodeAlignment);
    masm.bind(&begin);

    // Save the return address if it wasn't already saved by the call insn.
#if defined(JS_CODEGEN_ARM)
    masm.push(lr);
#elif defined(JS_CODEGEN_MIPS)
    masm.push(ra);
#elif defined(JS_CODEGEN_X86)
    static const unsigned EntryFrameSize = sizeof(void*);
#endif

    // Save all caller non-volatile registers before we clobber them here and in
    // the asm.js callee (which does not preserve non-volatile registers).
    masm.setFramePushed(0);
    masm.PushRegsInMask(NonVolatileRegs, NonVolatileSimdRegs);
    MOZ_ASSERT(masm.framePushed() == FramePushedAfterSave);

    // ARM and MIPS have a globally-pinned GlobalReg (x64 uses RIP-relative
    // addressing, x86 uses immediates in effective addresses). For the
    // AsmJSGlobalRegBias addition, see Assembler-(mips,arm).h.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    masm.movePtr(IntArgReg1, GlobalReg);
    masm.addPtr(Imm32(AsmJSGlobalRegBias), GlobalReg);
#endif

    // ARM, MIPS and x64 have a globally-pinned HeapReg (x86 uses immediates in
    // effective addresses). Loading the heap register depends on the global
    // register already having been loaded.
    masm.loadAsmJSHeapRegisterFromGlobalData();

    // Put the 'argv' argument into a non-argument/return register so that we
    // can use 'argv' while we fill in the arguments for the asm.js callee.
    // Also, save 'argv' on the stack so that we can recover it after the call.
    // Use a second non-argument/return register as temporary scratch.
    Register argv = ABIArgGenerator::NonArgReturnReg0;
    Register scratch = ABIArgGenerator::NonArgReturnReg1;
#if defined(JS_CODEGEN_X86)
    masm.loadPtr(Address(StackPointer, EntryFrameSize + masm.framePushed()), argv);
#else
    masm.movePtr(IntArgReg0, argv);
#endif
    masm.Push(argv);

    // Save the stack pointer to the saved non-volatile registers. We will use
    // this on two paths: normal return and exceptional return. Since
    // loadAsmJSActivation uses GlobalReg, we must do this after loading
    // GlobalReg.
    MOZ_ASSERT(masm.framePushed() == FramePushedForEntrySP);
    masm.loadAsmJSActivation(scratch);
    masm.storePtr(StackPointer, Address(scratch, AsmJSActivation::offsetOfEntrySP()));

    // Dynamically align the stack since ABIStackAlignment is not necessarily
    // AsmJSStackAlignment. We'll use entrySP to recover the original stack
    // pointer on return.
    masm.andPtr(Imm32(~(AsmJSStackAlignment - 1)), StackPointer);

    // Bump the stack for the call.
    PropertyName* funcName = m.module().exportedFunction(exportIndex).name();
    const ModuleCompiler::Func& func = *m.lookupFunction(funcName);
    masm.reserveStack(AlignBytes(StackArgBytes(func.sig().args()), AsmJSStackAlignment));

    // Copy parameters out of argv and into the registers/stack-slots specified by
    // the system ABI.
    for (ABIArgTypeIter iter(func.sig().args()); !iter.done(); iter++) {
        unsigned argOffset = iter.index() * sizeof(AsmJSModule::EntryArg);
        Address src(argv, argOffset);
        MIRType type = iter.mirType();
        switch (iter->kind()) {
          case ABIArg::GPR:
            masm.load32(src, iter->gpr());
            break;
          case ABIArg::FPU: {
            static_assert(sizeof(AsmJSModule::EntryArg) >= jit::Simd128DataSize,
                          "EntryArg must be big enough to store SIMD values");
            switch (type) {
              case MIRType_Int32x4:
                masm.loadUnalignedInt32x4(src, iter->fpu());
                break;
              case MIRType_Float32x4:
                masm.loadUnalignedFloat32x4(src, iter->fpu());
                break;
              case MIRType_Double:
                masm.loadDouble(src, iter->fpu());
                break;
              case MIRType_Float32:
                masm.loadFloat32(src, iter->fpu());
                break;
              default:
                MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected FPU type");
                break;
            }
            break;
          }
          case ABIArg::Stack:
            switch (type) {
              case MIRType_Int32:
                masm.load32(src, scratch);
                masm.storePtr(scratch, Address(StackPointer, iter->offsetFromArgBase()));
                break;
              case MIRType_Double:
                masm.loadDouble(src, ScratchDoubleReg);
                masm.storeDouble(ScratchDoubleReg, Address(StackPointer, iter->offsetFromArgBase()));
                break;
              case MIRType_Float32:
                masm.loadFloat32(src, ScratchFloat32Reg);
                masm.storeFloat32(ScratchFloat32Reg, Address(StackPointer, iter->offsetFromArgBase()));
                break;
              case MIRType_Int32x4:
                masm.loadUnalignedInt32x4(src, ScratchSimdReg);
                masm.storeAlignedInt32x4(ScratchSimdReg,
                                         Address(StackPointer, iter->offsetFromArgBase()));
                break;
              case MIRType_Float32x4:
                masm.loadUnalignedFloat32x4(src, ScratchSimdReg);
                masm.storeAlignedFloat32x4(ScratchSimdReg,
                                           Address(StackPointer, iter->offsetFromArgBase()));
                break;
              default:
                MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected stack arg type");
            }
            break;
        }
    }

    // Call into the real function.
    masm.assertStackAlignment(AsmJSStackAlignment);
    masm.call(CallSiteDesc(CallSiteDesc::Relative), &func.entry());

    // Recover the stack pointer value before dynamic alignment.
    masm.loadAsmJSActivation(scratch);
    masm.loadPtr(Address(scratch, AsmJSActivation::offsetOfEntrySP()), StackPointer);
    masm.setFramePushed(FramePushedForEntrySP);

    // Recover the 'argv' pointer which was saved before aligning the stack.
    masm.Pop(argv);

    // Store the return value in argv[0]
    switch (func.sig().retType().which()) {
      case RetType::Void:
        break;
      case RetType::Signed:
        masm.storeValue(JSVAL_TYPE_INT32, ReturnReg, Address(argv, 0));
        break;
      case RetType::Float:
        masm.convertFloat32ToDouble(ReturnFloat32Reg, ReturnDoubleReg);
        // Fall through as ReturnDoubleReg now contains a Double
      case RetType::Double:
        masm.canonicalizeDouble(ReturnDoubleReg);
        masm.storeDouble(ReturnDoubleReg, Address(argv, 0));
        break;
      case RetType::Int32x4:
        // We don't have control on argv alignment, do an unaligned access.
        masm.storeUnalignedInt32x4(ReturnSimdReg, Address(argv, 0));
        break;
      case RetType::Float32x4:
        // We don't have control on argv alignment, do an unaligned access.
        masm.storeUnalignedFloat32x4(ReturnSimdReg, Address(argv, 0));
        break;
    }

    // Restore clobbered non-volatile registers of the caller.
    masm.PopRegsInMask(NonVolatileRegs, NonVolatileSimdRegs);
    MOZ_ASSERT(masm.framePushed() == 0);

    masm.move32(Imm32(true), ReturnReg);
    masm.ret();

    return m.finishGeneratingEntry(exportIndex, &begin) && !masm.oom();
}

static void
FillArgumentArray(ModuleCompiler& m, const VarTypeVector& argTypes,
                  unsigned offsetToArgs, unsigned offsetToCallerStackArgs,
                  Register scratch)
{
    MacroAssembler& masm = m.masm();

    for (ABIArgTypeIter i(argTypes); !i.done(); i++) {
        Address dstAddr(StackPointer, offsetToArgs + i.index() * sizeof(Value));
        switch (i->kind()) {
          case ABIArg::GPR:
            masm.storeValue(JSVAL_TYPE_INT32, i->gpr(), dstAddr);
            break;
          case ABIArg::FPU:
            masm.canonicalizeDouble(i->fpu());
            masm.storeDouble(i->fpu(), dstAddr);
            break;
          case ABIArg::Stack:
            if (i.mirType() == MIRType_Int32) {
                Address src(StackPointer, offsetToCallerStackArgs + i->offsetFromArgBase());
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
                masm.load32(src, scratch);
                masm.storeValue(JSVAL_TYPE_INT32, scratch, dstAddr);
#else
                masm.memIntToValue(src, dstAddr);
#endif
            } else {
                MOZ_ASSERT(i.mirType() == MIRType_Double);
                Address src(StackPointer, offsetToCallerStackArgs + i->offsetFromArgBase());
                masm.loadDouble(src, ScratchDoubleReg);
                masm.canonicalizeDouble(ScratchDoubleReg);
                masm.storeDouble(ScratchDoubleReg, dstAddr);
            }
            break;
        }
    }
}

// If an FFI detaches its heap (viz., via ArrayBuffer.transfer), it must
// call change-heap to another heap (viz., the new heap returned by transfer)
// before returning to asm.js code. If the application fails to do this (if the
// heap pointer is null), jump to a stub.
static void
GenerateCheckForHeapDetachment(ModuleCompiler& m, Register scratch)
{
    if (!m.module().hasArrayView())
        return;

    MacroAssembler& masm = m.masm();
    MOZ_ASSERT(int(masm.framePushed()) >= int(ShadowStackSpace));
    AssertStackAlignment(masm, ABIStackAlignment);
#if defined(JS_CODEGEN_X86)
    CodeOffsetLabel label = masm.movlWithPatch(PatchedAbsoluteAddress(), scratch);
    masm.append(AsmJSGlobalAccess(label, AsmJSHeapGlobalDataOffset));
    masm.branchTestPtr(Assembler::Zero, scratch, scratch, &m.onDetachedLabel());
#else
    masm.branchTestPtr(Assembler::Zero, HeapReg, HeapReg, &m.onDetachedLabel());
#endif
}

static bool
GenerateFFIInterpExit(ModuleCompiler& m, const ModuleCompiler::ExitDescriptor& exit,
                      unsigned exitIndex, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    MOZ_ASSERT(masm.framePushed() == 0);

    // Argument types for InvokeFromAsmJS_*:
    static const MIRType typeArray[] = { MIRType_Pointer,   // exitDatum
                                         MIRType_Int32,     // argc
                                         MIRType_Pointer }; // argv
    MIRTypeVector invokeArgTypes(m.cx());
    if (!invokeArgTypes.append(typeArray, ArrayLength(typeArray)))
        return false;

    // At the point of the call, the stack layout shall be (sp grows to the left):
    //   | stack args | padding | Value argv[] | padding | retaddr | caller stack args |
    // The padding between stack args and argv ensures that argv is aligned. The
    // padding between argv and retaddr ensures that sp is aligned.
    unsigned offsetToArgv = AlignBytes(StackArgBytes(invokeArgTypes), sizeof(double));
    unsigned argvBytes = Max<size_t>(1, exit.sig().args().length()) * sizeof(Value);
    unsigned framePushed = StackDecrementForCall(masm, ABIStackAlignment, offsetToArgv + argvBytes);

    Label begin;
    GenerateAsmJSExitPrologue(masm, framePushed, AsmJSExit::SlowFFI, &begin);

    // Fill the argument array.
    unsigned offsetToCallerStackArgs = sizeof(AsmJSFrame) + masm.framePushed();
    Register scratch = ABIArgGenerator::NonArgReturnReg0;
    FillArgumentArray(m, exit.sig().args(), offsetToArgv, offsetToCallerStackArgs, scratch);

    // Prepare the arguments for the call to InvokeFromAsmJS_*.
    ABIArgMIRTypeIter i(invokeArgTypes);

    // argument 0: exitIndex
    if (i->kind() == ABIArg::GPR)
        masm.mov(ImmWord(exitIndex), i->gpr());
    else
        masm.store32(Imm32(exitIndex), Address(StackPointer, i->offsetFromArgBase()));
    i++;

    // argument 1: argc
    unsigned argc = exit.sig().args().length();
    if (i->kind() == ABIArg::GPR)
        masm.mov(ImmWord(argc), i->gpr());
    else
        masm.store32(Imm32(argc), Address(StackPointer, i->offsetFromArgBase()));
    i++;

    // argument 2: argv
    Address argv(StackPointer, offsetToArgv);
    if (i->kind() == ABIArg::GPR) {
        masm.computeEffectiveAddress(argv, i->gpr());
    } else {
        masm.computeEffectiveAddress(argv, scratch);
        masm.storePtr(scratch, Address(StackPointer, i->offsetFromArgBase()));
    }
    i++;
    MOZ_ASSERT(i.done());

    // Make the call, test whether it succeeded, and extract the return value.
    AssertStackAlignment(masm, ABIStackAlignment);
    switch (exit.sig().retType().which()) {
      case RetType::Void:
        masm.call(AsmJSImmPtr(AsmJSImm_InvokeFromAsmJS_Ignore));
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
        break;
      case RetType::Signed:
        masm.call(AsmJSImmPtr(AsmJSImm_InvokeFromAsmJS_ToInt32));
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
        masm.unboxInt32(argv, ReturnReg);
        break;
      case RetType::Double:
        masm.call(AsmJSImmPtr(AsmJSImm_InvokeFromAsmJS_ToNumber));
        masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
        masm.loadDouble(argv, ReturnDoubleReg);
        break;
      case RetType::Float:
        MOZ_CRASH("Float32 shouldn't be returned from a FFI");
      case RetType::Int32x4:
      case RetType::Float32x4:
        MOZ_CRASH("SIMD types shouldn't be returned from a FFI");
    }

    // The heap pointer may have changed during the FFI, so reload it and test
    // for detachment.
    masm.loadAsmJSHeapRegisterFromGlobalData();
    GenerateCheckForHeapDetachment(m, ABIArgGenerator::NonReturn_VolatileReg0);

    Label profilingReturn;
    GenerateAsmJSExitEpilogue(masm, framePushed, AsmJSExit::SlowFFI, &profilingReturn);
    return m.finishGeneratingInterpExit(exitIndex, &begin, &profilingReturn) && !masm.oom();
}

#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
static const unsigned MaybeSavedGlobalReg = sizeof(void*);
#else
static const unsigned MaybeSavedGlobalReg = 0;
#endif

static bool
GenerateFFIIonExit(ModuleCompiler& m, const ModuleCompiler::ExitDescriptor& exit,
                   unsigned exitIndex, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    MOZ_ASSERT(masm.framePushed() == 0);

    // Ion calls use the following stack layout (sp grows to the left):
    //   | retaddr | descriptor | callee | argc | this | arg1..N |
    // After the Ion frame, the global register (if present) is saved since Ion
    // does not preserve non-volatile regs. Also, unlike most ABIs, Ion requires
    // that sp be JitStackAlignment-aligned *after* pushing the return address.
    static_assert(AsmJSStackAlignment >= JitStackAlignment, "subsumes");
    unsigned sizeOfRetAddr = sizeof(void*);
    unsigned ionFrameBytes = 3 * sizeof(void*) + (1 + exit.sig().args().length()) * sizeof(Value);
    unsigned totalIonBytes = sizeOfRetAddr + ionFrameBytes + MaybeSavedGlobalReg;
    unsigned ionFramePushed = StackDecrementForCall(masm, JitStackAlignment, totalIonBytes) -
                              sizeOfRetAddr;

    Label begin;
    GenerateAsmJSExitPrologue(masm, ionFramePushed, AsmJSExit::JitFFI, &begin);

    // 1. Descriptor
    size_t argOffset = 0;
    uint32_t descriptor = MakeFrameDescriptor(ionFramePushed, JitFrame_Entry);
    masm.storePtr(ImmWord(uintptr_t(descriptor)), Address(StackPointer, argOffset));
    argOffset += sizeof(size_t);

    // 2. Callee
    Register callee = ABIArgGenerator::NonArgReturnReg0;   // live until call
    Register scratch = ABIArgGenerator::NonArgReturnReg1;  // repeatedly clobbered

    // 2.1. Get ExitDatum
    unsigned globalDataOffset = m.module().exitIndexToGlobalDataOffset(exitIndex);
#if defined(JS_CODEGEN_X64)
    m.masm().append(AsmJSGlobalAccess(masm.leaRipRelative(callee), globalDataOffset));
#elif defined(JS_CODEGEN_X86)
    m.masm().append(AsmJSGlobalAccess(masm.movlWithPatch(Imm32(0), callee), globalDataOffset));
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    masm.computeEffectiveAddress(Address(GlobalReg, globalDataOffset - AsmJSGlobalRegBias), callee);
#endif

    // 2.2. Get callee
    masm.loadPtr(Address(callee, offsetof(AsmJSModule::ExitDatum, fun)), callee);

    // 2.3. Save callee
    masm.storePtr(callee, Address(StackPointer, argOffset));
    argOffset += sizeof(size_t);

    // 2.4. Load callee executable entry point
    masm.loadPtr(Address(callee, JSFunction::offsetOfNativeOrScript()), callee);
    masm.loadBaselineOrIonNoArgCheck(callee, callee, nullptr);

    // 3. Argc
    unsigned argc = exit.sig().args().length();
    masm.storePtr(ImmWord(uintptr_t(argc)), Address(StackPointer, argOffset));
    argOffset += sizeof(size_t);

    // 4. |this| value
    masm.storeValue(UndefinedValue(), Address(StackPointer, argOffset));
    argOffset += sizeof(Value);

    // 5. Fill the arguments
    unsigned offsetToCallerStackArgs = ionFramePushed + sizeof(AsmJSFrame);
    FillArgumentArray(m, exit.sig().args(), argOffset, offsetToCallerStackArgs, scratch);
    argOffset += exit.sig().args().length() * sizeof(Value);
    MOZ_ASSERT(argOffset == ionFrameBytes);

    // 6. Jit code will clobber all registers, even non-volatiles. GlobalReg and
    //    HeapReg are removed from the general register set for asm.js code, so
    //    these will not have been saved by the caller like all other registers,
    //    so they must be explicitly preserved. Only save GlobalReg since
    //    HeapReg must be reloaded (from global data) after the call since the
    //    heap may change during the FFI call.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    static_assert(MaybeSavedGlobalReg == sizeof(void*), "stack frame accounting");
    masm.storePtr(GlobalReg, Address(StackPointer, ionFrameBytes));
#endif

    {
        // Enable Activation.
        //
        // This sequence requires four registers, and needs to preserve the 'callee'
        // register, so there are five live registers.
        MOZ_ASSERT(callee == AsmJSIonExitRegCallee);
        Register reg0 = AsmJSIonExitRegE0;
        Register reg1 = AsmJSIonExitRegE1;
        Register reg2 = AsmJSIonExitRegE2;
        Register reg3 = AsmJSIonExitRegE3;

        // The following is inlined:
        //   JSContext* cx = activation->cx();
        //   Activation* act = cx->runtime()->activation();
        //   act.active_ = true;
        //   act.prevJitTop_ = cx->runtime()->jitTop;
        //   act.prevJitJSContext_ = cx->runtime()->jitJSContext;
        //   cx->runtime()->jitJSContext = cx;
        //   act.prevJitActivation_ = cx->runtime()->jitActivation;
        //   cx->runtime()->jitActivation = act;
        //   act.prevProfilingActivation_ = cx->runtime()->profilingActivation;
        //   cx->runtime()->profilingActivation_ = act;
        // On the ARM store8() uses the secondScratchReg (lr) as a temp.
        size_t offsetOfActivation = JSRuntime::offsetOfActivation();
        size_t offsetOfJitTop = offsetof(JSRuntime, jitTop);
        size_t offsetOfJitJSContext = offsetof(JSRuntime, jitJSContext);
        size_t offsetOfJitActivation = offsetof(JSRuntime, jitActivation);
        size_t offsetOfProfilingActivation = JSRuntime::offsetOfProfilingActivation();
        masm.loadAsmJSActivation(reg0);
        masm.loadPtr(Address(reg0, AsmJSActivation::offsetOfContext()), reg3);
        masm.loadPtr(Address(reg3, JSContext::offsetOfRuntime()), reg0);
        masm.loadPtr(Address(reg0, offsetOfActivation), reg1);

        //   act.active_ = true;
        masm.store8(Imm32(1), Address(reg1, JitActivation::offsetOfActiveUint8()));

        //   act.prevJitTop_ = cx->runtime()->jitTop;
        masm.loadPtr(Address(reg0, offsetOfJitTop), reg2);
        masm.storePtr(reg2, Address(reg1, JitActivation::offsetOfPrevJitTop()));

        //   act.prevJitJSContext_ = cx->runtime()->jitJSContext;
        masm.loadPtr(Address(reg0, offsetOfJitJSContext), reg2);
        masm.storePtr(reg2, Address(reg1, JitActivation::offsetOfPrevJitJSContext()));
        //   cx->runtime()->jitJSContext = cx;
        masm.storePtr(reg3, Address(reg0, offsetOfJitJSContext));

        //   act.prevJitActivation_ = cx->runtime()->jitActivation;
        masm.loadPtr(Address(reg0, offsetOfJitActivation), reg2);
        masm.storePtr(reg2, Address(reg1, JitActivation::offsetOfPrevJitActivation()));
        //   cx->runtime()->jitActivation = act;
        masm.storePtr(reg1, Address(reg0, offsetOfJitActivation));

        //   act.prevProfilingActivation_ = cx->runtime()->profilingActivation;
        masm.loadPtr(Address(reg0, offsetOfProfilingActivation), reg2);
        masm.storePtr(reg2, Address(reg1, Activation::offsetOfPrevProfiling()));
        //   cx->runtime()->profilingActivation_ = act;
        masm.storePtr(reg1, Address(reg0, offsetOfProfilingActivation));
    }

    AssertStackAlignment(masm, JitStackAlignment, sizeOfRetAddr);
    masm.callJitFromAsmJS(callee);
    AssertStackAlignment(masm, JitStackAlignment, sizeOfRetAddr);

    {
        // Disable Activation.
        //
        // This sequence needs three registers, and must preserve the JSReturnReg_Data and
        // JSReturnReg_Type, so there are five live registers.
        MOZ_ASSERT(JSReturnReg_Data == AsmJSIonExitRegReturnData);
        MOZ_ASSERT(JSReturnReg_Type == AsmJSIonExitRegReturnType);
        Register reg0 = AsmJSIonExitRegD0;
        Register reg1 = AsmJSIonExitRegD1;
        Register reg2 = AsmJSIonExitRegD2;

        // The following is inlined:
        //   rt->profilingActivation = prevProfilingActivation_;
        //   rt->activation()->active_ = false;
        //   rt->jitTop = prevJitTop_;
        //   rt->jitJSContext = prevJitJSContext_;
        //   rt->jitActivation = prevJitActivation_;
        // On the ARM store8() uses the secondScratchReg (lr) as a temp.
        size_t offsetOfActivation = JSRuntime::offsetOfActivation();
        size_t offsetOfJitTop = offsetof(JSRuntime, jitTop);
        size_t offsetOfJitJSContext = offsetof(JSRuntime, jitJSContext);
        size_t offsetOfJitActivation = offsetof(JSRuntime, jitActivation);
        size_t offsetOfProfilingActivation = JSRuntime::offsetOfProfilingActivation();

        masm.movePtr(AsmJSImmPtr(AsmJSImm_Runtime), reg0);
        masm.loadPtr(Address(reg0, offsetOfActivation), reg1);

        //   rt->jitTop = prevJitTop_;
        masm.loadPtr(Address(reg1, JitActivation::offsetOfPrevJitTop()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfJitTop));

        //   rt->profilingActivation = rt->activation()->prevProfiling_;
        masm.loadPtr(Address(reg1, Activation::offsetOfPrevProfiling()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfProfilingActivation));

        //   rt->activation()->active_ = false;
        masm.store8(Imm32(0), Address(reg1, JitActivation::offsetOfActiveUint8()));

        //   rt->jitJSContext = prevJitJSContext_;
        masm.loadPtr(Address(reg1, JitActivation::offsetOfPrevJitJSContext()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfJitJSContext));

        //   rt->jitActivation = prevJitActivation_;
        masm.loadPtr(Address(reg1, JitActivation::offsetOfPrevJitActivation()), reg2);
        masm.storePtr(reg2, Address(reg0, offsetOfJitActivation));
    }

    // Reload the global register since Ion code can clobber any register.
#if defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_MIPS)
    static_assert(MaybeSavedGlobalReg == sizeof(void*), "stack frame accounting");
    masm.loadPtr(Address(StackPointer, ionFrameBytes), GlobalReg);
#endif

    // As explained above, the frame was aligned for Ion such that
    //   (sp + sizeof(void*)) % JitStackAlignment == 0
    // But now we possibly want to call one of several different C++ functions,
    // so subtract the sizeof(void*) so that sp is aligned for an ABI call.
    static_assert(ABIStackAlignment <= JitStackAlignment, "subsumes");
    masm.reserveStack(sizeOfRetAddr);
    unsigned nativeFramePushed = masm.framePushed();
    AssertStackAlignment(masm, ABIStackAlignment);

    masm.branchTestMagic(Assembler::Equal, JSReturnOperand, throwLabel);

    Label oolConvert;
    switch (exit.sig().retType().which()) {
      case RetType::Void:
        break;
      case RetType::Signed:
        masm.convertValueToInt32(JSReturnOperand, ReturnDoubleReg, ReturnReg, &oolConvert,
                                 /* -0 check */ false);
        break;
      case RetType::Double:
        masm.convertValueToDouble(JSReturnOperand, ReturnDoubleReg, &oolConvert);
        break;
      case RetType::Float:
        MOZ_CRASH("Float shouldn't be returned from a FFI");
      case RetType::Int32x4:
      case RetType::Float32x4:
        MOZ_CRASH("SIMD types shouldn't be returned from a FFI");
    }

    Label done;
    masm.bind(&done);

    // The heap pointer has to be reloaded anyway since Ion could have clobbered
    // it. Additionally, the FFI may have detached the heap buffer.
    masm.loadAsmJSHeapRegisterFromGlobalData();
    GenerateCheckForHeapDetachment(m, ABIArgGenerator::NonReturn_VolatileReg0);

    Label profilingReturn;
    GenerateAsmJSExitEpilogue(masm, masm.framePushed(), AsmJSExit::JitFFI, &profilingReturn);

    if (oolConvert.used()) {
        masm.bind(&oolConvert);
        masm.setFramePushed(nativeFramePushed);

        // Coercion calls use the following stack layout (sp grows to the left):
        //   | args | padding | Value argv[1] | padding | exit AsmJSFrame |
        MIRTypeVector coerceArgTypes(m.cx());
        JS_ALWAYS_TRUE(coerceArgTypes.append(MIRType_Pointer));
        unsigned offsetToCoerceArgv = AlignBytes(StackArgBytes(coerceArgTypes), sizeof(Value));
        MOZ_ASSERT(nativeFramePushed >= offsetToCoerceArgv + sizeof(Value));
        AssertStackAlignment(masm, ABIStackAlignment);

        // Store return value into argv[0]
        masm.storeValue(JSReturnOperand, Address(StackPointer, offsetToCoerceArgv));

        // argument 0: argv
        ABIArgMIRTypeIter i(coerceArgTypes);
        Address argv(StackPointer, offsetToCoerceArgv);
        if (i->kind() == ABIArg::GPR) {
            masm.computeEffectiveAddress(argv, i->gpr());
        } else {
            masm.computeEffectiveAddress(argv, scratch);
            masm.storePtr(scratch, Address(StackPointer, i->offsetFromArgBase()));
        }
        i++;
        MOZ_ASSERT(i.done());

        // Call coercion function
        AssertStackAlignment(masm, ABIStackAlignment);
        switch (exit.sig().retType().which()) {
          case RetType::Signed:
            masm.call(AsmJSImmPtr(AsmJSImm_CoerceInPlace_ToInt32));
            masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
            masm.unboxInt32(Address(StackPointer, offsetToCoerceArgv), ReturnReg);
            break;
          case RetType::Double:
            masm.call(AsmJSImmPtr(AsmJSImm_CoerceInPlace_ToNumber));
            masm.branchTest32(Assembler::Zero, ReturnReg, ReturnReg, throwLabel);
            masm.loadDouble(Address(StackPointer, offsetToCoerceArgv), ReturnDoubleReg);
            break;
          default:
            MOZ_CRASH("Unsupported convert type");
        }

        masm.jump(&done);
        masm.setFramePushed(0);
    }

    MOZ_ASSERT(masm.framePushed() == 0);

    return m.finishGeneratingJitExit(exitIndex, &begin, &profilingReturn) && !masm.oom();
}

// See "asm.js FFI calls" comment above.
static bool
GenerateFFIExits(ModuleCompiler& m, const ModuleCompiler::ExitDescriptor& exit, unsigned exitIndex,
                 Label* throwLabel)
{
    // Generate the slow path through the interpreter
    if (!GenerateFFIInterpExit(m, exit, exitIndex, throwLabel))
        return false;

    // Generate the fast path
    if (!GenerateFFIIonExit(m, exit, exitIndex, throwLabel))
        return false;

    return true;
}

// Generate a thunk that updates fp before calling the given builtin so that
// both the builtin and the calling function show up in profiler stacks. (This
// thunk is dynamically patched in when profiling is enabled.) Since the thunk
// pushes an AsmJSFrame on the stack, that means we must rebuild the stack
// frame. Fortunately, these are low arity functions and everything is passed in
// regs on everything but x86 anyhow.
//
// NB: Since this thunk is being injected at system ABI callsites, it must
//     preserve the argument registers (going in) and the return register
//     (coming out) and preserve non-volatile registers.
static bool
GenerateBuiltinThunk(ModuleCompiler& m, AsmJSExit::BuiltinKind builtin)
{
    MacroAssembler& masm = m.masm();
    MOZ_ASSERT(masm.framePushed() == 0);

    MIRTypeVector argTypes(m.cx());
    if (!argTypes.reserve(2))
        return false;

    switch (builtin) {
      case AsmJSExit::Builtin_ToInt32:
        argTypes.infallibleAppend(MIRType_Int32);
        break;
#if defined(JS_CODEGEN_ARM)
      case AsmJSExit::Builtin_IDivMod:
      case AsmJSExit::Builtin_UDivMod:
        argTypes.infallibleAppend(MIRType_Int32);
        argTypes.infallibleAppend(MIRType_Int32);
        break;
#endif
      case AsmJSExit::Builtin_SinD:
      case AsmJSExit::Builtin_CosD:
      case AsmJSExit::Builtin_TanD:
      case AsmJSExit::Builtin_ASinD:
      case AsmJSExit::Builtin_ACosD:
      case AsmJSExit::Builtin_ATanD:
      case AsmJSExit::Builtin_CeilD:
      case AsmJSExit::Builtin_FloorD:
      case AsmJSExit::Builtin_ExpD:
      case AsmJSExit::Builtin_LogD:
        argTypes.infallibleAppend(MIRType_Double);
        break;
      case AsmJSExit::Builtin_ModD:
      case AsmJSExit::Builtin_PowD:
      case AsmJSExit::Builtin_ATan2D:
        argTypes.infallibleAppend(MIRType_Double);
        argTypes.infallibleAppend(MIRType_Double);
        break;
      case AsmJSExit::Builtin_CeilF:
      case AsmJSExit::Builtin_FloorF:
        argTypes.infallibleAppend(MIRType_Float32);
        break;
      case AsmJSExit::Builtin_Limit:
        MOZ_CRASH("Bad builtin");
    }

    uint32_t framePushed = StackDecrementForCall(masm, ABIStackAlignment, argTypes);

    Label begin;
    GenerateAsmJSExitPrologue(masm, framePushed, AsmJSExit::Builtin(builtin), &begin);

    for (ABIArgMIRTypeIter i(argTypes); !i.done(); i++) {
        if (i->kind() != ABIArg::Stack)
            continue;
#if !defined(JS_CODEGEN_ARM)
        unsigned offsetToCallerStackArgs = sizeof(AsmJSFrame) + masm.framePushed();
        Address srcAddr(StackPointer, offsetToCallerStackArgs + i->offsetFromArgBase());
        Address dstAddr(StackPointer, i->offsetFromArgBase());
        if (i.mirType() == MIRType_Int32 || i.mirType() == MIRType_Float32) {
            masm.load32(srcAddr, ABIArgGenerator::NonArg_VolatileReg);
            masm.store32(ABIArgGenerator::NonArg_VolatileReg, dstAddr);
        } else {
            MOZ_ASSERT(i.mirType() == MIRType_Double);
            masm.loadDouble(srcAddr, ScratchDoubleReg);
            masm.storeDouble(ScratchDoubleReg, dstAddr);
        }
#else
        MOZ_CRASH("Architecture should have enough registers for all builtin calls");
#endif
    }

    AssertStackAlignment(masm, ABIStackAlignment);
    masm.call(BuiltinToImmKind(builtin));

    Label profilingReturn;
    GenerateAsmJSExitEpilogue(masm, framePushed, AsmJSExit::Builtin(builtin), &profilingReturn);
    return m.finishGeneratingBuiltinThunk(builtin, &begin, &profilingReturn) && !masm.oom();
}

static bool
GenerateStackOverflowExit(ModuleCompiler& m, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    GenerateAsmJSStackOverflowExit(masm, &m.stackOverflowLabel(), throwLabel);
    return m.finishGeneratingInlineStub(&m.stackOverflowLabel()) && !masm.oom();
}

static bool
GenerateOnDetachedLabelExit(ModuleCompiler& m, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    masm.bind(&m.onDetachedLabel());
    masm.assertStackAlignment(ABIStackAlignment);

    // For now, OnDetached always throws (see OnDetached comment).
    masm.call(AsmJSImmPtr(AsmJSImm_OnDetached));
    masm.jump(throwLabel);

    return m.finishGeneratingInlineStub(&m.onDetachedLabel()) && !masm.oom();
}

static bool
GenerateOnOutOfBoundsLabelExit(ModuleCompiler& m, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    masm.bind(&m.onOutOfBoundsLabel());

    // sp can be anything at this point, so ensure it is aligned when calling
    // into C++.  We unconditionally jump to throw so don't worry about restoring sp.
    masm.andPtr(Imm32(~(ABIStackAlignment - 1)), StackPointer);

    // OnOutOfBounds always throws.
    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(AsmJSImmPtr(AsmJSImm_OnOutOfBounds));
    masm.jump(throwLabel);

    return m.finishGeneratingInlineStub(&m.onOutOfBoundsLabel()) && !masm.oom();
}

static const RegisterSet AllRegsExceptSP =
    RegisterSet(GeneralRegisterSet(Registers::AllMask&
                                   ~(uint32_t(1) << Registers::StackPointer)),
                FloatRegisterSet(FloatRegisters::AllDoubleMask));

// The async interrupt-callback exit is called from arbitrarily-interrupted asm.js
// code. That means we must first save *all* registers and restore *all*
// registers (except the stack pointer) when we resume. The address to resume to
// (assuming that js::HandleExecutionInterrupt doesn't indicate that the
// execution should be aborted) is stored in AsmJSActivation::resumePC_.
// Unfortunately, loading this requires a scratch register which we don't have
// after restoring all registers. To hack around this, push the resumePC on the
// stack so that it can be popped directly into PC.
static bool
GenerateAsyncInterruptExit(ModuleCompiler& m, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    masm.align(CodeAlignment);
    masm.bind(&m.asyncInterruptLabel());

#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
    // Be very careful here not to perturb the machine state before saving it
    // to the stack. In particular, add/sub instructions may set conditions in
    // the flags register.
    masm.push(Imm32(0));            // space for resumePC
    masm.pushFlags();               // after this we are safe to use sub
    masm.setFramePushed(0);         // set to zero so we can use masm.framePushed() below
    masm.PushRegsInMask(AllRegsExceptSP, AllRegsExceptSP.fpus()); // save all GP/FP registers (except SP)

    Register scratch = ABIArgGenerator::NonArgReturnReg0;

    // Store resumePC into the reserved space.
    masm.loadAsmJSActivation(scratch);
    masm.loadPtr(Address(scratch, AsmJSActivation::offsetOfResumePC()), scratch);
    masm.storePtr(scratch, Address(StackPointer, masm.framePushed() + sizeof(void*)));

    // We know that StackPointer is word-aligned, but not necessarily
    // stack-aligned, so we need to align it dynamically.
    masm.mov(StackPointer, ABIArgGenerator::NonVolatileReg);
    masm.andPtr(Imm32(~(ABIStackAlignment - 1)), StackPointer);
    if (ShadowStackSpace)
        masm.subPtr(Imm32(ShadowStackSpace), StackPointer);

    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(AsmJSImmPtr(AsmJSImm_HandleExecutionInterrupt));

    masm.branchIfFalseBool(ReturnReg, throwLabel);

    // Restore the StackPointer to it's position before the call.
    masm.mov(ABIArgGenerator::NonVolatileReg, StackPointer);

    // Restore the machine state to before the interrupt.
    masm.PopRegsInMask(AllRegsExceptSP, AllRegsExceptSP.fpus()); // restore all GP/FP registers (except SP)
    masm.popFlags();              // after this, nothing that sets conditions
    masm.ret();                   // pop resumePC into PC
#elif defined(JS_CODEGEN_MIPS)
    // Reserve space to store resumePC.
    masm.subPtr(Imm32(sizeof(intptr_t)), StackPointer);
    // set to zero so we can use masm.framePushed() below.
    masm.setFramePushed(0);
    // When this platform supports SIMD extensions, we'll need to push high lanes
    // of SIMD registers as well.
    JS_STATIC_ASSERT(!SupportsSimd);
    // save all registers,except sp. After this stack is alligned.
    masm.PushRegsInMask(AllRegsExceptSP);

    // Save the stack pointer in a non-volatile register.
    masm.movePtr(StackPointer, s0);
    // Align the stack.
    masm.ma_and(StackPointer, StackPointer, Imm32(~(ABIStackAlignment - 1)));

    // Store resumePC into the reserved space.
    masm.loadAsmJSActivation(IntArgReg0);
    masm.loadPtr(Address(IntArgReg0, AsmJSActivation::offsetOfResumePC()), IntArgReg1);
    masm.storePtr(IntArgReg1, Address(s0, masm.framePushed()));

    // MIPS ABI requires rewserving stack for registes $a0 to $a3.
    masm.subPtr(Imm32(4 * sizeof(intptr_t)), StackPointer);

    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(AsmJSImm_HandleExecutionInterrupt);

    masm.addPtr(Imm32(4 * sizeof(intptr_t)), StackPointer);

    masm.branchIfFalseBool(ReturnReg, throwLabel);

    // This will restore stack to the address before the call.
    masm.movePtr(s0, StackPointer);
    masm.PopRegsInMask(AllRegsExceptSP);

    // Pop resumePC into PC. Clobber HeapReg to make the jump and restore it
    // during jump delay slot.
    masm.pop(HeapReg);
    masm.as_jr(HeapReg);
    masm.loadAsmJSHeapRegisterFromGlobalData();
#elif defined(JS_CODEGEN_ARM)
    masm.setFramePushed(0);         // set to zero so we can use masm.framePushed() below
    masm.PushRegsInMask(RegisterSet(GeneralRegisterSet(Registers::AllMask & ~(1<<Registers::sp)), FloatRegisterSet(uint32_t(0))));   // save all GP registers,excep sp

    // Save both the APSR and FPSCR in non-volatile registers.
    masm.as_mrs(r4);
    masm.as_vmrs(r5);
    // Save the stack pointer in a non-volatile register.
    masm.mov(sp,r6);
    // Align the stack.
    masm.ma_and(Imm32(~7), sp, sp);

    // Store resumePC into the return PC stack slot.
    masm.loadAsmJSActivation(IntArgReg0);
    masm.loadPtr(Address(IntArgReg0, AsmJSActivation::offsetOfResumePC()), IntArgReg1);
    masm.storePtr(IntArgReg1, Address(r6, 14 * sizeof(uint32_t*)));

    // When this platform supports SIMD extensions, we'll need to push and pop
    // high lanes of SIMD registers as well.
    JS_STATIC_ASSERT(!SupportsSimd);
    masm.PushRegsInMask(RegisterSet(GeneralRegisterSet(0), FloatRegisterSet(FloatRegisters::AllDoubleMask)));   // save all FP registers

    masm.assertStackAlignment(ABIStackAlignment);
    masm.call(AsmJSImm_HandleExecutionInterrupt);

    masm.branchIfFalseBool(ReturnReg, throwLabel);

    // Restore the machine state to before the interrupt. this will set the pc!
    masm.PopRegsInMask(RegisterSet(GeneralRegisterSet(0), FloatRegisterSet(FloatRegisters::AllDoubleMask)));   // restore all FP registers
    masm.mov(r6,sp);
    masm.as_vmsr(r5);
    masm.as_msr(r4);
    // Restore all GP registers
    masm.startDataTransferM(IsLoad, sp, IA, WriteBack);
    masm.transferReg(r0);
    masm.transferReg(r1);
    masm.transferReg(r2);
    masm.transferReg(r3);
    masm.transferReg(r4);
    masm.transferReg(r5);
    masm.transferReg(r6);
    masm.transferReg(r7);
    masm.transferReg(r8);
    masm.transferReg(r9);
    masm.transferReg(r10);
    masm.transferReg(r11);
    masm.transferReg(r12);
    masm.transferReg(lr);
    masm.finishDataTransfer();
    masm.ret();

#elif defined (JS_CODEGEN_NONE)
    MOZ_CRASH();
#else
# error "Unknown architecture!"
#endif

    return m.finishGeneratingInlineStub(&m.asyncInterruptLabel()) && !masm.oom();
}

static bool
GenerateSyncInterruptExit(ModuleCompiler& m, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    masm.setFramePushed(0);

    unsigned framePushed = StackDecrementForCall(masm, ABIStackAlignment, ShadowStackSpace);

    GenerateAsmJSExitPrologue(masm, framePushed, AsmJSExit::Interrupt, &m.syncInterruptLabel());

    AssertStackAlignment(masm, ABIStackAlignment);
    masm.call(AsmJSImmPtr(AsmJSImm_HandleExecutionInterrupt));
    masm.branchIfFalseBool(ReturnReg, throwLabel);

    Label profilingReturn;
    GenerateAsmJSExitEpilogue(masm, framePushed, AsmJSExit::Interrupt, &profilingReturn);
    return m.finishGeneratingInterrupt(&m.syncInterruptLabel(), &profilingReturn) && !masm.oom();
}

// If an exception is thrown, simply pop all frames (since asm.js does not
// contain try/catch). To do this:
//  1. Restore 'sp' to it's value right after the PushRegsInMask in GenerateEntry.
//  2. PopRegsInMask to restore the caller's non-volatile registers.
//  3. Return (to CallAsmJS).
static bool
GenerateThrowStub(ModuleCompiler& m, Label* throwLabel)
{
    MacroAssembler& masm = m.masm();
    masm.align(CodeAlignment);
    masm.bind(throwLabel);

    // We are about to pop all frames in this AsmJSActivation. Set fp to null to
    // maintain the invariant that fp is either null or pointing to a valid
    // frame.
    Register scratch = ABIArgGenerator::NonArgReturnReg0;
    masm.loadAsmJSActivation(scratch);
    masm.storePtr(ImmWord(0), Address(scratch, AsmJSActivation::offsetOfFP()));

    masm.setFramePushed(FramePushedForEntrySP);
    masm.loadPtr(Address(scratch, AsmJSActivation::offsetOfEntrySP()), StackPointer);
    masm.Pop(scratch);
    masm.PopRegsInMask(NonVolatileRegs, NonVolatileSimdRegs);
    MOZ_ASSERT(masm.framePushed() == 0);

    masm.mov(ImmWord(0), ReturnReg);
    masm.ret();

    return m.finishGeneratingInlineStub(throwLabel) && !masm.oom();
}

static bool
GenerateStubs(ModuleCompiler& m)
{
    for (unsigned i = 0; i < m.module().numExportedFunctions(); i++) {
        if (m.module().exportedFunction(i).isChangeHeap())
            continue;
        if (!GenerateEntry(m, i))
           return false;
    }

    Label throwLabel;

    for (ModuleCompiler::ExitMap::Range r = m.allExits(); !r.empty(); r.popFront()) {
        if (!GenerateFFIExits(m, r.front().key(), r.front().value(), &throwLabel))
            return false;
    }

    if (m.stackOverflowLabel().used() && !GenerateStackOverflowExit(m, &throwLabel))
        return false;

    if (m.onDetachedLabel().used() && !GenerateOnDetachedLabelExit(m, &throwLabel))
        return false;

    if (!GenerateOnOutOfBoundsLabelExit(m, &throwLabel))
        return false;

    if (!GenerateAsyncInterruptExit(m, &throwLabel))
        return false;
    if (m.syncInterruptLabel().used() && !GenerateSyncInterruptExit(m, &throwLabel))
        return false;

    if (!GenerateThrowStub(m, &throwLabel))
        return false;

    for (unsigned i = 0; i < AsmJSExit::Builtin_Limit; i++) {
        if (!GenerateBuiltinThunk(m, AsmJSExit::BuiltinKind(i)))
            return false;
    }

    return true;
}

static bool
FinishModule(ModuleCompiler& m,
             ScopedJSDeletePtr<AsmJSModule>* module)
{
    LifoAlloc lifo(TempAllocator::PreferredLifoChunkSize);
    TempAllocator alloc(&lifo);
    JitContext jitContext(m.cx(), &alloc);

    m.masm().resetForNewCodeGenerator(alloc);

    if (!GenerateStubs(m))
        return false;

    return m.finish(module);
}

static bool
CheckModule(ExclusiveContext* cx, AsmJSParser& parser, ParseNode* stmtList,
            ScopedJSDeletePtr<AsmJSModule>* moduleOut,
            ScopedJSFreePtr<char>* compilationTimeReport)
{
    if (!LookupAsmJSModuleInCache(cx, parser, moduleOut, compilationTimeReport))
        return false;
    if (*moduleOut)
        return true;

    ModuleCompiler m(cx, parser);
    if (!m.init())
        return false;

    if (PropertyName* moduleFunctionName = FunctionName(m.moduleFunctionNode())) {
        if (!CheckModuleLevelName(m, m.moduleFunctionNode(), moduleFunctionName))
            return false;
        m.initModuleFunctionName(moduleFunctionName);
    }

    if (!CheckFunctionHead(m, m.moduleFunctionNode()))
        return false;

    if (!CheckModuleArguments(m, m.moduleFunctionNode()))
        return false;

    if (!CheckPrecedingStatements(m, stmtList))
        return false;

    if (!CheckModuleProcessingDirectives(m))
        return false;

    if (!CheckModuleGlobals(m))
        return false;

    m.startFunctionBodies();

    if (!CheckFunctions(m))
        return false;

    m.finishFunctionBodies();

    if (!CheckFuncPtrTables(m))
        return false;

    if (!CheckModuleReturn(m))
        return false;

    TokenKind tk;
    if (!PeekToken(m.parser(), &tk))
        return false;
    if (tk != TOK_EOF && tk != TOK_RC)
        return m.fail(nullptr, "top-level export (return) must be the last statement");

    // The instruction cache is flushed when dynamically linking, so can inhibit now.
    AutoFlushICache afc("CheckModule", /* inhibit= */ true);

    ScopedJSDeletePtr<AsmJSModule> module;
    if (!FinishModule(m, &module))
        return false;

    JS::AsmJSCacheResult cacheResult = StoreAsmJSModuleInCache(parser, *module, cx);
    module->staticallyLink(cx);

    m.buildCompilationTimeReport(cacheResult, compilationTimeReport);
    *moduleOut = module.forget();
    return true;
}

static bool
Warn(AsmJSParser& parser, int errorNumber, const char* str)
{
    parser.reportNoOffset(ParseWarning, /* strict = */ false, errorNumber, str ? str : "");
    return false;
}

static bool
EstablishPreconditions(ExclusiveContext* cx, AsmJSParser& parser)
{
#ifdef JS_CODEGEN_NONE
    return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by lack of a JIT compiler");
#endif

    if (!cx->jitSupportsFloatingPoint())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by lack of floating point support");

    if (cx->gcSystemPageSize() != AsmJSPageSize)
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by non 4KiB system page size");

    if (!parser.options().asmJSOption)
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by javascript.options.asmjs in about:config");

    if (!parser.options().compileAndGo)
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Temporarily disabled for event-handler and other cloneable scripts");

    if (cx->compartment()->debuggerObservesAsmJS())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by debugger");

    if (parser.pc->isGenerator())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by generator context");

    if (parser.pc->isArrowFunction())
        return Warn(parser, JSMSG_USE_ASM_TYPE_FAIL, "Disabled by arrow function context");

    return true;
}

static bool
NoExceptionPending(ExclusiveContext* cx)
{
    return !cx->isJSContext() || !cx->asJSContext()->isExceptionPending();
}

bool
js::ValidateAsmJS(ExclusiveContext* cx, AsmJSParser& parser, ParseNode* stmtList, bool* validated)
{
    *validated = false;

    if (!EstablishPreconditions(cx, parser))
        return NoExceptionPending(cx);

    ScopedJSFreePtr<char> compilationTimeReport;
    ScopedJSDeletePtr<AsmJSModule> module;
    if (!CheckModule(cx, parser, stmtList, &module, &compilationTimeReport))
        return NoExceptionPending(cx);

    RootedObject moduleObj(cx, AsmJSModuleObject::create(cx, &module));
    if (!moduleObj)
        return false;

    FunctionBox* funbox = parser.pc->maybeFunction->pn_funbox;
    RootedFunction moduleFun(cx, NewAsmJSModuleFunction(cx, funbox->function(), moduleObj));
    if (!moduleFun)
        return false;

    MOZ_ASSERT(funbox->function()->isInterpreted());
    funbox->object = moduleFun;

    *validated = true;
    Warn(parser, JSMSG_USE_ASM_TYPE_OK, compilationTimeReport.get());
    return NoExceptionPending(cx);
}

bool
js::IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // See EstablishPreconditions.
#ifdef JS_CODEGEN_NONE
    bool available = false;
#else
    bool available = cx->jitSupportsFloatingPoint() &&
                     cx->gcSystemPageSize() == AsmJSPageSize &&
                     cx->runtime()->options().asmJS();
#endif

    args.rval().set(BooleanValue(available));
    return true;
}
