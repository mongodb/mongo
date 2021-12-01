/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "wasm/WasmBinaryToAST.h"

#include "mozilla/MathAlgorithms.h"
#include "mozilla/Sprintf.h"

#include "vm/JSCompartment.h"
#include "vm/JSContext.h"
#include "wasm/WasmBinaryIterator.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::wasm;

using mozilla::FloorLog2;

enum AstDecodeTerminationKind
{
    Unknown,
    End,
    Else
};

struct AstDecodeStackItem
{
    AstExpr* expr;
    AstDecodeTerminationKind terminationKind;
    ExprType type;

    explicit AstDecodeStackItem()
      : expr(nullptr),
        terminationKind(AstDecodeTerminationKind::Unknown),
        type(ExprType::Limit)
    {}
    explicit AstDecodeStackItem(AstDecodeTerminationKind terminationKind, ExprType type)
      : expr(nullptr),
        terminationKind(terminationKind),
        type(type)
    {}
    explicit AstDecodeStackItem(AstExpr* expr)
     : expr(expr),
       terminationKind(AstDecodeTerminationKind::Unknown),
       type(ExprType::Limit)
    {}
};

// We don't define a Value type because OpIter doesn't push void values, which
// we actually need here because we're building an AST, so we maintain our own
// stack.
struct AstDecodePolicy
{
    typedef Nothing Value;
    typedef Nothing ControlItem;
};

typedef OpIter<AstDecodePolicy> AstDecodeOpIter;

class AstDecodeContext
{
  public:
    typedef AstVector<AstDecodeStackItem> AstDecodeStack;
    typedef AstVector<uint32_t> DepthStack;

    JSContext* cx;
    LifoAlloc& lifo;
    Decoder& d;
    bool generateNames;

  private:
    ModuleEnvironment env_;

    AstModule& module_;
    AstDecodeOpIter *iter_;
    AstDecodeStack exprs_;
    DepthStack depths_;
    const ValTypeVector* locals_;
    AstNameVector blockLabels_;
    uint32_t currentLabelIndex_;
    ExprType retType_;

  public:
    AstDecodeContext(JSContext* cx, LifoAlloc& lifo, Decoder& d, AstModule& module,
                     bool generateNames)
     : cx(cx),
       lifo(lifo),
       d(d),
       generateNames(generateNames),
       env_(CompileMode::Once, Tier::Ion, DebugEnabled::False,
            cx->compartment()->creationOptions().getSharedMemoryAndAtomicsEnabled()
            ? Shareable::True
            : Shareable::False),
       module_(module),
       iter_(nullptr),
       exprs_(lifo),
       depths_(lifo),
       locals_(nullptr),
       blockLabels_(lifo),
       currentLabelIndex_(0),
       retType_(ExprType::Limit)
    {}

    ModuleEnvironment& env() { return env_; }

    AstModule& module() { return module_; }
    AstDecodeOpIter& iter() { return *iter_; }
    AstDecodeStack& exprs() { return exprs_; }
    DepthStack& depths() { return depths_; }

    AstNameVector& blockLabels() { return blockLabels_; }

    ExprType retType() const { return retType_; }
    const ValTypeVector& locals() const { return *locals_; }

    void popBack() { return exprs().popBack(); }
    AstDecodeStackItem popCopy() { return exprs().popCopy(); }
    AstDecodeStackItem& top() { return exprs().back(); }
    MOZ_MUST_USE bool push(AstDecodeStackItem item) { return exprs().append(item); }

    bool needFirst() {
        for (size_t i = depths().back(); i < exprs().length(); ++i) {
            if (!exprs()[i].expr->isVoid())
                return true;
        }
        return false;
    }

    AstExpr* handleVoidExpr(AstExpr* voidNode)
    {
        MOZ_ASSERT(voidNode->isVoid());

        // To attach a node that "returns void" to the middle of an AST, wrap it
        // in a first node next to the node it should accompany.
        if (needFirst()) {
            AstExpr *prev = popCopy().expr;

            // If the previous/A node is already a First, reuse it.
            if (prev->kind() == AstExprKind::First) {
                if (!prev->as<AstFirst>().exprs().append(voidNode))
                    return nullptr;
                return prev;
            }

            AstExprVector exprs(lifo);
            if (!exprs.append(prev))
                return nullptr;
            if (!exprs.append(voidNode))
                return nullptr;

            return new(lifo) AstFirst(Move(exprs));
        }

        return voidNode;
    }

    void startFunction(AstDecodeOpIter* iter, const ValTypeVector* locals, ExprType retType)
    {
        iter_ = iter;
        locals_ = locals;
        currentLabelIndex_ = 0;
        retType_ = retType;
    }
    void endFunction()
    {
        iter_ = nullptr;
        locals_ = nullptr;
        retType_ = ExprType::Limit;
        MOZ_ASSERT(blockLabels_.length() == 0);
    }
    uint32_t nextLabelIndex()
    {
        return currentLabelIndex_++;
    }
};

static bool
GenerateName(AstDecodeContext& c, const AstName& prefix, uint32_t index, AstName* name)
{
    if (!c.generateNames) {
        *name = AstName();
        return true;
    }

    AstVector<char16_t> result(c.lifo);
    if (!result.append(u'$'))
        return false;
    if (!result.append(prefix.begin(), prefix.length()))
        return false;

    uint32_t tmp = index;
    do {
        if (!result.append(u'0'))
            return false;
        tmp /= 10;
    } while (tmp);

    if (index) {
        char16_t* p = result.end();
        for (tmp = index; tmp; tmp /= 10)
            *(--p) = u'0' + (tmp % 10);
    }

    size_t length = result.length();
    char16_t* begin = result.extractOrCopyRawBuffer();
    if (!begin)
        return false;

    *name = AstName(begin, length);
    return true;
}

static bool
GenerateRef(AstDecodeContext& c, const AstName& prefix, uint32_t index, AstRef* ref)
{
    MOZ_ASSERT(index != AstNoIndex);

    if (!c.generateNames) {
        *ref = AstRef(index);
        return true;
    }

    AstName name;
    if (!GenerateName(c, prefix, index, &name))
        return false;
    MOZ_ASSERT(!name.empty());

    *ref = AstRef(name);
    ref->setIndex(index);
    return true;
}

static bool
GenerateFuncRef(AstDecodeContext& c, uint32_t funcIndex, AstRef* ref)
{
    if (funcIndex < c.module().numFuncImports()) {
        *ref = AstRef(c.module().funcImportNames()[funcIndex]);
    } else {
        if (!GenerateRef(c, AstName(u"func"), funcIndex, ref))
            return false;
    }
    return true;
}

static bool
AstDecodeCallArgs(AstDecodeContext& c, const SigWithId& sig, AstExprVector* funcArgs)
{
    MOZ_ASSERT(!c.iter().currentBlockHasPolymorphicBase());

    uint32_t numArgs = sig.args().length();
    if (!funcArgs->resize(numArgs))
        return false;

    for (size_t i = 0; i < numArgs; ++i)
        (*funcArgs)[i] = c.exprs()[c.exprs().length() - numArgs + i].expr;

    c.exprs().shrinkBy(numArgs);

    return true;
}

static bool
AstDecodeExpr(AstDecodeContext& c);

static bool
AstDecodeDrop(AstDecodeContext& c)
{
    if (!c.iter().readDrop())
        return false;

    AstDecodeStackItem value = c.popCopy();

    AstExpr* tmp = new(c.lifo) AstDrop(*value.expr);
    if (!tmp)
        return false;

    tmp = c.handleVoidExpr(tmp);
    if (!tmp)
        return false;

    if (!c.push(AstDecodeStackItem(tmp)))
        return false;

    return true;
}

static bool
AstDecodeCall(AstDecodeContext& c)
{
    uint32_t funcIndex;
    AstDecodeOpIter::ValueVector unusedArgs;
    if (!c.iter().readCall(&funcIndex, &unusedArgs))
        return false;

    if (c.iter().currentBlockHasPolymorphicBase())
        return true;

    AstRef funcRef;
    if (!GenerateFuncRef(c, funcIndex, &funcRef))
        return false;

    const SigWithId* sig = c.env().funcSigs[funcIndex];

    AstExprVector args(c.lifo);
    if (!AstDecodeCallArgs(c, *sig, &args))
        return false;

    AstCall* call = new(c.lifo) AstCall(Op::Call, sig->ret(), funcRef, Move(args));
    if (!call)
        return false;

    AstExpr* result = call;
    if (IsVoid(sig->ret()))
        result = c.handleVoidExpr(call);

    if (!c.push(AstDecodeStackItem(result)))
        return false;

    return true;
}

static bool
AstDecodeCallIndirect(AstDecodeContext& c)
{
    uint32_t sigIndex;
    AstDecodeOpIter::ValueVector unusedArgs;
    if (!c.iter().readCallIndirect(&sigIndex, nullptr, &unusedArgs))
        return false;

    if (c.iter().currentBlockHasPolymorphicBase())
        return true;

    AstDecodeStackItem index = c.popCopy();

    AstRef sigRef;
    if (!GenerateRef(c, AstName(u"type"), sigIndex, &sigRef))
        return false;

    const SigWithId& sig = c.env().sigs[sigIndex];
    AstExprVector args(c.lifo);
    if (!AstDecodeCallArgs(c, sig, &args))
        return false;

    AstCallIndirect* call = new(c.lifo) AstCallIndirect(sigRef, sig.ret(), Move(args), index.expr);
    if (!call)
        return false;

    AstExpr* result = call;
    if (IsVoid(sig.ret()))
        result = c.handleVoidExpr(call);

    if (!c.push(AstDecodeStackItem(result)))
        return false;

    return true;
}

static bool
AstDecodeGetBlockRef(AstDecodeContext& c, uint32_t depth, AstRef* ref)
{
    if (!c.generateNames || depth >= c.blockLabels().length()) {
        // Also ignoring if it's a function body label.
        *ref = AstRef(depth);
        return true;
    }

    uint32_t index = c.blockLabels().length() - depth - 1;
    if (c.blockLabels()[index].empty()) {
        if (!GenerateName(c, AstName(u"label"), c.nextLabelIndex(), &c.blockLabels()[index]))
            return false;
    }
    *ref = AstRef(c.blockLabels()[index]);
    ref->setIndex(depth);
    return true;
}

static bool
AstDecodeBrTable(AstDecodeContext& c)
{
    bool unreachable = c.iter().currentBlockHasPolymorphicBase();

    Uint32Vector depths;
    uint32_t defaultDepth;
    ExprType type;
    if (!c.iter().readBrTable(&depths, &defaultDepth, &type, nullptr, nullptr))
        return false;

    if (unreachable)
        return true;

    AstRefVector table(c.lifo);
    if (!table.resize(depths.length()))
        return false;

    for (size_t i = 0; i < depths.length(); ++i) {
        if (!AstDecodeGetBlockRef(c, depths[i], &table[i]))
            return false;
    }

    AstDecodeStackItem index = c.popCopy();
    AstDecodeStackItem value;
    if (!IsVoid(type))
        value = c.popCopy();

    AstRef def;
    if (!AstDecodeGetBlockRef(c, defaultDepth, &def))
        return false;

    auto branchTable = new(c.lifo) AstBranchTable(*index.expr, def, Move(table), value.expr);
    if (!branchTable)
        return false;

    if (!c.push(AstDecodeStackItem(branchTable)))
        return false;

    return true;
}

static bool
AstDecodeBlock(AstDecodeContext& c, Op op)
{
    MOZ_ASSERT(op == Op::Block || op == Op::Loop);

    if (!c.blockLabels().append(AstName()))
        return false;

    if (op == Op::Loop) {
      if (!c.iter().readLoop())
          return false;
    } else {
      if (!c.iter().readBlock())
          return false;
    }

    if (!c.depths().append(c.exprs().length()))
        return false;

    ExprType type;
    while (true) {
        if (!AstDecodeExpr(c))
            return false;

        const AstDecodeStackItem& item = c.top();
        if (!item.expr) { // Op::End was found
            type = item.type;
            c.popBack();
            break;
        }
    }

    AstExprVector exprs(c.lifo);
    for (auto i = c.exprs().begin() + c.depths().back(), e = c.exprs().end();
         i != e; ++i) {
        if (!exprs.append(i->expr))
            return false;
    }
    c.exprs().shrinkTo(c.depths().popCopy());

    AstName name = c.blockLabels().popCopy();
    AstBlock* block = new(c.lifo) AstBlock(op, type, name, Move(exprs));
    if (!block)
        return false;

    AstExpr* result = block;
    if (IsVoid(type))
        result = c.handleVoidExpr(block);

    if (!c.push(AstDecodeStackItem(result)))
        return false;

    return true;
}

static bool
AstDecodeIf(AstDecodeContext& c)
{
    if (!c.iter().readIf(nullptr))
        return false;

    AstDecodeStackItem cond = c.popCopy();

    bool hasElse = false;

    if (!c.depths().append(c.exprs().length()))
        return false;

    if (!c.blockLabels().append(AstName()))
        return false;

    ExprType type;
    while (true) {
        if (!AstDecodeExpr(c))
            return false;

        const AstDecodeStackItem& item = c.top();
        if (!item.expr) { // Op::End was found
            hasElse = item.terminationKind == AstDecodeTerminationKind::Else;
            type = item.type;
            c.popBack();
            break;
        }
    }

    AstExprVector thenExprs(c.lifo);
    for (auto i = c.exprs().begin() + c.depths().back(), e = c.exprs().end();
         i != e; ++i) {
        if (!thenExprs.append(i->expr))
            return false;
    }
    c.exprs().shrinkTo(c.depths().back());

    AstExprVector elseExprs(c.lifo);
    if (hasElse) {
        while (true) {
            if (!AstDecodeExpr(c))
                return false;

            const AstDecodeStackItem& item = c.top();
            if (!item.expr) { // Op::End was found
                c.popBack();
                break;
            }
        }

        for (auto i = c.exprs().begin() + c.depths().back(), e = c.exprs().end();
             i != e; ++i) {
            if (!elseExprs.append(i->expr))
                return false;
        }
        c.exprs().shrinkTo(c.depths().back());
    }

    c.depths().popBack();

    AstName name = c.blockLabels().popCopy();

    AstIf* if_ = new(c.lifo) AstIf(type, cond.expr, name, Move(thenExprs), Move(elseExprs));
    if (!if_)
        return false;

    AstExpr* result = if_;
    if (IsVoid(type))
        result = c.handleVoidExpr(if_);

    if (!c.push(AstDecodeStackItem(result)))
        return false;

    return true;
}

static bool
AstDecodeEnd(AstDecodeContext& c)
{
    LabelKind kind;
    ExprType type;
    if (!c.iter().readEnd(&kind, &type, nullptr))
        return false;

    c.iter().popEnd();

    if (!c.push(AstDecodeStackItem(AstDecodeTerminationKind::End, type)))
        return false;

    return true;
}

static bool
AstDecodeElse(AstDecodeContext& c)
{
    ExprType type;

    if (!c.iter().readElse(&type, nullptr))
        return false;

    if (!c.push(AstDecodeStackItem(AstDecodeTerminationKind::Else, type)))
        return false;

    return true;
}

static bool
AstDecodeNop(AstDecodeContext& c)
{
    if (!c.iter().readNop())
        return false;

    AstExpr* tmp = new(c.lifo) AstNop();
    if (!tmp)
        return false;

    tmp = c.handleVoidExpr(tmp);
    if (!tmp)
        return false;

    if (!c.push(AstDecodeStackItem(tmp)))
        return false;

    return true;
}

static bool
AstDecodeUnary(AstDecodeContext& c, ValType type, Op op)
{
    if (!c.iter().readUnary(type, nullptr))
        return false;

    AstDecodeStackItem operand = c.popCopy();

    AstUnaryOperator* unary = new(c.lifo) AstUnaryOperator(op, operand.expr);
    if (!unary)
        return false;

    if (!c.push(AstDecodeStackItem(unary)))
        return false;

    return true;
}

static bool
AstDecodeBinary(AstDecodeContext& c, ValType type, Op op)
{
    if (!c.iter().readBinary(type, nullptr, nullptr))
        return false;

    AstDecodeStackItem rhs = c.popCopy();
    AstDecodeStackItem lhs = c.popCopy();

    AstBinaryOperator* binary = new(c.lifo) AstBinaryOperator(op, lhs.expr, rhs.expr);
    if (!binary)
        return false;

    if (!c.push(AstDecodeStackItem(binary)))
        return false;

    return true;
}

static bool
AstDecodeSelect(AstDecodeContext& c)
{
    StackType type;
    if (!c.iter().readSelect(&type, nullptr, nullptr, nullptr))
        return false;

    if (c.iter().currentBlockHasPolymorphicBase())
        return true;

    AstDecodeStackItem selectFalse = c.popCopy();
    AstDecodeStackItem selectTrue = c.popCopy();
    AstDecodeStackItem cond = c.popCopy();

    auto* select = new(c.lifo) AstTernaryOperator(Op::Select, cond.expr, selectTrue.expr,
                                                  selectFalse.expr);
    if (!select)
        return false;

    if (!c.push(AstDecodeStackItem(select)))
        return false;

    return true;
}

static bool
AstDecodeComparison(AstDecodeContext& c, ValType type, Op op)
{
    if (!c.iter().readComparison(type, nullptr, nullptr))
        return false;

    AstDecodeStackItem rhs = c.popCopy();
    AstDecodeStackItem lhs = c.popCopy();

    AstComparisonOperator* comparison = new(c.lifo) AstComparisonOperator(op, lhs.expr, rhs.expr);
    if (!comparison)
        return false;

    if (!c.push(AstDecodeStackItem(comparison)))
        return false;

    return true;
}

static bool
AstDecodeConversion(AstDecodeContext& c, ValType fromType, ValType toType, Op op)
{
    if (!c.iter().readConversion(fromType, toType, nullptr))
        return false;

    AstDecodeStackItem operand = c.popCopy();

    AstConversionOperator* conversion = new(c.lifo) AstConversionOperator(op, operand.expr);
    if (!conversion)
        return false;

    if (!c.push(AstDecodeStackItem(conversion)))
        return false;

    return true;
}

#ifdef ENABLE_WASM_SATURATING_TRUNC_OPS
static bool
AstDecodeExtraConversion(AstDecodeContext& c, ValType fromType, ValType toType, NumericOp op)
{
    if (!c.iter().readConversion(fromType, toType, nullptr))
        return false;

    AstDecodeStackItem operand = c.popCopy();

    AstExtraConversionOperator* conversion =
        new(c.lifo) AstExtraConversionOperator(op, operand.expr);
    if (!conversion)
        return false;

    if (!c.push(AstDecodeStackItem(conversion)))
        return false;

    return true;
}
#endif

static AstLoadStoreAddress
AstDecodeLoadStoreAddress(const LinearMemoryAddress<Nothing>& addr, const AstDecodeStackItem& item)
{
    uint32_t flags = FloorLog2(addr.align);
    return AstLoadStoreAddress(item.expr, flags, addr.offset);
}

static bool
AstDecodeLoad(AstDecodeContext& c, ValType type, uint32_t byteSize, Op op)
{
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readLoad(type, byteSize, &addr))
        return false;

    AstDecodeStackItem item = c.popCopy();

    AstLoad* load = new(c.lifo) AstLoad(op, AstDecodeLoadStoreAddress(addr, item));
    if (!load)
        return false;

    if (!c.push(AstDecodeStackItem(load)))
        return false;

    return true;
}

static bool
AstDecodeStore(AstDecodeContext& c, ValType type, uint32_t byteSize, Op op)
{
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readStore(type, byteSize, &addr, nullptr))
        return false;

    AstDecodeStackItem value = c.popCopy();
    AstDecodeStackItem item = c.popCopy();

    AstStore* store = new(c.lifo) AstStore(op, AstDecodeLoadStoreAddress(addr, item), value.expr);
    if (!store)
        return false;

    AstExpr* wrapped = c.handleVoidExpr(store);
    if (!wrapped)
        return false;

    if (!c.push(AstDecodeStackItem(wrapped)))
        return false;

    return true;
}

static bool
AstDecodeCurrentMemory(AstDecodeContext& c)
{
    if (!c.iter().readCurrentMemory())
        return false;

    AstCurrentMemory* gm = new(c.lifo) AstCurrentMemory();
    if (!gm)
        return false;

    if (!c.push(AstDecodeStackItem(gm)))
        return false;

    return true;
}

static bool
AstDecodeGrowMemory(AstDecodeContext& c)
{
    if (!c.iter().readGrowMemory(nullptr))
        return false;

    AstDecodeStackItem operand = c.popCopy();

    AstGrowMemory* gm = new(c.lifo) AstGrowMemory(operand.expr);
    if (!gm)
        return false;

    if (!c.push(AstDecodeStackItem(gm)))
        return false;

    return true;
}

static bool
AstDecodeBranch(AstDecodeContext& c, Op op)
{
    MOZ_ASSERT(op == Op::Br || op == Op::BrIf);

    uint32_t depth;
    ExprType type;
    AstDecodeStackItem value;
    AstDecodeStackItem cond;
    if (op == Op::Br) {
        if (!c.iter().readBr(&depth, &type, nullptr))
            return false;
        if (!IsVoid(type))
            value = c.popCopy();
    } else {
        if (!c.iter().readBrIf(&depth, &type, nullptr, nullptr))
            return false;
        if (!IsVoid(type))
            value = c.popCopy();
        cond = c.popCopy();
    }

    AstRef depthRef;
    if (!AstDecodeGetBlockRef(c, depth, &depthRef))
        return false;

    if (op == Op::Br || !value.expr)
        type = ExprType::Void;
    AstBranch* branch = new(c.lifo) AstBranch(op, type, cond.expr, depthRef, value.expr);
    if (!branch)
        return false;

    if (!c.push(AstDecodeStackItem(branch)))
        return false;

    return true;
}

static bool
AstDecodeGetLocal(AstDecodeContext& c)
{
    uint32_t getLocalId;
    if (!c.iter().readGetLocal(c.locals(), &getLocalId))
        return false;

    AstRef localRef;
    if (!GenerateRef(c, AstName(u"var"), getLocalId, &localRef))
        return false;

    AstGetLocal* getLocal = new(c.lifo) AstGetLocal(localRef);
    if (!getLocal)
        return false;

    if (!c.push(AstDecodeStackItem(getLocal)))
        return false;

    return true;
}

static bool
AstDecodeSetLocal(AstDecodeContext& c)
{
    uint32_t setLocalId;
    if (!c.iter().readSetLocal(c.locals(), &setLocalId, nullptr))
        return false;

    AstDecodeStackItem setLocalValue = c.popCopy();

    AstRef localRef;
    if (!GenerateRef(c, AstName(u"var"), setLocalId, &localRef))
        return false;

    AstSetLocal* setLocal = new(c.lifo) AstSetLocal(localRef, *setLocalValue.expr);
    if (!setLocal)
        return false;

    AstExpr* expr = c.handleVoidExpr(setLocal);
    if (!expr)
        return false;

    if (!c.push(AstDecodeStackItem(expr)))
        return false;

    return true;
}

static bool
AstDecodeTeeLocal(AstDecodeContext& c)
{
    uint32_t teeLocalId;
    if (!c.iter().readTeeLocal(c.locals(), &teeLocalId, nullptr))
        return false;

    AstDecodeStackItem teeLocalValue = c.popCopy();

    AstRef localRef;
    if (!GenerateRef(c, AstName(u"var"), teeLocalId, &localRef))
        return false;

    AstTeeLocal* teeLocal = new(c.lifo) AstTeeLocal(localRef, *teeLocalValue.expr);
    if (!teeLocal)
        return false;

    if (!c.push(AstDecodeStackItem(teeLocal)))
        return false;

    return true;
}

static bool
AstDecodeGetGlobal(AstDecodeContext& c)
{
    uint32_t globalId;
    if (!c.iter().readGetGlobal(&globalId))
        return false;

    AstRef globalRef;
    if (!GenerateRef(c, AstName(u"global"), globalId, &globalRef))
        return false;

    auto* getGlobal = new(c.lifo) AstGetGlobal(globalRef);
    if (!getGlobal)
        return false;

    if (!c.push(AstDecodeStackItem(getGlobal)))
        return false;

    return true;
}

static bool
AstDecodeSetGlobal(AstDecodeContext& c)
{
    uint32_t globalId;
    if (!c.iter().readSetGlobal(&globalId, nullptr))
        return false;

    AstDecodeStackItem value = c.popCopy();

    AstRef globalRef;
    if (!GenerateRef(c, AstName(u"global"), globalId, &globalRef))
        return false;

    auto* setGlobal = new(c.lifo) AstSetGlobal(globalRef, *value.expr);
    if (!setGlobal)
        return false;

    AstExpr* expr = c.handleVoidExpr(setGlobal);
    if (!expr)
        return false;

    if (!c.push(AstDecodeStackItem(expr)))
        return false;

    return true;
}

static bool
AstDecodeReturn(AstDecodeContext& c)
{
    if (!c.iter().readReturn(nullptr))
        return false;

    AstDecodeStackItem result;
    if (!IsVoid(c.retType()))
       result = c.popCopy();

    AstReturn* ret = new(c.lifo) AstReturn(result.expr);
    if (!ret)
        return false;

    if (!c.push(AstDecodeStackItem(ret)))
        return false;

    return true;
}

static bool
AstDecodeAtomicLoad(AstDecodeContext& c, ThreadOp op)
{
    ValType type;
    uint32_t byteSize;
    switch (op) {
      case ThreadOp::I32AtomicLoad:    type = ValType::I32; byteSize = 4; break;
      case ThreadOp::I64AtomicLoad:    type = ValType::I64; byteSize = 8; break;
      case ThreadOp::I32AtomicLoad8U:  type = ValType::I32; byteSize = 1; break;
      case ThreadOp::I32AtomicLoad16U: type = ValType::I32; byteSize = 2; break;
      case ThreadOp::I64AtomicLoad8U:  type = ValType::I64; byteSize = 1; break;
      case ThreadOp::I64AtomicLoad16U: type = ValType::I64; byteSize = 2; break;
      case ThreadOp::I64AtomicLoad32U: type = ValType::I64; byteSize = 4; break;
      default:
        MOZ_CRASH("Should not happen");
    }

    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readAtomicLoad(&addr, type, byteSize))
        return false;

    AstDecodeStackItem item = c.popCopy();

    AstAtomicLoad* load = new(c.lifo) AstAtomicLoad(op, AstDecodeLoadStoreAddress(addr, item));
    if (!load)
        return false;

    if (!c.push(AstDecodeStackItem(load)))
        return false;

    return true;
}

static bool
AstDecodeAtomicStore(AstDecodeContext& c, ThreadOp op)
{
    ValType type;
    uint32_t byteSize;
    switch (op) {
      case ThreadOp::I32AtomicStore:    type = ValType::I32; byteSize = 4; break;
      case ThreadOp::I64AtomicStore:    type = ValType::I64; byteSize = 8; break;
      case ThreadOp::I32AtomicStore8U:  type = ValType::I32; byteSize = 1; break;
      case ThreadOp::I32AtomicStore16U: type = ValType::I32; byteSize = 2; break;
      case ThreadOp::I64AtomicStore8U:  type = ValType::I64; byteSize = 1; break;
      case ThreadOp::I64AtomicStore16U: type = ValType::I64; byteSize = 2; break;
      case ThreadOp::I64AtomicStore32U: type = ValType::I64; byteSize = 4; break;
      default:
        MOZ_CRASH("Should not happen");
    }

    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readAtomicStore(&addr, type, byteSize, &nothing))
        return false;

    AstDecodeStackItem value = c.popCopy();
    AstDecodeStackItem item = c.popCopy();

    AstAtomicStore* store = new(c.lifo) AstAtomicStore(op, AstDecodeLoadStoreAddress(addr, item), value.expr);
    if (!store)
        return false;

    AstExpr* wrapped = c.handleVoidExpr(store);
    if (!wrapped)
        return false;

    if (!c.push(AstDecodeStackItem(wrapped)))
        return false;

    return true;
}

static bool
AstDecodeAtomicRMW(AstDecodeContext& c, ThreadOp op)
{
    ValType type;
    uint32_t byteSize;
    switch (op) {
      case ThreadOp::I32AtomicAdd:
      case ThreadOp::I32AtomicSub:
      case ThreadOp::I32AtomicAnd:
      case ThreadOp::I32AtomicOr:
      case ThreadOp::I32AtomicXor:
      case ThreadOp::I32AtomicXchg:
        type = ValType::I32;
        byteSize = 4;
        break;
      case ThreadOp::I64AtomicAdd:
      case ThreadOp::I64AtomicSub:
      case ThreadOp::I64AtomicAnd:
      case ThreadOp::I64AtomicOr:
      case ThreadOp::I64AtomicXor:
      case ThreadOp::I64AtomicXchg:
        type = ValType::I64;
        byteSize = 8;
        break;
      case ThreadOp::I32AtomicAdd8U:
      case ThreadOp::I32AtomicSub8U:
      case ThreadOp::I32AtomicOr8U:
      case ThreadOp::I32AtomicXor8U:
      case ThreadOp::I32AtomicXchg8U:
      case ThreadOp::I32AtomicAnd8U:
        type = ValType::I32;
        byteSize = 1;
        break;
      case ThreadOp::I32AtomicAdd16U:
      case ThreadOp::I32AtomicSub16U:
      case ThreadOp::I32AtomicAnd16U:
      case ThreadOp::I32AtomicOr16U:
      case ThreadOp::I32AtomicXor16U:
      case ThreadOp::I32AtomicXchg16U:
        type = ValType::I32;
        byteSize = 2;
        break;
      case ThreadOp::I64AtomicAdd8U:
      case ThreadOp::I64AtomicSub8U:
      case ThreadOp::I64AtomicAnd8U:
      case ThreadOp::I64AtomicOr8U:
      case ThreadOp::I64AtomicXor8U:
      case ThreadOp::I64AtomicXchg8U:
        type = ValType::I64;
        byteSize = 1;
        break;
      case ThreadOp::I64AtomicAdd16U:
      case ThreadOp::I64AtomicSub16U:
      case ThreadOp::I64AtomicAnd16U:
      case ThreadOp::I64AtomicOr16U:
      case ThreadOp::I64AtomicXor16U:
      case ThreadOp::I64AtomicXchg16U:
        type = ValType::I64;
        byteSize = 2;
        break;
      case ThreadOp::I64AtomicAdd32U:
      case ThreadOp::I64AtomicSub32U:
      case ThreadOp::I64AtomicAnd32U:
      case ThreadOp::I64AtomicOr32U:
      case ThreadOp::I64AtomicXor32U:
      case ThreadOp::I64AtomicXchg32U:
        type = ValType::I64;
        byteSize = 4;
        break;
      default:
        MOZ_CRASH("Should not happen");
    }

    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readAtomicRMW(&addr, type, byteSize, &nothing))
        return false;

    AstDecodeStackItem value = c.popCopy();
    AstDecodeStackItem item = c.popCopy();

    AstAtomicRMW* rmw = new(c.lifo) AstAtomicRMW(op, AstDecodeLoadStoreAddress(addr, item),
                                                 value.expr);
    if (!rmw)
        return false;

    if (!c.push(AstDecodeStackItem(rmw)))
        return false;

    return true;
}

static bool
AstDecodeAtomicCmpXchg(AstDecodeContext& c, ThreadOp op)
{
    ValType type;
    uint32_t byteSize;
    switch (op) {
      case ThreadOp::I32AtomicCmpXchg:    type = ValType::I32; byteSize = 4; break;
      case ThreadOp::I64AtomicCmpXchg:    type = ValType::I64; byteSize = 8; break;
      case ThreadOp::I32AtomicCmpXchg8U:  type = ValType::I32; byteSize = 1; break;
      case ThreadOp::I32AtomicCmpXchg16U: type = ValType::I32; byteSize = 2; break;
      case ThreadOp::I64AtomicCmpXchg8U:  type = ValType::I64; byteSize = 1; break;
      case ThreadOp::I64AtomicCmpXchg16U: type = ValType::I64; byteSize = 2; break;
      case ThreadOp::I64AtomicCmpXchg32U: type = ValType::I64; byteSize = 4; break;
      default:
        MOZ_CRASH("Should not happen");
    }

    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readAtomicCmpXchg(&addr, type, byteSize, &nothing, &nothing))
        return false;

    AstDecodeStackItem replacement = c.popCopy();
    AstDecodeStackItem expected = c.popCopy();
    AstDecodeStackItem item = c.popCopy();

    AstAtomicCmpXchg* cmpxchg =
        new(c.lifo) AstAtomicCmpXchg(op, AstDecodeLoadStoreAddress(addr, item), expected.expr,
                                     replacement.expr);
    if (!cmpxchg)
        return false;

    if (!c.push(AstDecodeStackItem(cmpxchg)))
        return false;

    return true;
}

static bool
AstDecodeWait(AstDecodeContext& c, ThreadOp op)
{
    ValType type;
    uint32_t byteSize;
    switch (op) {
      case ThreadOp::I32Wait: type = ValType::I32; byteSize = 4; break;
      case ThreadOp::I64Wait: type = ValType::I64; byteSize = 8; break;
      default:
        MOZ_CRASH("Should not happen");
    }

    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readWait(&addr, type, byteSize, &nothing, &nothing))
        return false;

    AstDecodeStackItem timeout = c.popCopy();
    AstDecodeStackItem value = c.popCopy();
    AstDecodeStackItem item = c.popCopy();

    AstWait* wait = new(c.lifo) AstWait(op, AstDecodeLoadStoreAddress(addr, item), value.expr,
                                        timeout.expr);
    if (!wait)
        return false;

    if (!c.push(AstDecodeStackItem(wait)))
        return false;

    return true;
}

static bool
AstDecodeWake(AstDecodeContext& c)
{
    Nothing nothing;
    LinearMemoryAddress<Nothing> addr;
    if (!c.iter().readWake(&addr, &nothing))
        return false;

    AstDecodeStackItem count = c.popCopy();
    AstDecodeStackItem item = c.popCopy();

    AstWake* wake = new(c.lifo) AstWake(AstDecodeLoadStoreAddress(addr, item), count.expr);
    if (!wake)
        return false;

    if (!c.push(AstDecodeStackItem(wake)))
        return false;

    return true;
}

static bool
AstDecodeExpr(AstDecodeContext& c)
{
    uint32_t exprOffset = c.iter().currentOffset();
    OpBytes op;
    if (!c.iter().readOp(&op))
        return false;

    AstExpr* tmp;
    switch (op.b0) {
      case uint16_t(Op::Nop):
        if (!AstDecodeNop(c))
            return false;
        break;
      case uint16_t(Op::Drop):
        if (!AstDecodeDrop(c))
            return false;
        break;
      case uint16_t(Op::Call):
        if (!AstDecodeCall(c))
            return false;
        break;
      case uint16_t(Op::CallIndirect):
        if (!AstDecodeCallIndirect(c))
            return false;
        break;
      case uint16_t(Op::I32Const):
        int32_t i32;
        if (!c.iter().readI32Const(&i32))
            return false;
        tmp = new(c.lifo) AstConst(Val((uint32_t)i32));
        if (!tmp || !c.push(AstDecodeStackItem(tmp)))
            return false;
        break;
      case uint16_t(Op::I64Const):
        int64_t i64;
        if (!c.iter().readI64Const(&i64))
            return false;
        tmp = new(c.lifo) AstConst(Val((uint64_t)i64));
        if (!tmp || !c.push(AstDecodeStackItem(tmp)))
            return false;
        break;
      case uint16_t(Op::F32Const): {
        float f32;
        if (!c.iter().readF32Const(&f32))
            return false;
        tmp = new(c.lifo) AstConst(Val(f32));
        if (!tmp || !c.push(AstDecodeStackItem(tmp)))
            return false;
        break;
      }
      case uint16_t(Op::F64Const): {
        double f64;
        if (!c.iter().readF64Const(&f64))
            return false;
        tmp = new(c.lifo) AstConst(Val(f64));
        if (!tmp || !c.push(AstDecodeStackItem(tmp)))
            return false;
        break;
      }
      case uint16_t(Op::GetLocal):
        if (!AstDecodeGetLocal(c))
            return false;
        break;
      case uint16_t(Op::SetLocal):
        if (!AstDecodeSetLocal(c))
            return false;
        break;
      case uint16_t(Op::TeeLocal):
        if (!AstDecodeTeeLocal(c))
            return false;
        break;
      case uint16_t(Op::Select):
        if (!AstDecodeSelect(c))
            return false;
        break;
      case uint16_t(Op::Block):
      case uint16_t(Op::Loop):
        if (!AstDecodeBlock(c, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::If):
        if (!AstDecodeIf(c))
            return false;
        break;
      case uint16_t(Op::Else):
        if (!AstDecodeElse(c))
            return false;
        break;
      case uint16_t(Op::End):
        if (!AstDecodeEnd(c))
            return false;
        break;
      case uint16_t(Op::I32Clz):
      case uint16_t(Op::I32Ctz):
      case uint16_t(Op::I32Popcnt):
        if (!AstDecodeUnary(c, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Clz):
      case uint16_t(Op::I64Ctz):
      case uint16_t(Op::I64Popcnt):
        if (!AstDecodeUnary(c, ValType::I64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32Abs):
      case uint16_t(Op::F32Neg):
      case uint16_t(Op::F32Ceil):
      case uint16_t(Op::F32Floor):
      case uint16_t(Op::F32Sqrt):
      case uint16_t(Op::F32Trunc):
      case uint16_t(Op::F32Nearest):
        if (!AstDecodeUnary(c, ValType::F32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64Abs):
      case uint16_t(Op::F64Neg):
      case uint16_t(Op::F64Ceil):
      case uint16_t(Op::F64Floor):
      case uint16_t(Op::F64Sqrt):
      case uint16_t(Op::F64Trunc):
      case uint16_t(Op::F64Nearest):
        if (!AstDecodeUnary(c, ValType::F64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Add):
      case uint16_t(Op::I32Sub):
      case uint16_t(Op::I32Mul):
      case uint16_t(Op::I32DivS):
      case uint16_t(Op::I32DivU):
      case uint16_t(Op::I32RemS):
      case uint16_t(Op::I32RemU):
      case uint16_t(Op::I32And):
      case uint16_t(Op::I32Or):
      case uint16_t(Op::I32Xor):
      case uint16_t(Op::I32Shl):
      case uint16_t(Op::I32ShrS):
      case uint16_t(Op::I32ShrU):
      case uint16_t(Op::I32Rotl):
      case uint16_t(Op::I32Rotr):
        if (!AstDecodeBinary(c, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Add):
      case uint16_t(Op::I64Sub):
      case uint16_t(Op::I64Mul):
      case uint16_t(Op::I64DivS):
      case uint16_t(Op::I64DivU):
      case uint16_t(Op::I64RemS):
      case uint16_t(Op::I64RemU):
      case uint16_t(Op::I64And):
      case uint16_t(Op::I64Or):
      case uint16_t(Op::I64Xor):
      case uint16_t(Op::I64Shl):
      case uint16_t(Op::I64ShrS):
      case uint16_t(Op::I64ShrU):
      case uint16_t(Op::I64Rotl):
      case uint16_t(Op::I64Rotr):
        if (!AstDecodeBinary(c, ValType::I64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32Add):
      case uint16_t(Op::F32Sub):
      case uint16_t(Op::F32Mul):
      case uint16_t(Op::F32Div):
      case uint16_t(Op::F32Min):
      case uint16_t(Op::F32Max):
      case uint16_t(Op::F32CopySign):
        if (!AstDecodeBinary(c, ValType::F32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64Add):
      case uint16_t(Op::F64Sub):
      case uint16_t(Op::F64Mul):
      case uint16_t(Op::F64Div):
      case uint16_t(Op::F64Min):
      case uint16_t(Op::F64Max):
      case uint16_t(Op::F64CopySign):
        if (!AstDecodeBinary(c, ValType::F64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Eq):
      case uint16_t(Op::I32Ne):
      case uint16_t(Op::I32LtS):
      case uint16_t(Op::I32LtU):
      case uint16_t(Op::I32LeS):
      case uint16_t(Op::I32LeU):
      case uint16_t(Op::I32GtS):
      case uint16_t(Op::I32GtU):
      case uint16_t(Op::I32GeS):
      case uint16_t(Op::I32GeU):
        if (!AstDecodeComparison(c, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Eq):
      case uint16_t(Op::I64Ne):
      case uint16_t(Op::I64LtS):
      case uint16_t(Op::I64LtU):
      case uint16_t(Op::I64LeS):
      case uint16_t(Op::I64LeU):
      case uint16_t(Op::I64GtS):
      case uint16_t(Op::I64GtU):
      case uint16_t(Op::I64GeS):
      case uint16_t(Op::I64GeU):
        if (!AstDecodeComparison(c, ValType::I64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32Eq):
      case uint16_t(Op::F32Ne):
      case uint16_t(Op::F32Lt):
      case uint16_t(Op::F32Le):
      case uint16_t(Op::F32Gt):
      case uint16_t(Op::F32Ge):
        if (!AstDecodeComparison(c, ValType::F32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64Eq):
      case uint16_t(Op::F64Ne):
      case uint16_t(Op::F64Lt):
      case uint16_t(Op::F64Le):
      case uint16_t(Op::F64Gt):
      case uint16_t(Op::F64Ge):
        if (!AstDecodeComparison(c, ValType::F64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Eqz):
        if (!AstDecodeConversion(c, ValType::I32, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Eqz):
      case uint16_t(Op::I32WrapI64):
        if (!AstDecodeConversion(c, ValType::I64, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32TruncSF32):
      case uint16_t(Op::I32TruncUF32):
      case uint16_t(Op::I32ReinterpretF32):
        if (!AstDecodeConversion(c, ValType::F32, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32TruncSF64):
      case uint16_t(Op::I32TruncUF64):
        if (!AstDecodeConversion(c, ValType::F64, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64ExtendSI32):
      case uint16_t(Op::I64ExtendUI32):
        if (!AstDecodeConversion(c, ValType::I32, ValType::I64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64TruncSF32):
      case uint16_t(Op::I64TruncUF32):
        if (!AstDecodeConversion(c, ValType::F32, ValType::I64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64TruncSF64):
      case uint16_t(Op::I64TruncUF64):
      case uint16_t(Op::I64ReinterpretF64):
        if (!AstDecodeConversion(c, ValType::F64, ValType::I64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32ConvertSI32):
      case uint16_t(Op::F32ConvertUI32):
      case uint16_t(Op::F32ReinterpretI32):
        if (!AstDecodeConversion(c, ValType::I32, ValType::F32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32ConvertSI64):
      case uint16_t(Op::F32ConvertUI64):
        if (!AstDecodeConversion(c, ValType::I64, ValType::F32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32DemoteF64):
        if (!AstDecodeConversion(c, ValType::F64, ValType::F32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64ConvertSI32):
      case uint16_t(Op::F64ConvertUI32):
        if (!AstDecodeConversion(c, ValType::I32, ValType::F64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64ConvertSI64):
      case uint16_t(Op::F64ConvertUI64):
      case uint16_t(Op::F64ReinterpretI64):
        if (!AstDecodeConversion(c, ValType::I64, ValType::F64, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64PromoteF32):
        if (!AstDecodeConversion(c, ValType::F32, ValType::F64, Op(op.b0)))
            return false;
        break;
#ifdef ENABLE_WASM_SIGNEXTEND_OPS
      case uint16_t(Op::I32Extend8S):
      case uint16_t(Op::I32Extend16S):
        if (!AstDecodeConversion(c, ValType::I32, ValType::I32, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Extend8S):
      case uint16_t(Op::I64Extend16S):
      case uint16_t(Op::I64Extend32S):
        if (!AstDecodeConversion(c, ValType::I64, ValType::I64, Op(op.b0)))
            return false;
        break;
#endif
      case uint16_t(Op::I32Load8S):
      case uint16_t(Op::I32Load8U):
        if (!AstDecodeLoad(c, ValType::I32, 1, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Load16S):
      case uint16_t(Op::I32Load16U):
        if (!AstDecodeLoad(c, ValType::I32, 2, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Load):
        if (!AstDecodeLoad(c, ValType::I32, 4, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Load8S):
      case uint16_t(Op::I64Load8U):
        if (!AstDecodeLoad(c, ValType::I64, 1, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Load16S):
      case uint16_t(Op::I64Load16U):
        if (!AstDecodeLoad(c, ValType::I64, 2, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Load32S):
      case uint16_t(Op::I64Load32U):
        if (!AstDecodeLoad(c, ValType::I64, 4, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Load):
        if (!AstDecodeLoad(c, ValType::I64, 8, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32Load):
        if (!AstDecodeLoad(c, ValType::F32, 4, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64Load):
        if (!AstDecodeLoad(c, ValType::F64, 8, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Store8):
        if (!AstDecodeStore(c, ValType::I32, 1, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Store16):
        if (!AstDecodeStore(c, ValType::I32, 2, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I32Store):
        if (!AstDecodeStore(c, ValType::I32, 4, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Store8):
        if (!AstDecodeStore(c, ValType::I64, 1, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Store16):
        if (!AstDecodeStore(c, ValType::I64, 2, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Store32):
        if (!AstDecodeStore(c, ValType::I64, 4, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::I64Store):
        if (!AstDecodeStore(c, ValType::I64, 8, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F32Store):
        if (!AstDecodeStore(c, ValType::F32, 4, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::F64Store):
        if (!AstDecodeStore(c, ValType::F64, 8, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::CurrentMemory):
        if (!AstDecodeCurrentMemory(c))
            return false;
        break;
      case uint16_t(Op::GrowMemory):
        if (!AstDecodeGrowMemory(c))
            return false;
        break;
      case uint16_t(Op::SetGlobal):
        if (!AstDecodeSetGlobal(c))
            return false;
        break;
      case uint16_t(Op::GetGlobal):
        if (!AstDecodeGetGlobal(c))
            return false;
        break;
      case uint16_t(Op::Br):
      case uint16_t(Op::BrIf):
        if (!AstDecodeBranch(c, Op(op.b0)))
            return false;
        break;
      case uint16_t(Op::BrTable):
        if (!AstDecodeBrTable(c))
            return false;
        break;
      case uint16_t(Op::Return):
        if (!AstDecodeReturn(c))
            return false;
        break;
      case uint16_t(Op::Unreachable):
        if (!c.iter().readUnreachable())
            return false;
        tmp = new(c.lifo) AstUnreachable();
        if (!tmp)
            return false;
        if (!c.push(AstDecodeStackItem(tmp)))
            return false;
        break;
#ifdef ENABLE_WASM_SATURATING_TRUNC_OPS
      case uint16_t(Op::NumericPrefix):
        switch (op.b1) {
          case uint16_t(NumericOp::I32TruncSSatF32):
          case uint16_t(NumericOp::I32TruncUSatF32):
            if (!AstDecodeExtraConversion(c, ValType::F32, ValType::I32, NumericOp(op.b1)))
                return false;
            break;
          case uint16_t(NumericOp::I32TruncSSatF64):
          case uint16_t(NumericOp::I32TruncUSatF64):
            if (!AstDecodeExtraConversion(c, ValType::F64, ValType::I32, NumericOp(op.b1)))
                return false;
            break;
          case uint16_t(NumericOp::I64TruncSSatF32):
          case uint16_t(NumericOp::I64TruncUSatF32):
            if (!AstDecodeExtraConversion(c, ValType::F32, ValType::I64, NumericOp(op.b1)))
                return false;
            break;
          case uint16_t(NumericOp::I64TruncSSatF64):
          case uint16_t(NumericOp::I64TruncUSatF64):
            if (!AstDecodeExtraConversion(c, ValType::F64, ValType::I64, NumericOp(op.b1)))
                return false;
            break;
          default:
            return c.iter().unrecognizedOpcode(&op);
        }
        break;
#endif
      case uint16_t(Op::ThreadPrefix):
        switch (op.b1) {
          case uint16_t(ThreadOp::Wake):
            if (!AstDecodeWake(c))
                return false;
            break;
          case uint16_t(ThreadOp::I32Wait):
          case uint16_t(ThreadOp::I64Wait):
            if (!AstDecodeWait(c, ThreadOp(op.b1)))
                return false;
            break;
          case uint16_t(ThreadOp::I32AtomicLoad):
          case uint16_t(ThreadOp::I64AtomicLoad):
          case uint16_t(ThreadOp::I32AtomicLoad8U):
          case uint16_t(ThreadOp::I32AtomicLoad16U):
          case uint16_t(ThreadOp::I64AtomicLoad8U):
          case uint16_t(ThreadOp::I64AtomicLoad16U):
          case uint16_t(ThreadOp::I64AtomicLoad32U):
            if (!AstDecodeAtomicLoad(c, ThreadOp(op.b1)))
                return false;
            break;
          case uint16_t(ThreadOp::I32AtomicStore):
          case uint16_t(ThreadOp::I64AtomicStore):
          case uint16_t(ThreadOp::I32AtomicStore8U):
          case uint16_t(ThreadOp::I32AtomicStore16U):
          case uint16_t(ThreadOp::I64AtomicStore8U):
          case uint16_t(ThreadOp::I64AtomicStore16U):
          case uint16_t(ThreadOp::I64AtomicStore32U):
            if (!AstDecodeAtomicStore(c, ThreadOp(op.b1)))
                return false;
            break;
          case uint16_t(ThreadOp::I32AtomicAdd):
          case uint16_t(ThreadOp::I64AtomicAdd):
          case uint16_t(ThreadOp::I32AtomicAdd8U):
          case uint16_t(ThreadOp::I32AtomicAdd16U):
          case uint16_t(ThreadOp::I64AtomicAdd8U):
          case uint16_t(ThreadOp::I64AtomicAdd16U):
          case uint16_t(ThreadOp::I64AtomicAdd32U):
          case uint16_t(ThreadOp::I32AtomicSub):
          case uint16_t(ThreadOp::I64AtomicSub):
          case uint16_t(ThreadOp::I32AtomicSub8U):
          case uint16_t(ThreadOp::I32AtomicSub16U):
          case uint16_t(ThreadOp::I64AtomicSub8U):
          case uint16_t(ThreadOp::I64AtomicSub16U):
          case uint16_t(ThreadOp::I64AtomicSub32U):
          case uint16_t(ThreadOp::I32AtomicAnd):
          case uint16_t(ThreadOp::I64AtomicAnd):
          case uint16_t(ThreadOp::I32AtomicAnd8U):
          case uint16_t(ThreadOp::I32AtomicAnd16U):
          case uint16_t(ThreadOp::I64AtomicAnd8U):
          case uint16_t(ThreadOp::I64AtomicAnd16U):
          case uint16_t(ThreadOp::I64AtomicAnd32U):
          case uint16_t(ThreadOp::I32AtomicOr):
          case uint16_t(ThreadOp::I64AtomicOr):
          case uint16_t(ThreadOp::I32AtomicOr8U):
          case uint16_t(ThreadOp::I32AtomicOr16U):
          case uint16_t(ThreadOp::I64AtomicOr8U):
          case uint16_t(ThreadOp::I64AtomicOr16U):
          case uint16_t(ThreadOp::I64AtomicOr32U):
          case uint16_t(ThreadOp::I32AtomicXor):
          case uint16_t(ThreadOp::I64AtomicXor):
          case uint16_t(ThreadOp::I32AtomicXor8U):
          case uint16_t(ThreadOp::I32AtomicXor16U):
          case uint16_t(ThreadOp::I64AtomicXor8U):
          case uint16_t(ThreadOp::I64AtomicXor16U):
          case uint16_t(ThreadOp::I64AtomicXor32U):
          case uint16_t(ThreadOp::I32AtomicXchg):
          case uint16_t(ThreadOp::I64AtomicXchg):
          case uint16_t(ThreadOp::I32AtomicXchg8U):
          case uint16_t(ThreadOp::I32AtomicXchg16U):
          case uint16_t(ThreadOp::I64AtomicXchg8U):
          case uint16_t(ThreadOp::I64AtomicXchg16U):
          case uint16_t(ThreadOp::I64AtomicXchg32U):
            if (!AstDecodeAtomicRMW(c, ThreadOp(op.b1)))
                return false;
            break;
          case uint16_t(ThreadOp::I32AtomicCmpXchg):
          case uint16_t(ThreadOp::I64AtomicCmpXchg):
          case uint16_t(ThreadOp::I32AtomicCmpXchg8U):
          case uint16_t(ThreadOp::I32AtomicCmpXchg16U):
          case uint16_t(ThreadOp::I64AtomicCmpXchg8U):
          case uint16_t(ThreadOp::I64AtomicCmpXchg16U):
          case uint16_t(ThreadOp::I64AtomicCmpXchg32U):
            if (!AstDecodeAtomicCmpXchg(c, ThreadOp(op.b1)))
                return false;
            break;
          default:
            return c.iter().unrecognizedOpcode(&op);
        }
        break;
      case uint16_t(Op::MozPrefix):
        return c.iter().unrecognizedOpcode(&op);
      default:
        return c.iter().unrecognizedOpcode(&op);
    }

    AstExpr* lastExpr = c.top().expr;
    if (lastExpr) {
        // If last node is a 'first' node, the offset must assigned to it
        // last child.
        if (lastExpr->kind() == AstExprKind::First)
            lastExpr->as<AstFirst>().exprs().back()->setOffset(exprOffset);
        else
            lastExpr->setOffset(exprOffset);
    }
    return true;
}

static bool
AstDecodeFunctionBody(AstDecodeContext &c, uint32_t funcIndex, AstFunc** func)
{
    uint32_t offset = c.d.currentOffset();
    uint32_t bodySize;
    if (!c.d.readVarU32(&bodySize))
        return c.d.fail("expected number of function body bytes");

    if (c.d.bytesRemain() < bodySize)
        return c.d.fail("function body length too big");

    const uint8_t* bodyBegin = c.d.currentPosition();
    const uint8_t* bodyEnd = bodyBegin + bodySize;

    const SigWithId* sig = c.env().funcSigs[funcIndex];

    ValTypeVector locals;
    if (!locals.appendAll(sig->args()))
        return false;

    if (!DecodeLocalEntries(c.d, ModuleKind::Wasm, &locals))
        return false;

    AstDecodeOpIter iter(c.env(), c.d);
    c.startFunction(&iter, &locals, sig->ret());

    AstName funcName;
    if (!GenerateName(c, AstName(u"func"), funcIndex, &funcName))
        return false;

    uint32_t numParams = sig->args().length();
    uint32_t numLocals = locals.length();

    AstValTypeVector vars(c.lifo);
    for (uint32_t i = numParams; i < numLocals; i++) {
        if (!vars.append(locals[i]))
            return false;
    }

    AstNameVector localsNames(c.lifo);
    for (uint32_t i = 0; i < numLocals; i++) {
        AstName varName;
        if (!GenerateName(c, AstName(u"var"), i, &varName))
            return false;
        if (!localsNames.append(varName))
            return false;
    }

    if (!c.iter().readFunctionStart(sig->ret()))
        return false;

    if (!c.depths().append(c.exprs().length()))
        return false;

    uint32_t endOffset = offset;
    while (c.d.currentPosition() < bodyEnd) {
        if (!AstDecodeExpr(c))
            return false;

        const AstDecodeStackItem& item = c.top();
        if (!item.expr) { // Op::End was found
            c.popBack();
            break;
        }

        endOffset = c.d.currentOffset();
    }

    AstExprVector body(c.lifo);
    for (auto i = c.exprs().begin() + c.depths().back(), e = c.exprs().end(); i != e; ++i) {
        if (!body.append(i->expr))
            return false;
    }
    c.exprs().shrinkTo(c.depths().popCopy());

    if (!c.iter().readFunctionEnd(bodyEnd))
        return false;

    c.endFunction();

    if (c.d.currentPosition() != bodyEnd)
        return c.d.fail("function body length mismatch");

    size_t sigIndex = c.env().funcIndexToSigIndex(funcIndex);

    AstRef sigRef;
    if (!GenerateRef(c, AstName(u"type"), sigIndex, &sigRef))
        return false;

    *func = new(c.lifo) AstFunc(funcName, sigRef, Move(vars), Move(localsNames), Move(body));
    if (!*func)
        return false;
    (*func)->setOffset(offset);
    (*func)->setEndOffset(endOffset);

    return true;
}

/*****************************************************************************/
// wasm decoding and generation

static bool
AstCreateSignatures(AstDecodeContext& c)
{
    SigWithIdVector& sigs = c.env().sigs;

    for (size_t sigIndex = 0; sigIndex < sigs.length(); sigIndex++) {
        const Sig& sig = sigs[sigIndex];

        AstValTypeVector args(c.lifo);
        if (!args.appendAll(sig.args()))
            return false;

        AstSig sigNoName(Move(args), sig.ret());

        AstName sigName;
        if (!GenerateName(c, AstName(u"type"), sigIndex, &sigName))
            return false;

        AstSig* astSig = new(c.lifo) AstSig(sigName, Move(sigNoName));
        if (!astSig || !c.module().append(astSig))
            return false;
    }

    return true;
}

static bool
ToAstName(AstDecodeContext& c, const char* name, AstName* out)
{
    size_t len = strlen(name);
    char16_t* buffer = static_cast<char16_t *>(c.lifo.alloc(len * sizeof(char16_t)));
    if (!buffer)
        return false;

    for (size_t i = 0; i < len; i++)
        buffer[i] = name[i];

    *out = AstName(buffer, len);
    return true;
}

static bool
AstCreateImports(AstDecodeContext& c)
{
    size_t lastFunc = 0;
    size_t lastGlobal = 0;
    size_t lastTable = 0;
    size_t lastMemory = 0;

    Maybe<Limits> memory;
    if (c.env().usesMemory()) {
        memory = Some(Limits(c.env().minMemoryLength,
                             c.env().maxMemoryLength,
                             c.env().memoryUsage == MemoryUsage::Shared
                               ? Shareable::True
                               : Shareable::False));
    }

    for (size_t importIndex = 0; importIndex < c.env().imports.length(); importIndex++) {
        const Import& import = c.env().imports[importIndex];

        AstName moduleName;
        if (!ToAstName(c, import.module.get(), &moduleName))
            return false;

        AstName fieldName;
        if (!ToAstName(c, import.field.get(), &fieldName))
            return false;

        AstImport* ast = nullptr;
        switch (import.kind) {
          case DefinitionKind::Function: {
            AstName importName;
            if (!GenerateName(c, AstName(u"import"), lastFunc, &importName))
                return false;

            size_t sigIndex = c.env().funcIndexToSigIndex(lastFunc);

            AstRef sigRef;
            if (!GenerateRef(c, AstName(u"type"), sigIndex, &sigRef))
                return false;

            ast = new(c.lifo) AstImport(importName, moduleName, fieldName, sigRef);
            lastFunc++;
            break;
          }
          case DefinitionKind::Global: {
            AstName importName;
            if (!GenerateName(c, AstName(u"global"), lastGlobal, &importName))
                return false;

            const GlobalDesc& global = c.env().globals[lastGlobal];
            ValType type = global.type();
            bool isMutable = global.isMutable();

            ast = new(c.lifo) AstImport(importName, moduleName, fieldName,
                                        AstGlobal(importName, type, isMutable));
            lastGlobal++;
            break;
          }
          case DefinitionKind::Table: {
            AstName importName;
            if (!GenerateName(c, AstName(u"table"), lastTable, &importName))
                return false;

            ast = new(c.lifo) AstImport(importName, moduleName, fieldName, DefinitionKind::Table,
                                        c.env().tables[lastTable].limits);
            lastTable++;
            break;
          }
          case DefinitionKind::Memory: {
            AstName importName;
            if (!GenerateName(c, AstName(u"memory"), lastMemory, &importName))
                return false;

            ast = new(c.lifo) AstImport(importName, moduleName, fieldName, DefinitionKind::Memory,
                                        *memory);
            lastMemory++;
            break;
          }
        }

        if (!ast || !c.module().append(ast))
            return false;
    }

    return true;
}

static bool
AstCreateTables(AstDecodeContext& c)
{
    size_t numImported = c.module().tables().length();

    for (size_t i = numImported; i < c.env().tables.length(); i++) {
        AstName name;
        if (!GenerateName(c, AstName(u"table"), i, &name))
            return false;
        if (!c.module().addTable(name, c.env().tables[i].limits))
            return false;
    }

    return true;
}

static bool
AstCreateMemory(AstDecodeContext& c)
{
    bool importedMemory = !!c.module().memories().length();
    if (!c.env().usesMemory() || importedMemory)
        return true;

    AstName name;
    if (!GenerateName(c, AstName(u"memory"), c.module().memories().length(), &name))
        return false;

    return c.module().addMemory(name, Limits(c.env().minMemoryLength,
                                             c.env().maxMemoryLength,
                                             c.env().memoryUsage == MemoryUsage::Shared
                                               ? Shareable::True
                                               : Shareable::False));
}

static AstExpr*
ToAstExpr(AstDecodeContext& c, const InitExpr& initExpr)
{
    switch (initExpr.kind()) {
      case InitExpr::Kind::Constant: {
        return new(c.lifo) AstConst(Val(initExpr.val()));
      }
      case InitExpr::Kind::GetGlobal: {
        AstRef globalRef;
        if (!GenerateRef(c, AstName(u"global"), initExpr.globalIndex(), &globalRef))
            return nullptr;
        return new(c.lifo) AstGetGlobal(globalRef);
      }
    }
    return nullptr;
}

static bool
AstCreateGlobals(AstDecodeContext& c)
{
    for (uint32_t i = 0; i < c.env().globals.length(); i++) {
        const GlobalDesc& global = c.env().globals[i];
        if (global.isImport())
            continue;

        AstName name;
        if (!GenerateName(c, AstName(u"global"), i, &name))
            return false;

        AstExpr* init = global.isConstant()
                        ? new(c.lifo) AstConst(global.constantValue())
                        : ToAstExpr(c, global.initExpr());
        if (!init)
            return false;

        auto* g = new(c.lifo) AstGlobal(name, global.type(), global.isMutable(), Some(init));
        if (!g || !c.module().append(g))
            return false;
    }

    return true;
}

static bool
AstCreateExports(AstDecodeContext& c)
{
    for (const Export& exp : c.env().exports) {
        size_t index;
        switch (exp.kind()) {
          case DefinitionKind::Function: index = exp.funcIndex(); break;
          case DefinitionKind::Global: index = exp.globalIndex(); break;
          case DefinitionKind::Memory: index = 0; break;
          case DefinitionKind::Table: index = 0; break;
        }

        AstName name;
        if (!ToAstName(c, exp.fieldName(), &name))
            return false;

        AstExport* e = new(c.lifo) AstExport(name, exp.kind(), AstRef(index));
        if (!e || !c.module().append(e))
            return false;
    }

    return true;
}

static bool
AstCreateStartFunc(AstDecodeContext &c)
{
    if (!c.env().startFuncIndex)
        return true;

    AstRef funcRef;
    if (!GenerateFuncRef(c, *c.env().startFuncIndex, &funcRef))
        return false;

    c.module().setStartFunc(AstStartFunc(funcRef));
    return true;
}

static bool
AstCreateElems(AstDecodeContext &c)
{
    for (const ElemSegment& seg : c.env().elemSegments) {
        AstRefVector elems(c.lifo);
        if (!elems.reserve(seg.elemFuncIndices.length()))
            return false;

        for (uint32_t i : seg.elemFuncIndices)
            elems.infallibleAppend(AstRef(i));

        AstExpr* offset = ToAstExpr(c, seg.offset);
        if (!offset)
            return false;

        AstElemSegment* segment = new(c.lifo) AstElemSegment(offset, Move(elems));
        if (!segment || !c.module().append(segment))
            return false;
    }

    return true;
}

static bool
AstDecodeEnvironment(AstDecodeContext& c)
{
    if (!DecodeModuleEnvironment(c.d, &c.env()))
        return false;

    if (!AstCreateSignatures(c))
        return false;

    if (!AstCreateImports(c))
        return false;

    if (!AstCreateTables(c))
        return false;

    if (!AstCreateMemory(c))
        return false;

    if (!AstCreateGlobals(c))
        return false;

    if (!AstCreateExports(c))
        return false;

    if (!AstCreateStartFunc(c))
        return false;

    if (!AstCreateElems(c))
        return false;

    return true;
}

static bool
AstDecodeCodeSection(AstDecodeContext& c)
{
    if (!c.env().codeSection) {
        if (c.env().numFuncDefs() != 0)
            return c.d.fail("expected function bodies");
        return true;
    }

    uint32_t numFuncBodies;
    if (!c.d.readVarU32(&numFuncBodies))
        return c.d.fail("expected function body count");

    if (numFuncBodies != c.env().numFuncDefs())
        return c.d.fail("function body count does not match function signature count");

    for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncBodies; funcDefIndex++) {
        AstFunc* func;
        if (!AstDecodeFunctionBody(c, c.module().numFuncImports() + funcDefIndex, &func))
            return false;
        if (!c.module().append(func))
            return false;
    }

    return c.d.finishSection(*c.env().codeSection, "code");
}

// Number of bytes to display in a single fragment of a data section (per line).
static const size_t WRAP_DATA_BYTES = 30;

static bool
AstDecodeModuleTail(AstDecodeContext& c)
{
    MOZ_ASSERT(c.module().memories().length() <= 1, "at most one memory in MVP");

    if (!DecodeModuleTail(c.d, &c.env()))
        return false;

    for (DataSegment& s : c.env().dataSegments) {
        const uint8_t* src = c.d.begin() + s.bytecodeOffset;
        char16_t* buffer = static_cast<char16_t*>(c.lifo.alloc(s.length * sizeof(char16_t)));
        for (size_t i = 0; i < s.length; i++)
            buffer[i] = src[i];

        AstExpr* offset = ToAstExpr(c, s.offset);
        if (!offset)
            return false;

        AstNameVector fragments(c.lifo);
        for (size_t start = 0; start < s.length; start += WRAP_DATA_BYTES) {
            AstName name(buffer + start, Min(WRAP_DATA_BYTES, s.length - start));
            if (!fragments.append(name))
                return false;
        }

        AstDataSegment* segment = new(c.lifo) AstDataSegment(offset, Move(fragments));
        if (!segment || !c.module().append(segment))
            return false;
    }

    return true;
}

bool
wasm::BinaryToAst(JSContext* cx, const uint8_t* bytes, uint32_t length, LifoAlloc& lifo,
                  AstModule** module)
{
    AstModule* result = new(lifo) AstModule(lifo);
    if (!result || !result->init())
        return false;

    UniqueChars error;
    Decoder d(bytes, bytes + length, 0, &error, /* resilient */ true);
    AstDecodeContext c(cx, lifo, d, *result, true);

    if (!AstDecodeEnvironment(c) ||
        !AstDecodeCodeSection(c) ||
        !AstDecodeModuleTail(c))
    {
        if (error) {
            JS_ReportErrorNumberUTF8(c.cx, GetErrorMessage, nullptr, JSMSG_WASM_COMPILE_ERROR,
                                     error.get());
            return false;
        }
        ReportOutOfMemory(c.cx);
        return false;
    }

    MOZ_ASSERT(!error, "unreported error in decoding");

    *module = result;
    return true;
}
