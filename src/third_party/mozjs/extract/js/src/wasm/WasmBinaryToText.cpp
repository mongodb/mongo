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

#include "wasm/WasmBinaryToText.h"

#include "jsnum.h"

#include "util/StringBuffer.h"
#include "vm/ArrayBufferObject.h"
#include "wasm/WasmAST.h"
#include "wasm/WasmBinaryToAST.h"
#include "wasm/WasmDebug.h"
#include "wasm/WasmTextUtils.h"
#include "wasm/WasmTypes.h"

using namespace js;
using namespace js::wasm;

using mozilla::IsInfinite;
using mozilla::IsNaN;
using mozilla::IsNegativeZero;

struct WasmRenderContext
{
    JSContext* cx;
    AstModule* module;
    WasmPrintBuffer& buffer;
    GeneratedSourceMap* maybeSourceMap;
    uint32_t indent;
    uint32_t currentFuncIndex;

    WasmRenderContext(JSContext* cx, AstModule* module, WasmPrintBuffer& buffer,
                      GeneratedSourceMap* sourceMap)
      : cx(cx),
        module(module),
        buffer(buffer),
        maybeSourceMap(sourceMap),
        indent(0),
        currentFuncIndex(0)
    {}

    StringBuffer& sb() { return buffer.stringBuffer(); }
};

/*****************************************************************************/
// utilities

// Return true on purpose, so that we have a useful error message to provide to
// the user.
static bool
Fail(WasmRenderContext& c, const char* msg)
{
    c.buffer.stringBuffer().clear();

    return c.buffer.append("There was a problem when rendering the wasm text format: ") &&
           c.buffer.append(msg, strlen(msg)) &&
           c.buffer.append("\nYou should consider file a bug on Bugzilla in the "
                           "Core:::JavaScript Engine::JIT component at "
                           "https://bugzilla.mozilla.org/enter_bug.cgi.");
}

static bool
RenderIndent(WasmRenderContext& c)
{
    for (uint32_t i = 0; i < c.indent; i++) {
        if (!c.buffer.append("  "))
            return false;
    }
    return true;
}

static bool
RenderInt32(WasmRenderContext& c, int32_t num)
{
    return NumberValueToStringBuffer(c.cx, Int32Value(num), c.sb());
}

static bool
RenderInt64(WasmRenderContext& c, int64_t num)
{
    if (num < 0 && !c.buffer.append("-"))
        return false;
    if (!num)
        return c.buffer.append("0");
    return RenderInBase<10>(c.sb(), mozilla::Abs(num));
}

static bool
RenderDouble(WasmRenderContext& c, double d)
{
    if (IsNaN(d))
        return RenderNaN(c.sb(), d);
    if (IsNegativeZero(d))
        return c.buffer.append("-0");
    if (IsInfinite(d)) {
        if (d > 0)
            return c.buffer.append("infinity");
        return c.buffer.append("-infinity");
    }
    return NumberValueToStringBuffer(c.cx, DoubleValue(d), c.sb());
}

static bool
RenderFloat32(WasmRenderContext& c, float f)
{
    if (IsNaN(f))
        return RenderNaN(c.sb(), f);
    return RenderDouble(c, double(f));
}

static bool
RenderEscapedString(WasmRenderContext& c, const AstName& s)
{
    size_t length = s.length();
    const char16_t* p = s.begin();
    for (size_t i = 0; i < length; i++) {
        char16_t byte = p[i];
        switch (byte) {
          case '\n':
            if (!c.buffer.append("\\n"))
                return false;
            break;
          case '\r':
            if (!c.buffer.append("\\0d"))
                return false;
            break;
          case '\t':
            if (!c.buffer.append("\\t"))
                return false;
            break;
          case '\f':
            if (!c.buffer.append("\\0c"))
                return false;
            break;
          case '\b':
            if (!c.buffer.append("\\08"))
                return false;
            break;
          case '\\':
            if (!c.buffer.append("\\\\"))
                return false;
            break;
          case '"' :
            if (!c.buffer.append("\\\""))
                return false;
            break;
          case '\'':
            if (!c.buffer.append("\\'"))
                return false;
            break;
          default:
            if (byte >= 32 && byte < 127) {
                if (!c.buffer.append((char)byte))
                    return false;
            } else {
                char digit1 = byte / 16, digit2 = byte % 16;
                if (!c.buffer.append("\\"))
                    return false;
                if (!c.buffer.append((char)(digit1 < 10 ? digit1 + '0' : digit1 + 'a' - 10)))
                    return false;
                if (!c.buffer.append((char)(digit2 < 10 ? digit2 + '0' : digit2 + 'a' - 10)))
                    return false;
            }
            break;
        }
    }
    return true;
}

static bool
RenderExprType(WasmRenderContext& c, ExprType type)
{
    switch (type) {
      case ExprType::Void: return true; // ignoring void
      case ExprType::I32: return c.buffer.append("i32");
      case ExprType::I64: return c.buffer.append("i64");
      case ExprType::F32: return c.buffer.append("f32");
      case ExprType::F64: return c.buffer.append("f64");
      default:;
    }

    MOZ_CRASH("bad type");
}

static bool
RenderValType(WasmRenderContext& c, ValType type)
{
    return RenderExprType(c, ToExprType(type));
}

static bool
RenderName(WasmRenderContext& c, const AstName& name)
{
    return c.buffer.append(name.begin(), name.end());
}

static bool
RenderRef(WasmRenderContext& c, const AstRef& ref)
{
    if (ref.name().empty())
        return RenderInt32(c, ref.index());

    return RenderName(c, ref.name());
}

static bool
RenderBlockNameAndSignature(WasmRenderContext& c, const AstName& name, ExprType type)
{
    if (!name.empty()) {
        if (!c.buffer.append(' '))
            return false;

        if (!RenderName(c, name))
            return false;
    }

    if (!IsVoid(type)) {
        if (!c.buffer.append(' '))
            return false;

        if (!RenderExprType(c, type))
            return false;
    }

    return true;
}

static bool
RenderExpr(WasmRenderContext& c, AstExpr& expr, bool newLine = true);

#define MAP_AST_EXPR(c, expr)                                                         \
    if (c.maybeSourceMap) {                                                           \
        uint32_t lineno = c.buffer.lineno();                                          \
        uint32_t column = c.buffer.column();                                          \
        if (!c.maybeSourceMap->exprlocs().emplaceBack(lineno, column, expr.offset())) \
            return false;                                                             \
    }

/*****************************************************************************/
// binary format parsing and rendering

static bool
RenderNop(WasmRenderContext& c, AstNop& nop)
{
    if (!RenderIndent(c))
        return false;
    MAP_AST_EXPR(c, nop);
    return c.buffer.append("nop");
}

static bool
RenderDrop(WasmRenderContext& c, AstDrop& drop)
{
    if (!RenderExpr(c, drop.value()))
        return false;

    if (!RenderIndent(c))
        return false;
    MAP_AST_EXPR(c, drop);
    return c.buffer.append("drop");
}

static bool
RenderUnreachable(WasmRenderContext& c, AstUnreachable& unreachable)
{
    if (!RenderIndent(c))
        return false;
    MAP_AST_EXPR(c, unreachable);
    return c.buffer.append("unreachable");
}

static bool
RenderCallArgs(WasmRenderContext& c, const AstExprVector& args)
{
    for (uint32_t i = 0; i < args.length(); i++) {
        if (!RenderExpr(c, *args[i]))
            return false;
    }

    return true;
}

static bool
RenderCall(WasmRenderContext& c, AstCall& call)
{
    if (!RenderCallArgs(c, call.args()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, call);
    if (call.op() == Op::Call) {
        if (!c.buffer.append("call "))
            return false;
    } else {
        return Fail(c, "unexpected operator");
    }

    return RenderRef(c, call.func());
}

static bool
RenderCallIndirect(WasmRenderContext& c, AstCallIndirect& call)
{
    if (!RenderCallArgs(c, call.args()))
        return false;

    if (!RenderExpr(c, *call.index()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, call);
    if (!c.buffer.append("call_indirect "))
        return false;
    return RenderRef(c, call.sig());
}

static bool
RenderConst(WasmRenderContext& c, AstConst& cst)
{
    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, cst);
    if (!RenderValType(c, cst.val().type()))
        return false;
    if (!c.buffer.append(".const "))
        return false;

    switch (ToExprType(cst.val().type())) {
      case ExprType::I32:
        return RenderInt32(c, (int32_t)cst.val().i32());
      case ExprType::I64:
        return RenderInt64(c, (int64_t)cst.val().i64());
      case ExprType::F32:
        return RenderFloat32(c, cst.val().f32());
      case ExprType::F64:
        return RenderDouble(c, cst.val().f64());
      default:
        break;
    }

    return false;
}

static bool
RenderGetLocal(WasmRenderContext& c, AstGetLocal& gl)
{
    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, gl);
    if (!c.buffer.append("get_local "))
        return false;
    return RenderRef(c, gl.local());
}

static bool
RenderSetLocal(WasmRenderContext& c, AstSetLocal& sl)
{
    if (!RenderExpr(c, sl.value()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, sl);
    if (!c.buffer.append("set_local "))
        return false;
    return RenderRef(c, sl.local());
}

static bool
RenderTeeLocal(WasmRenderContext& c, AstTeeLocal& tl)
{
    if (!RenderExpr(c, tl.value()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, tl);
    if (!c.buffer.append("tee_local "))
        return false;
    return RenderRef(c, tl.local());
}

static bool
RenderGetGlobal(WasmRenderContext& c, AstGetGlobal& gg)
{
    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, gg);
    if (!c.buffer.append("get_global "))
        return false;
    return RenderRef(c, gg.global());
}

static bool
RenderSetGlobal(WasmRenderContext& c, AstSetGlobal& sg)
{
    if (!RenderExpr(c, sg.value()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, sg);
    if (!c.buffer.append("set_global "))
        return false;
    return RenderRef(c, sg.global());
}

static bool
RenderExprList(WasmRenderContext& c, const AstExprVector& exprs, uint32_t startAt = 0)
{
    for (uint32_t i = startAt; i < exprs.length(); i++) {
        if (!RenderExpr(c, *exprs[i]))
            return false;
    }
    return true;
}

static bool
RenderBlock(WasmRenderContext& c, AstBlock& block, bool isInline = false)
{
    if (!isInline && !RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, block);
    if (block.op() == Op::Block) {
        if (!c.buffer.append("block"))
            return false;
    } else if (block.op() == Op::Loop) {
        if (!c.buffer.append("loop"))
            return false;
    } else {
        return Fail(c, "unexpected block kind");
    }

    if (!RenderBlockNameAndSignature(c, block.name(), block.type()))
        return false;

    uint32_t startAtSubExpr = 0;

    // If there is a stack of blocks, print them all inline.
    if (block.op() == Op::Block &&
        block.exprs().length() &&
        block.exprs()[0]->kind() == AstExprKind::Block &&
        block.exprs()[0]->as<AstBlock>().op() == Op::Block)
    {
        if (!c.buffer.append(' '))
            return false;

        // Render the first inner expr (block) at the same indent level, but
        // next instructions one level further.
        if (!RenderBlock(c, block.exprs()[0]->as<AstBlock>(), /* isInline */ true))
            return false;

        startAtSubExpr = 1;
    }

    if (!c.buffer.append('\n'))
        return false;

    c.indent++;
    if (!RenderExprList(c, block.exprs(), startAtSubExpr))
        return false;
    c.indent--;

    return RenderIndent(c) &&
           c.buffer.append("end ") &&
           RenderName(c, block.name());
}

static bool
RenderFirst(WasmRenderContext& c, AstFirst& first)
{
    return RenderExprList(c, first.exprs());
}

static bool
RenderCurrentMemory(WasmRenderContext& c, AstCurrentMemory& cm)
{
    if (!RenderIndent(c))
        return false;

    return c.buffer.append("current_memory\n");
}

static bool
RenderGrowMemory(WasmRenderContext& c, AstGrowMemory& gm)
{
    if (!RenderExpr(c, *gm.operand()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, gm);
    return c.buffer.append("grow_memory\n");
}

static bool
RenderUnaryOperator(WasmRenderContext& c, AstUnaryOperator& unary)
{
    if (!RenderExpr(c, *unary.operand()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, unary);
    const char* opStr;
    switch (unary.op()) {
      case Op::I32Eqz:     opStr = "i32.eqz"; break;
      case Op::I32Clz:     opStr = "i32.clz"; break;
      case Op::I32Ctz:     opStr = "i32.ctz"; break;
      case Op::I32Popcnt:  opStr = "i32.popcnt"; break;
      case Op::I64Clz:     opStr = "i64.clz"; break;
      case Op::I64Ctz:     opStr = "i64.ctz"; break;
      case Op::I64Popcnt:  opStr = "i64.popcnt"; break;
      case Op::F32Abs:     opStr = "f32.abs"; break;
      case Op::F32Neg:     opStr = "f32.neg"; break;
      case Op::F32Ceil:    opStr = "f32.ceil"; break;
      case Op::F32Floor:   opStr = "f32.floor"; break;
      case Op::F32Sqrt:    opStr = "f32.sqrt"; break;
      case Op::F32Trunc:   opStr = "f32.trunc"; break;
      case Op::F32Nearest: opStr = "f32.nearest"; break;
      case Op::F64Abs:     opStr = "f64.abs"; break;
      case Op::F64Neg:     opStr = "f64.neg"; break;
      case Op::F64Ceil:    opStr = "f64.ceil"; break;
      case Op::F64Floor:   opStr = "f64.floor"; break;
      case Op::F64Nearest: opStr = "f64.nearest"; break;
      case Op::F64Sqrt:    opStr = "f64.sqrt"; break;
      case Op::F64Trunc:   opStr = "f64.trunc"; break;
      default:               return Fail(c, "unexpected unary operator");
    }

    return c.buffer.append(opStr, strlen(opStr));
}

static bool
RenderBinaryOperator(WasmRenderContext& c, AstBinaryOperator& binary)
{
    if (!RenderExpr(c, *binary.lhs()))
        return false;
    if (!RenderExpr(c, *binary.rhs()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, binary);
    const char* opStr;
    switch (binary.op()) {
      case Op::I32Add:      opStr = "i32.add"; break;
      case Op::I32Sub:      opStr = "i32.sub"; break;
      case Op::I32Mul:      opStr = "i32.mul"; break;
      case Op::I32DivS:     opStr = "i32.div_s"; break;
      case Op::I32DivU:     opStr = "i32.div_u"; break;
      case Op::I32RemS:     opStr = "i32.rem_s"; break;
      case Op::I32RemU:     opStr = "i32.rem_u"; break;
      case Op::I32And:      opStr = "i32.and"; break;
      case Op::I32Or:       opStr = "i32.or"; break;
      case Op::I32Xor:      opStr = "i32.xor"; break;
      case Op::I32Shl:      opStr = "i32.shl"; break;
      case Op::I32ShrS:     opStr = "i32.shr_s"; break;
      case Op::I32ShrU:     opStr = "i32.shr_u"; break;
      case Op::I32Rotl:     opStr = "i32.rotl"; break;
      case Op::I32Rotr:     opStr = "i32.rotr"; break;
      case Op::I64Add:      opStr = "i64.add"; break;
      case Op::I64Sub:      opStr = "i64.sub"; break;
      case Op::I64Mul:      opStr = "i64.mul"; break;
      case Op::I64DivS:     opStr = "i64.div_s"; break;
      case Op::I64DivU:     opStr = "i64.div_u"; break;
      case Op::I64RemS:     opStr = "i64.rem_s"; break;
      case Op::I64RemU:     opStr = "i64.rem_u"; break;
      case Op::I64And:      opStr = "i64.and"; break;
      case Op::I64Or:       opStr = "i64.or"; break;
      case Op::I64Xor:      opStr = "i64.xor"; break;
      case Op::I64Shl:      opStr = "i64.shl"; break;
      case Op::I64ShrS:     opStr = "i64.shr_s"; break;
      case Op::I64ShrU:     opStr = "i64.shr_u"; break;
      case Op::I64Rotl:     opStr = "i64.rotl"; break;
      case Op::I64Rotr:     opStr = "i64.rotr"; break;
      case Op::F32Add:      opStr = "f32.add"; break;
      case Op::F32Sub:      opStr = "f32.sub"; break;
      case Op::F32Mul:      opStr = "f32.mul"; break;
      case Op::F32Div:      opStr = "f32.div"; break;
      case Op::F32Min:      opStr = "f32.min"; break;
      case Op::F32Max:      opStr = "f32.max"; break;
      case Op::F32CopySign: opStr = "f32.copysign"; break;
      case Op::F64Add:      opStr = "f64.add"; break;
      case Op::F64Sub:      opStr = "f64.sub"; break;
      case Op::F64Mul:      opStr = "f64.mul"; break;
      case Op::F64Div:      opStr = "f64.div"; break;
      case Op::F64Min:      opStr = "f64.min"; break;
      case Op::F64Max:      opStr = "f64.max"; break;
      case Op::F64CopySign: opStr = "f64.copysign"; break;
      default:                return Fail(c, "unexpected binary operator");
    }

    return c.buffer.append(opStr, strlen(opStr));
}

static bool
RenderTernaryOperator(WasmRenderContext& c, AstTernaryOperator& ternary)
{
    if (!RenderExpr(c, *ternary.op0()))
        return false;
    if (!RenderExpr(c, *ternary.op1()))
        return false;
    if (!RenderExpr(c, *ternary.op2()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, ternary);
    const char* opStr;
    switch (ternary.op()) {
      case Op::Select: opStr = "select"; break;
      default:           return Fail(c, "unexpected ternary operator");
    }

    return c.buffer.append(opStr, strlen(opStr));
}

static bool
RenderComparisonOperator(WasmRenderContext& c, AstComparisonOperator& comp)
{
    if (!RenderExpr(c, *comp.lhs()))
        return false;
    if (!RenderExpr(c, *comp.rhs()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, comp);
    const char* opStr;
    switch (comp.op()) {
      case Op::I32Eq:  opStr = "i32.eq"; break;
      case Op::I32Ne:  opStr = "i32.ne"; break;
      case Op::I32LtS: opStr = "i32.lt_s"; break;
      case Op::I32LtU: opStr = "i32.lt_u"; break;
      case Op::I32LeS: opStr = "i32.le_s"; break;
      case Op::I32LeU: opStr = "i32.le_u"; break;
      case Op::I32GtS: opStr = "i32.gt_s"; break;
      case Op::I32GtU: opStr = "i32.gt_u"; break;
      case Op::I32GeS: opStr = "i32.ge_s"; break;
      case Op::I32GeU: opStr = "i32.ge_u"; break;
      case Op::I64Eq:  opStr = "i64.eq"; break;
      case Op::I64Ne:  opStr = "i64.ne"; break;
      case Op::I64LtS: opStr = "i64.lt_s"; break;
      case Op::I64LtU: opStr = "i64.lt_u"; break;
      case Op::I64LeS: opStr = "i64.le_s"; break;
      case Op::I64LeU: opStr = "i64.le_u"; break;
      case Op::I64GtS: opStr = "i64.gt_s"; break;
      case Op::I64GtU: opStr = "i64.gt_u"; break;
      case Op::I64GeS: opStr = "i64.ge_s"; break;
      case Op::I64GeU: opStr = "i64.ge_u"; break;
      case Op::F32Eq:  opStr = "f32.eq"; break;
      case Op::F32Ne:  opStr = "f32.ne"; break;
      case Op::F32Lt:  opStr = "f32.lt"; break;
      case Op::F32Le:  opStr = "f32.le"; break;
      case Op::F32Gt:  opStr = "f32.gt"; break;
      case Op::F32Ge:  opStr = "f32.ge"; break;
      case Op::F64Eq:  opStr = "f64.eq"; break;
      case Op::F64Ne:  opStr = "f64.ne"; break;
      case Op::F64Lt:  opStr = "f64.lt"; break;
      case Op::F64Le:  opStr = "f64.le"; break;
      case Op::F64Gt:  opStr = "f64.gt"; break;
      case Op::F64Ge:  opStr = "f64.ge"; break;
      default:           return Fail(c, "unexpected comparison operator");
    }

    return c.buffer.append(opStr, strlen(opStr));
}

static bool
RenderConversionOperator(WasmRenderContext& c, AstConversionOperator& conv)
{
    if (!RenderExpr(c, *conv.operand()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, conv);
    const char* opStr;
    switch (conv.op()) {
      case Op::I32WrapI64:        opStr = "i32.wrap/i64"; break;
      case Op::I32TruncSF32:      opStr = "i32.trunc_s/f32"; break;
      case Op::I32TruncUF32:      opStr = "i32.trunc_u/f32"; break;
      case Op::I32ReinterpretF32: opStr = "i32.reinterpret/f32"; break;
      case Op::I32TruncSF64:      opStr = "i32.trunc_s/f64"; break;
      case Op::I32TruncUF64:      opStr = "i32.trunc_u/f64"; break;
      case Op::I64ExtendSI32:     opStr = "i64.extend_s/i32"; break;
      case Op::I64ExtendUI32:     opStr = "i64.extend_u/i32"; break;
      case Op::I64TruncSF32:      opStr = "i64.trunc_s/f32"; break;
      case Op::I64TruncUF32:      opStr = "i64.trunc_u/f32"; break;
      case Op::I64TruncSF64:      opStr = "i64.trunc_s/f64"; break;
      case Op::I64TruncUF64:      opStr = "i64.trunc_u/f64"; break;
      case Op::I64ReinterpretF64: opStr = "i64.reinterpret/f64"; break;
      case Op::F32ConvertSI32:    opStr = "f32.convert_s/i32"; break;
      case Op::F32ConvertUI32:    opStr = "f32.convert_u/i32"; break;
      case Op::F32ReinterpretI32: opStr = "f32.reinterpret/i32"; break;
      case Op::F32ConvertSI64:    opStr = "f32.convert_s/i64"; break;
      case Op::F32ConvertUI64:    opStr = "f32.convert_u/i64"; break;
      case Op::F32DemoteF64:      opStr = "f32.demote/f64"; break;
      case Op::F64ConvertSI32:    opStr = "f64.convert_s/i32"; break;
      case Op::F64ConvertUI32:    opStr = "f64.convert_u/i32"; break;
      case Op::F64ConvertSI64:    opStr = "f64.convert_s/i64"; break;
      case Op::F64ConvertUI64:    opStr = "f64.convert_u/i64"; break;
      case Op::F64ReinterpretI64: opStr = "f64.reinterpret/i64"; break;
      case Op::F64PromoteF32:     opStr = "f64.promote/f32"; break;
#ifdef ENABLE_WASM_SIGNEXTEND_OPS
      case Op::I32Extend8S:       opStr = "i32.extend8_s"; break;
      case Op::I32Extend16S:      opStr = "i32.extend16_s"; break;
      case Op::I64Extend8S:       opStr = "i64.extend8_s"; break;
      case Op::I64Extend16S:      opStr = "i64.extend16_s"; break;
      case Op::I64Extend32S:      opStr = "i64.extend32_s"; break;
#endif
      case Op::I32Eqz:            opStr = "i32.eqz"; break;
      case Op::I64Eqz:            opStr = "i64.eqz"; break;
      default:                      return Fail(c, "unexpected conversion operator");
    }
    return c.buffer.append(opStr, strlen(opStr));
}

#ifdef ENABLE_WASM_SATURATING_TRUNC_OPS
static bool
RenderExtraConversionOperator(WasmRenderContext& c, AstExtraConversionOperator& conv)
{
    if (!RenderExpr(c, *conv.operand()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, conv);
    const char* opStr;
    switch (conv.op()) {
      case NumericOp::I32TruncSSatF32:   opStr = "i32.trunc_s:sat/f32"; break;
      case NumericOp::I32TruncUSatF32:   opStr = "i32.trunc_u:sat/f32"; break;
      case NumericOp::I32TruncSSatF64:   opStr = "i32.trunc_s:sat/f64"; break;
      case NumericOp::I32TruncUSatF64:   opStr = "i32.trunc_u:sat/f64"; break;
      case NumericOp::I64TruncSSatF32:   opStr = "i64.trunc_s:sat/f32"; break;
      case NumericOp::I64TruncUSatF32:   opStr = "i64.trunc_u:sat/f32"; break;
      case NumericOp::I64TruncSSatF64:   opStr = "i64.trunc_s:sat/f64"; break;
      case NumericOp::I64TruncUSatF64:   opStr = "i64.trunc_u:sat/f64"; break;
      default:                      return Fail(c, "unexpected extra conversion operator");
    }
    return c.buffer.append(opStr, strlen(opStr));
}
#endif

static bool
RenderIf(WasmRenderContext& c, AstIf& if_)
{
    if (!RenderExpr(c, if_.cond()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, if_);
    if (!c.buffer.append("if"))
        return false;
    if (!RenderBlockNameAndSignature(c, if_.name(), if_.type()))
        return false;
    if (!c.buffer.append('\n'))
        return false;

    c.indent++;
    if (!RenderExprList(c, if_.thenExprs()))
        return false;
    c.indent--;

    if (if_.hasElse()) {
        if (!RenderIndent(c))
            return false;

        if (!c.buffer.append("else\n"))
            return false;

        c.indent++;
        if (!RenderExprList(c, if_.elseExprs()))
            return false;
        c.indent--;
    }

    if (!RenderIndent(c))
        return false;

    return c.buffer.append("end");
}

static bool
RenderLoadStoreBase(WasmRenderContext& c, const AstLoadStoreAddress& lsa)
{
    return RenderExpr(c, lsa.base());
}

static bool
RenderLoadStoreAddress(WasmRenderContext& c, const AstLoadStoreAddress& lsa, uint32_t defaultAlignLog2)
{
    if (lsa.offset() != 0) {
        if (!c.buffer.append(" offset="))
            return false;
        if (!RenderInt32(c, lsa.offset()))
            return false;
    }

    uint32_t alignLog2 = lsa.flags();
    if (defaultAlignLog2 != alignLog2) {
        if (!c.buffer.append(" align="))
            return false;
        if (!RenderInt32(c, 1 << alignLog2))
            return false;
    }

    return true;
}

static bool
RenderLoad(WasmRenderContext& c, AstLoad& load)
{
    if (!RenderLoadStoreBase(c, load.address()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, load);
    uint32_t defaultAlignLog2;
    switch (load.op()) {
      case Op::I32Load8S:
        if (!c.buffer.append("i32.load8_s"))
            return false;
        defaultAlignLog2 = 0;
        break;
      case Op::I64Load8S:
        if (!c.buffer.append("i64.load8_s"))
            return false;
        defaultAlignLog2 = 0;
        break;
      case Op::I32Load8U:
        if (!c.buffer.append("i32.load8_u"))
            return false;
        defaultAlignLog2 = 0;
        break;
      case Op::I64Load8U:
        if (!c.buffer.append("i64.load8_u"))
            return false;
        defaultAlignLog2 = 0;
        break;
      case Op::I32Load16S:
        if (!c.buffer.append("i32.load16_s"))
            return false;
        defaultAlignLog2 = 1;
        break;
      case Op::I64Load16S:
        if (!c.buffer.append("i64.load16_s"))
            return false;
        defaultAlignLog2 = 1;
        break;
      case Op::I32Load16U:
        if (!c.buffer.append("i32.load16_u"))
            return false;
        defaultAlignLog2 = 1;
        break;
      case Op::I64Load16U:
        if (!c.buffer.append("i64.load16_u"))
            return false;
        defaultAlignLog2 = 1;
        break;
      case Op::I64Load32S:
        if (!c.buffer.append("i64.load32_s"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::I64Load32U:
        if (!c.buffer.append("i64.load32_u"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::I32Load:
        if (!c.buffer.append("i32.load"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::I64Load:
        if (!c.buffer.append("i64.load"))
            return false;
        defaultAlignLog2 = 3;
        break;
      case Op::F32Load:
        if (!c.buffer.append("f32.load"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::F64Load:
        if (!c.buffer.append("f64.load"))
            return false;
        defaultAlignLog2 = 3;
        break;
      default:
        return Fail(c, "unexpected load operator");
    }

    return RenderLoadStoreAddress(c, load.address(), defaultAlignLog2);
}

static bool
RenderStore(WasmRenderContext& c, AstStore& store)
{
    if (!RenderLoadStoreBase(c, store.address()))
        return false;

    if (!RenderExpr(c, store.value()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, store);
    uint32_t defaultAlignLog2;
    switch (store.op()) {
      case Op::I32Store8:
        if (!c.buffer.append("i32.store8"))
            return false;
        defaultAlignLog2 = 0;
        break;
      case Op::I64Store8:
        if (!c.buffer.append("i64.store8"))
            return false;
        defaultAlignLog2 = 0;
        break;
      case Op::I32Store16:
        if (!c.buffer.append("i32.store16"))
            return false;
        defaultAlignLog2 = 1;
        break;
      case Op::I64Store16:
        if (!c.buffer.append("i64.store16"))
            return false;
        defaultAlignLog2 = 1;
        break;
      case Op::I64Store32:
        if (!c.buffer.append("i64.store32"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::I32Store:
        if (!c.buffer.append("i32.store"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::I64Store:
        if (!c.buffer.append("i64.store"))
            return false;
        defaultAlignLog2 = 3;
        break;
      case Op::F32Store:
        if (!c.buffer.append("f32.store"))
            return false;
        defaultAlignLog2 = 2;
        break;
      case Op::F64Store:
        if (!c.buffer.append("f64.store"))
            return false;
        defaultAlignLog2 = 3;
        break;
      default:
        return Fail(c, "unexpected store operator");
    }

    return RenderLoadStoreAddress(c, store.address(), defaultAlignLog2);
}

static bool
RenderBranch(WasmRenderContext& c, AstBranch& branch)
{
    Op op = branch.op();
    MOZ_ASSERT(op == Op::BrIf || op == Op::Br);

    if (op == Op::BrIf) {
        if (!RenderExpr(c, branch.cond()))
            return false;
    }

    if (branch.maybeValue()) {
        if (!RenderExpr(c, *(branch.maybeValue())))
            return false;
    }

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, branch);
    if (op == Op::BrIf ? !c.buffer.append("br_if ") : !c.buffer.append("br "))
        return false;

    return RenderRef(c, branch.target());
}

static bool
RenderBrTable(WasmRenderContext& c, AstBranchTable& table)
{
    if (table.maybeValue()) {
        if (!RenderExpr(c, *(table.maybeValue())))
            return false;
    }

    // Index
    if (!RenderExpr(c, table.index()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, table);
    if (!c.buffer.append("br_table "))
        return false;

    uint32_t tableLength = table.table().length();
    for (uint32_t i = 0; i < tableLength; i++) {
        if (!RenderRef(c, table.table()[i]))
            return false;

        if (!c.buffer.append(" "))
            return false;
    }

    return RenderRef(c, table.def());
}

static bool
RenderReturn(WasmRenderContext& c, AstReturn& ret)
{
    if (ret.maybeExpr()) {
        if (!RenderExpr(c, *(ret.maybeExpr())))
            return false;
    }

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, ret);
    return c.buffer.append("return");
}

static bool
RenderAtomicCmpXchg(WasmRenderContext& c, AstAtomicCmpXchg& cmpxchg)
{
    if (!RenderLoadStoreBase(c, cmpxchg.address()))
        return false;

    if (!RenderExpr(c, cmpxchg.expected()))
        return false;
    if (!RenderExpr(c, cmpxchg.replacement()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, cmpxchg);
    const char* opname;
    switch (cmpxchg.op()) {
      case ThreadOp::I32AtomicCmpXchg8U:  opname = "i32.atomic.rmw8_u.cmpxchg"; break;
      case ThreadOp::I64AtomicCmpXchg8U:  opname = "i64.atomic.rmw8_u.cmpxchg"; break;
      case ThreadOp::I32AtomicCmpXchg16U: opname = "i32.atomic.rmw16_u.cmpxchg"; break;
      case ThreadOp::I64AtomicCmpXchg16U: opname = "i64.atomic.rmw16_u.cmpxchg"; break;
      case ThreadOp::I64AtomicCmpXchg32U: opname = "i64.atomic.rmw32_u.cmpxchg"; break;
      case ThreadOp::I32AtomicCmpXchg:    opname = "i32.atomic.rmw.cmpxchg"; break;
      case ThreadOp::I64AtomicCmpXchg:    opname = "i64.atomic.rmw.cmpxchg"; break;
      default:                            return Fail(c, "unexpected cmpxchg operator");
    }

    if (!c.buffer.append(opname, strlen(opname)))
        return false;

    return RenderLoadStoreAddress(c, cmpxchg.address(), 0);
}

static bool
RenderAtomicLoad(WasmRenderContext& c, AstAtomicLoad& load)
{
    if (!RenderLoadStoreBase(c, load.address()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, load);
    const char* opname;
    switch (load.op()) {
      case ThreadOp::I32AtomicLoad8U:  opname = "i32.atomic.load8_u"; break;
      case ThreadOp::I64AtomicLoad8U:  opname = "i64.atomic.load8_u"; break;
      case ThreadOp::I32AtomicLoad16U: opname = "i32.atomic.load16_u"; break;
      case ThreadOp::I64AtomicLoad16U: opname = "i64.atomic.load16_u"; break;
      case ThreadOp::I64AtomicLoad32U: opname = "i64.atomic.load32_u"; break;
      case ThreadOp::I32AtomicLoad:    opname = "i32.atomic.load"; break;
      case ThreadOp::I64AtomicLoad:    opname = "i64.atomic.load"; break;
      default:                         return Fail(c, "unexpected load operator");
    }

    if (!c.buffer.append(opname, strlen(opname)))
        return false;

    return RenderLoadStoreAddress(c, load.address(), 0);
}

static bool
RenderAtomicRMW(WasmRenderContext& c, AstAtomicRMW& rmw)
{
    if (!RenderLoadStoreBase(c, rmw.address()))
        return false;

    if (!RenderExpr(c, rmw.value()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, rmw);
    const char* opname;
    switch (rmw.op()) {
      case ThreadOp::I32AtomicAdd:     opname = "i32.atomic.rmw.add"; break;
      case ThreadOp::I64AtomicAdd:     opname = "i64.atomic.rmw.add"; break;
      case ThreadOp::I32AtomicAdd8U:   opname = "i32.atomic.rmw8_u.add"; break;
      case ThreadOp::I32AtomicAdd16U:  opname = "i32.atomic.rmw16_u.add"; break;
      case ThreadOp::I64AtomicAdd8U:   opname = "i64.atomic.rmw8_u.add"; break;
      case ThreadOp::I64AtomicAdd16U:  opname = "i64.atomic.rmw16_u.add"; break;
      case ThreadOp::I64AtomicAdd32U:  opname = "i64.atomic.rmw32_u.add"; break;
      case ThreadOp::I32AtomicSub:     opname = "i32.atomic.rmw.sub"; break;
      case ThreadOp::I64AtomicSub:     opname = "i64.atomic.rmw.sub"; break;
      case ThreadOp::I32AtomicSub8U:   opname = "i32.atomic.rmw8_u.sub"; break;
      case ThreadOp::I32AtomicSub16U:  opname = "i32.atomic.rmw16_u.sub"; break;
      case ThreadOp::I64AtomicSub8U:   opname = "i64.atomic.rmw8_u.sub"; break;
      case ThreadOp::I64AtomicSub16U:  opname = "i64.atomic.rmw16_u.sub"; break;
      case ThreadOp::I64AtomicSub32U:  opname = "i64.atomic.rmw32_u.sub"; break;
      case ThreadOp::I32AtomicAnd:     opname = "i32.atomic.rmw.and"; break;
      case ThreadOp::I64AtomicAnd:     opname = "i64.atomic.rmw.and"; break;
      case ThreadOp::I32AtomicAnd8U:   opname = "i32.atomic.rmw8_u.and"; break;
      case ThreadOp::I32AtomicAnd16U:  opname = "i32.atomic.rmw16_u.and"; break;
      case ThreadOp::I64AtomicAnd8U:   opname = "i64.atomic.rmw8_u.and"; break;
      case ThreadOp::I64AtomicAnd16U:  opname = "i64.atomic.rmw16_u.and"; break;
      case ThreadOp::I64AtomicAnd32U:  opname = "i64.atomic.rmw32_u.and"; break;
      case ThreadOp::I32AtomicOr:      opname = "i32.atomic.rmw.or"; break;
      case ThreadOp::I64AtomicOr:      opname = "i64.atomic.rmw.or"; break;
      case ThreadOp::I32AtomicOr8U:    opname = "i32.atomic.rmw8_u.or"; break;
      case ThreadOp::I32AtomicOr16U:   opname = "i32.atomic.rmw16_u.or"; break;
      case ThreadOp::I64AtomicOr8U:    opname = "i64.atomic.rmw8_u.or"; break;
      case ThreadOp::I64AtomicOr16U:   opname = "i64.atomic.rmw16_u.or"; break;
      case ThreadOp::I64AtomicOr32U:   opname = "i64.atomic.rmw32_u.or"; break;
      case ThreadOp::I32AtomicXor:     opname = "i32.atomic.rmw.xor"; break;
      case ThreadOp::I64AtomicXor:     opname = "i64.atomic.rmw.xor"; break;
      case ThreadOp::I32AtomicXor8U:   opname = "i32.atomic.rmw8_u.xor"; break;
      case ThreadOp::I32AtomicXor16U:  opname = "i32.atomic.rmw16_u.xor"; break;
      case ThreadOp::I64AtomicXor8U:   opname = "i64.atomic.rmw8_u.xor"; break;
      case ThreadOp::I64AtomicXor16U:  opname = "i64.atomic.rmw16_u.xor"; break;
      case ThreadOp::I64AtomicXor32U:  opname = "i64.atomic.rmw32_u.xor"; break;
      case ThreadOp::I32AtomicXchg:    opname = "i32.atomic.rmw.xchg"; break;
      case ThreadOp::I64AtomicXchg:    opname = "i64.atomic.rmw.xchg"; break;
      case ThreadOp::I32AtomicXchg8U:  opname = "i32.atomic.rmw8_u.xchg"; break;
      case ThreadOp::I32AtomicXchg16U: opname = "i32.atomic.rmw16_u.xchg"; break;
      case ThreadOp::I64AtomicXchg8U:  opname = "i64.atomic.rmw8_u.xchg"; break;
      case ThreadOp::I64AtomicXchg16U: opname = "i64.atomic.rmw16_u.xchg"; break;
      case ThreadOp::I64AtomicXchg32U: opname = "i64.atomic.rmw32_u.xchg"; break;
      default:                         return Fail(c, "unexpected rmw operator");
    }

    if (!c.buffer.append(opname, strlen(opname)))
        return false;

    return RenderLoadStoreAddress(c, rmw.address(), 0);
}

static bool
RenderAtomicStore(WasmRenderContext& c, AstAtomicStore& store)
{
    if (!RenderLoadStoreBase(c, store.address()))
        return false;

    if (!RenderExpr(c, store.value()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, store);
    const char* opname;
    switch (store.op()) {
      case ThreadOp::I32AtomicStore8U:  opname = "i32.atomic.store8_u"; break;
      case ThreadOp::I64AtomicStore8U:  opname = "i64.atomic.store8_u"; break;
      case ThreadOp::I32AtomicStore16U: opname = "i32.atomic.store16_u"; break;
      case ThreadOp::I64AtomicStore16U: opname = "i64.atomic.store16_u"; break;
      case ThreadOp::I64AtomicStore32U: opname = "i64.atomic.store32_u"; break;
      case ThreadOp::I32AtomicStore:    opname = "i32.atomic.store"; break;
      case ThreadOp::I64AtomicStore:    opname = "i64.atomic.store"; break;
      default:                          return Fail(c, "unexpected store operator");
    }

    if (!c.buffer.append(opname, strlen(opname)))
        return false;

    return RenderLoadStoreAddress(c, store.address(), 0);
}

static bool
RenderWait(WasmRenderContext& c, AstWait& wait)
{
    if (!RenderLoadStoreBase(c, wait.address()))
        return false;

    if (!RenderExpr(c, wait.expected()))
        return false;

    if (!RenderExpr(c, wait.timeout()))
        return false;

    if (!RenderIndent(c))
        return false;

    MAP_AST_EXPR(c, wait);
    const char* opname;
    switch (wait.op()) {
      case ThreadOp::I32Wait:  opname = "i32.atomic.wait"; break;
      case ThreadOp::I64Wait:  opname = "i64.atomic.wait"; break;
      default:           return Fail(c, "unexpected wait operator");
    }

    if (!c.buffer.append(opname, strlen(opname)))
        return false;

    return RenderLoadStoreAddress(c, wait.address(), 0);
}

static bool
RenderWake(WasmRenderContext& c, AstWake& wake)
{
    if (!RenderLoadStoreBase(c, wake.address()))
        return false;

    if (!RenderExpr(c, wake.count()))
        return false;

    if (!RenderIndent(c))
        return false;

    if (!c.buffer.append("atomic.wake", strlen("atomic.wake")))
        return false;

    return RenderLoadStoreAddress(c, wake.address(), 0);
}

static bool
RenderExpr(WasmRenderContext& c, AstExpr& expr, bool newLine /* = true */)
{
    switch (expr.kind()) {
      case AstExprKind::Drop:
        if (!RenderDrop(c, expr.as<AstDrop>()))
            return false;
        break;
      case AstExprKind::Nop:
        if (!RenderNop(c, expr.as<AstNop>()))
            return false;
        break;
      case AstExprKind::Unreachable:
        if (!RenderUnreachable(c, expr.as<AstUnreachable>()))
            return false;
        break;
      case AstExprKind::Call:
        if (!RenderCall(c, expr.as<AstCall>()))
            return false;
        break;
      case AstExprKind::CallIndirect:
        if (!RenderCallIndirect(c, expr.as<AstCallIndirect>()))
            return false;
        break;
      case AstExprKind::Const:
        if (!RenderConst(c, expr.as<AstConst>()))
            return false;
        break;
      case AstExprKind::GetLocal:
        if (!RenderGetLocal(c, expr.as<AstGetLocal>()))
            return false;
        break;
      case AstExprKind::SetLocal:
        if (!RenderSetLocal(c, expr.as<AstSetLocal>()))
            return false;
        break;
      case AstExprKind::GetGlobal:
        if (!RenderGetGlobal(c, expr.as<AstGetGlobal>()))
            return false;
        break;
      case AstExprKind::SetGlobal:
        if (!RenderSetGlobal(c, expr.as<AstSetGlobal>()))
            return false;
        break;
      case AstExprKind::TeeLocal:
        if (!RenderTeeLocal(c, expr.as<AstTeeLocal>()))
            return false;
        break;
      case AstExprKind::Block:
        if (!RenderBlock(c, expr.as<AstBlock>()))
            return false;
        break;
      case AstExprKind::If:
        if (!RenderIf(c, expr.as<AstIf>()))
            return false;
        break;
      case AstExprKind::UnaryOperator:
        if (!RenderUnaryOperator(c, expr.as<AstUnaryOperator>()))
            return false;
        break;
      case AstExprKind::BinaryOperator:
        if (!RenderBinaryOperator(c, expr.as<AstBinaryOperator>()))
            return false;
        break;
      case AstExprKind::TernaryOperator:
        if (!RenderTernaryOperator(c, expr.as<AstTernaryOperator>()))
            return false;
        break;
      case AstExprKind::ComparisonOperator:
        if (!RenderComparisonOperator(c, expr.as<AstComparisonOperator>()))
            return false;
        break;
      case AstExprKind::ConversionOperator:
        if (!RenderConversionOperator(c, expr.as<AstConversionOperator>()))
            return false;
        break;
#ifdef ENABLE_WASM_SATURATING_TRUNC_OPS
      case AstExprKind::ExtraConversionOperator:
        if (!RenderExtraConversionOperator(c, expr.as<AstExtraConversionOperator>()))
            return false;
        break;
#endif
      case AstExprKind::Load:
        if (!RenderLoad(c, expr.as<AstLoad>()))
            return false;
        break;
      case AstExprKind::Store:
        if (!RenderStore(c, expr.as<AstStore>()))
            return false;
        break;
      case AstExprKind::Branch:
        if (!RenderBranch(c, expr.as<AstBranch>()))
            return false;
        break;
      case AstExprKind::BranchTable:
        if (!RenderBrTable(c, expr.as<AstBranchTable>()))
            return false;
        break;
      case AstExprKind::Return:
        if (!RenderReturn(c, expr.as<AstReturn>()))
            return false;
        break;
      case AstExprKind::First:
        newLine = false;
        if (!RenderFirst(c, expr.as<AstFirst>()))
            return false;
        break;
      case AstExprKind::CurrentMemory:
        if (!RenderCurrentMemory(c, expr.as<AstCurrentMemory>()))
            return false;
        break;
      case AstExprKind::GrowMemory:
        if (!RenderGrowMemory(c, expr.as<AstGrowMemory>()))
            return false;
        break;
      case AstExprKind::AtomicCmpXchg:
        if (!RenderAtomicCmpXchg(c, expr.as<AstAtomicCmpXchg>()))
            return false;
        break;
      case AstExprKind::AtomicLoad:
        if (!RenderAtomicLoad(c, expr.as<AstAtomicLoad>()))
            return false;
        break;
      case AstExprKind::AtomicRMW:
        if (!RenderAtomicRMW(c, expr.as<AstAtomicRMW>()))
            return false;
        break;
      case AstExprKind::AtomicStore:
        if (!RenderAtomicStore(c, expr.as<AstAtomicStore>()))
            return false;
        break;
      case AstExprKind::Wait:
        if (!RenderWait(c, expr.as<AstWait>()))
            return false;
        break;
      case AstExprKind::Wake:
        if (!RenderWake(c, expr.as<AstWake>()))
            return false;
        break;
      default:
        MOZ_CRASH("Bad AstExprKind");
    }

    return !newLine || c.buffer.append("\n");
}

static bool
RenderSignature(WasmRenderContext& c, const AstSig& sig, const AstNameVector* maybeLocals = nullptr)
{
    uint32_t paramsNum = sig.args().length();

    if (maybeLocals) {
        for (uint32_t i = 0; i < paramsNum; i++) {
            if (!c.buffer.append(" (param "))
                return false;
            const AstName& name = (*maybeLocals)[i];
            if (!name.empty()) {
                if (!RenderName(c, name))
                    return false;
                if (!c.buffer.append(" "))
                    return false;
            }
            ValType arg = sig.args()[i];
            if (!RenderValType(c, arg))
                return false;
            if (!c.buffer.append(")"))
                return false;
        }
    } else if (paramsNum > 0) {
        if (!c.buffer.append(" (param"))
            return false;
        for (uint32_t i = 0; i < paramsNum; i++) {
            if (!c.buffer.append(" "))
                return false;
            ValType arg = sig.args()[i];
            if (!RenderValType(c, arg))
                return false;
        }
        if (!c.buffer.append(")"))
            return false;
    }
    if (sig.ret() != ExprType::Void) {
        if (!c.buffer.append(" (result "))
            return false;
        if (!RenderExprType(c, sig.ret()))
            return false;
        if (!c.buffer.append(")"))
            return false;
    }
    return true;
}

static bool
RenderTypeSection(WasmRenderContext& c, const AstModule::SigVector& sigs)
{
    uint32_t numSigs = sigs.length();
    if (!numSigs)
        return true;

    for (uint32_t sigIndex = 0; sigIndex < numSigs; sigIndex++) {
        const AstSig* sig = sigs[sigIndex];
        if (!RenderIndent(c))
            return false;
        if (!c.buffer.append("(type"))
            return false;
        if (!sig->name().empty()) {
            if (!c.buffer.append(" "))
                return false;
            if (!RenderName(c, sig->name()))
                return false;
        }
        if (!c.buffer.append(" (func"))
            return false;
        if (!RenderSignature(c, *sig))
            return false;
        if (!c.buffer.append("))\n"))
            return false;
    }
    return true;
}

static bool
RenderLimits(WasmRenderContext& c, const Limits& limits)
{
    if (!RenderInt32(c, limits.initial))
        return false;
    if (limits.maximum) {
        if (!c.buffer.append(" "))
            return false;
        if (!RenderInt32(c, *limits.maximum))
            return false;
    }
    if (limits.shared == Shareable::True) {
        if (!c.buffer.append(" shared"))
            return false;
    }
    return true;
}

static bool
RenderResizableTable(WasmRenderContext& c, const Limits& table)
{
    if (!c.buffer.append("(table "))
        return false;
    if (!RenderLimits(c, table))
        return false;
    MOZ_ASSERT(table.shared == Shareable::False);
    return c.buffer.append(" anyfunc)");
}

static bool
RenderTableSection(WasmRenderContext& c, const AstModule& module)
{
    if (!module.hasTable())
        return true;
    for (const AstResizable& table : module.tables()) {
        if (table.imported)
            continue;
        if (!RenderIndent(c))
            return false;
        if (!RenderResizableTable(c, table.limits))
            return false;
        if (!c.buffer.append("\n"))
            return false;
    }
    return true;
}

static bool
RenderInlineExpr(WasmRenderContext& c, AstExpr& expr)
{
    if (!c.buffer.append("("))
        return false;

    uint32_t prevIndent = c.indent;
    c.indent = 0;
    if (!RenderExpr(c, expr, /* newLine */ false))
        return false;
    c.indent = prevIndent;

    return c.buffer.append(")");
}

static bool
RenderElemSection(WasmRenderContext& c, const AstModule& module)
{
    for (const AstElemSegment* segment : module.elemSegments()) {
        if (!RenderIndent(c))
            return false;
        if (!c.buffer.append("(elem "))
            return false;
        if (!RenderInlineExpr(c, *segment->offset()))
            return false;

        for (const AstRef& elem : segment->elems()) {
            if (!c.buffer.append(" "))
                return false;

            uint32_t index = elem.index();
            AstName name = index < module.funcImportNames().length()
                           ? module.funcImportNames()[index]
                           : module.funcs()[index - module.funcImportNames().length()]->name();

            if (name.empty()) {
                if (!RenderInt32(c, index))
                    return false;
            } else {
                if (!RenderName(c, name))
                    return false;
            }
        }

        if (!c.buffer.append(")\n"))
            return false;
    }

    return true;
}

static bool
RenderGlobal(WasmRenderContext& c, const AstGlobal& glob, bool inImport = false)
{
    if (!c.buffer.append("(global "))
        return false;

    if (!inImport) {
        if (!RenderName(c, glob.name()))
            return false;
        if (!c.buffer.append(" "))
            return false;
    }

    if (glob.isMutable()) {
        if (!c.buffer.append("(mut "))
            return false;
        if (!RenderValType(c, glob.type()))
            return false;
        if (!c.buffer.append(")"))
            return false;
    } else {
        if (!RenderValType(c, glob.type()))
            return false;
    }

    if (glob.hasInit()) {
        if (!c.buffer.append(" "))
            return false;
        if (!RenderInlineExpr(c, glob.init()))
            return false;
    }

    if (!c.buffer.append(")"))
        return false;

    return inImport || c.buffer.append("\n");
}

static bool
RenderGlobalSection(WasmRenderContext& c, const AstModule& module)
{
    if (module.globals().empty())
        return true;

    for (const AstGlobal* global : module.globals()) {
        if (!RenderIndent(c))
            return false;
        if (!RenderGlobal(c, *global))
            return false;
    }

    return true;
}

static bool
RenderResizableMemory(WasmRenderContext& c, const Limits& memory)
{
    if (!c.buffer.append("(memory "))
        return false;

    Limits resizedMemory = memory;

    MOZ_ASSERT(resizedMemory.initial % PageSize == 0);
    resizedMemory.initial /= PageSize;

    if (resizedMemory.maximum) {
        if (*resizedMemory.maximum == UINT32_MAX) {
            // See special casing in DecodeMemoryLimits.
            *resizedMemory.maximum = MaxMemoryMaximumPages;
        } else {
            MOZ_ASSERT(*resizedMemory.maximum % PageSize == 0);
            *resizedMemory.maximum /= PageSize;
        }
    }

    if (!RenderLimits(c, resizedMemory))
        return false;

    return c.buffer.append(")");
}

static bool
RenderImport(WasmRenderContext& c, AstImport& import, const AstModule& module)
{
    if (!RenderIndent(c))
        return false;
    if (!c.buffer.append("(import "))
        return false;
    if (!RenderName(c, import.name()))
        return false;
    if (!c.buffer.append(" \""))
        return false;

    const AstName& moduleName = import.module();
    if (!RenderEscapedString(c, moduleName))
        return false;

    if (!c.buffer.append("\" \""))
        return false;

    const AstName& fieldName = import.field();
    if (!RenderEscapedString(c, fieldName))
        return false;

    if (!c.buffer.append("\" "))
        return false;

    switch (import.kind()) {
      case DefinitionKind::Function: {
        if (!c.buffer.append("(func"))
            return false;
        const AstSig* sig = module.sigs()[import.funcSig().index()];
        if (!RenderSignature(c, *sig))
            return false;
        if (!c.buffer.append(")"))
            return false;
        break;
      }
      case DefinitionKind::Table: {
        if (!RenderResizableTable(c, import.limits()))
            return false;
        break;
      }
      case DefinitionKind::Memory: {
        if (!RenderResizableMemory(c, import.limits()))
            return false;
        break;
      }
      case DefinitionKind::Global: {
        const AstGlobal& glob = import.global();
        if (!RenderGlobal(c, glob, /* inImport */ true))
            return false;
        break;
      }
    }

    return c.buffer.append(")\n");
}

static bool
RenderImportSection(WasmRenderContext& c, const AstModule& module)
{
    for (AstImport* import : module.imports()) {
        if (!RenderImport(c, *import, module))
            return false;
    }
    return true;
}

static bool
RenderExport(WasmRenderContext& c, AstExport& export_,
             const AstModule::NameVector& funcImportNames,
             const AstModule::FuncVector& funcs)
{
    if (!RenderIndent(c))
        return false;
    if (!c.buffer.append("(export \""))
        return false;
    if (!RenderEscapedString(c, export_.name()))
        return false;
    if (!c.buffer.append("\" "))
        return false;

    switch (export_.kind()) {
      case DefinitionKind::Function: {
        uint32_t index = export_.ref().index();
        AstName name = index < funcImportNames.length()
                       ? funcImportNames[index]
                       : funcs[index - funcImportNames.length()]->name();
        if (name.empty()) {
            if (!RenderInt32(c, index))
                return false;
        } else {
            if (!RenderName(c, name))
                return false;
        }
        break;
      }
      case DefinitionKind::Table: {
        if (!c.buffer.append("table"))
            return false;
        break;
      }
      case DefinitionKind::Memory: {
        if (!c.buffer.append("memory"))
            return false;
        break;
      }
      case DefinitionKind::Global: {
        if (!c.buffer.append("global "))
            return false;
        if (!RenderRef(c, export_.ref()))
            return false;
        break;
      }
    }

    return c.buffer.append(")\n");
}

static bool
RenderExportSection(WasmRenderContext& c, const AstModule::ExportVector& exports,
                    const AstModule::NameVector& funcImportNames,
                    const AstModule::FuncVector& funcs)
{
    uint32_t numExports = exports.length();
    for (uint32_t i = 0; i < numExports; i++) {
        if (!RenderExport(c, *exports[i], funcImportNames, funcs))
            return false;
    }
    return true;
}

static bool
RenderFunctionBody(WasmRenderContext& c, AstFunc& func, const AstModule::SigVector& sigs)
{
    const AstSig* sig = sigs[func.sig().index()];

    uint32_t argsNum = sig->args().length();
    uint32_t localsNum = func.vars().length();
    if (localsNum > 0) {
        if (!RenderIndent(c))
            return false;
        for (uint32_t i = 0; i < localsNum; i++) {
            if (!c.buffer.append("(local "))
                return false;
            const AstName& name = func.locals()[argsNum + i];
            if (!name.empty()) {
                if (!RenderName(c, name))
                    return false;
                if (!c.buffer.append(" "))
                    return false;
            }
            ValType local = func.vars()[i];
            if (!RenderValType(c, local))
                return false;
            if (!c.buffer.append(") "))
                return false;
        }
        if (!c.buffer.append("\n"))
            return false;
    }


    uint32_t exprsNum = func.body().length();
    for (uint32_t i = 0; i < exprsNum; i++) {
        if (!RenderExpr(c, *func.body()[i]))
            return false;
    }

    if (c.maybeSourceMap) {
        if (!c.maybeSourceMap->exprlocs().emplaceBack(c.buffer.lineno(), c.buffer.column(), func.endOffset()))
            return false;
    }

    return true;
}

static bool
RenderCodeSection(WasmRenderContext& c, const AstModule::FuncVector& funcs,
                  const AstModule::SigVector& sigs)
{
    uint32_t numFuncBodies = funcs.length();
    for (uint32_t funcIndex = 0; funcIndex < numFuncBodies; funcIndex++) {
        AstFunc* func = funcs[funcIndex];
        uint32_t sigIndex = func->sig().index();
        AstSig* sig = sigs[sigIndex];

        if (!RenderIndent(c))
            return false;
        if (!c.buffer.append("(func "))
            return false;
        if (!func->name().empty()) {
            if (!RenderName(c, func->name()))
                return false;
        }

        if (!RenderSignature(c, *sig, &(func->locals())))
            return false;
        if (!c.buffer.append("\n"))
            return false;

        c.currentFuncIndex = funcIndex;

        c.indent++;
        if (!RenderFunctionBody(c, *func, sigs))
            return false;
        c.indent--;
        if (!RenderIndent(c))
            return false;
        if (!c.buffer.append(")\n"))
            return false;
    }

    return true;
}

static bool
RenderMemorySection(WasmRenderContext& c, const AstModule& module)
{
    if (!module.hasMemory())
        return true;

    for (const AstResizable& memory : module.memories()) {
        if (memory.imported)
            continue;
        if (!RenderIndent(c))
            return false;
        if (!RenderResizableMemory(c, memory.limits))
            return false;
        if (!c.buffer.append("\n"))
            return false;
    }

    return true;
}

static bool
RenderDataSection(WasmRenderContext& c, const AstModule& module)
{
    uint32_t numSegments = module.dataSegments().length();
    if (!numSegments)
        return true;

    for (const AstDataSegment* seg : module.dataSegments()) {
        if (!RenderIndent(c))
            return false;
        if (!c.buffer.append("(data "))
            return false;
        if (!RenderInlineExpr(c, *seg->offset()))
            return false;
        if (!c.buffer.append("\n"))
            return false;

        c.indent++;
        for (const AstName& fragment : seg->fragments()) {
            if (!RenderIndent(c))
                return false;
            if (!c.buffer.append("\""))
                return false;
            if (!RenderEscapedString(c, fragment))
                return false;
            if (!c.buffer.append("\"\n"))
                return false;
        }
        c.indent--;

        if (!RenderIndent(c))
            return false;
        if (!c.buffer.append(")\n"))
            return false;
    }

    return true;
}

static bool
RenderStartSection(WasmRenderContext& c, AstModule& module)
{
    if (!module.hasStartFunc())
        return true;

    if (!RenderIndent(c))
        return false;
    if (!c.buffer.append("(start "))
        return false;
    if (!RenderRef(c, module.startFunc().func()))
        return false;
    if (!c.buffer.append(")\n"))
        return false;

    return true;
}

static bool
RenderModule(WasmRenderContext& c, AstModule& module)
{
    if (!c.buffer.append("(module\n"))
        return false;

    c.indent++;

    if (!RenderTypeSection(c, module.sigs()))
        return false;

    if (!RenderImportSection(c, module))
        return false;

    if (!RenderTableSection(c, module))
        return false;

    if (!RenderMemorySection(c, module))
        return false;

    if (!RenderGlobalSection(c, module))
        return false;

    if (!RenderExportSection(c, module.exports(), module.funcImportNames(), module.funcs()))
        return false;

    if (!RenderStartSection(c, module))
        return false;

    if (!RenderElemSection(c, module))
        return false;

    if (!RenderCodeSection(c, module.funcs(), module.sigs()))
        return false;

    if (!RenderDataSection(c, module))
        return false;

    c.indent--;

    if (!c.buffer.append(")"))
        return false;

    return true;
}

#undef MAP_AST_EXPR

/*****************************************************************************/
// Top-level functions

bool
wasm::BinaryToText(JSContext* cx, const uint8_t* bytes, size_t length, StringBuffer& buffer,
                   GeneratedSourceMap* sourceMap /* = nullptr */)
{
    LifoAlloc lifo(AST_LIFO_DEFAULT_CHUNK_SIZE);

    AstModule* module;
    if (!BinaryToAst(cx, bytes, length, lifo, &module))
        return false;

    WasmPrintBuffer buf(buffer);
    WasmRenderContext c(cx, module, buf, sourceMap);

    if (!RenderModule(c, *module)) {
        if (!cx->isExceptionPending())
            ReportOutOfMemory(cx);
        return false;
    }

    return true;
}
