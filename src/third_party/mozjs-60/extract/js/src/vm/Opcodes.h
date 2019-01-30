/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=0 ft=c:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Opcodes_h
#define vm_Opcodes_h

#include "mozilla/Attributes.h"

#include <stddef.h>

/*
 * JavaScript operation bytecodes.  Add a new bytecode by claiming one of the
 * JSOP_UNUSED* here or by extracting the first unused opcode from
 * FOR_EACH_TRAILING_UNUSED_OPCODE and updating js::detail::LastDefinedOpcode
 * below.
 *
 * Includers must define a macro with the following form:
 *
 * #define MACRO(op,val,name,image,length,nuses,ndefs,format) ...
 *
 * Then simply use FOR_EACH_OPCODE(MACRO) to invoke MACRO for every opcode.
 * Selected arguments can be expanded in initializers.
 *
 * Field        Description
 * op           Bytecode name, which is the JSOp enumerator name
 * value        Bytecode value, which is the JSOp enumerator value
 * name         C string containing name for disassembler
 * image        C string containing "image" for pretty-printer, null if ugly
 * length       Number of bytes including any immediate operands
 * nuses        Number of stack slots consumed by bytecode, -1 if variadic
 * ndefs        Number of stack slots produced by bytecode, -1 if variadic
 * format       Bytecode plus immediate operand encoding format
 *
 * This file is best viewed with 128 columns:
12345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678
 */

/*
 * SpiderMonkey bytecode categorization (as used in generated documentation):
 *
 * [Index]
 *   [Statements]
 *     Jumps
 *     Switch Statement
 *     For-In Statement
 *     With Statement
 *     Exception Handling
 *     Function
 *     Generator
 *     Debugger
 *   [Variables and Scopes]
 *     Variables
 *     Free Variables
 *     Local Variables
 *     Aliased Variables
 *     Intrinsics
 *     Block-local Scope
 *     This
 *     Super
 *     Arguments
 *     Var Scope
 *   [Operators]
 *     Comparison Operators
 *     Arithmetic Operators
 *     Bitwise Logical Operators
 *     Bitwise Shift Operators
 *     Logical Operators
 *     Special Operators
 *     Stack Operations
 *     Debugger
 *   [Literals]
 *     Constants
 *     Object
 *     Array
 *     RegExp
 *     Class
 *   [Other]
 */

#define FOR_EACH_OPCODE(macro) \
    /* legend:  op      val   name          image       len use def  format */ \
    /*
     * No operation is performed.
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_NOP,       0,  "nop",        NULL,         1,  0,  0, JOF_BYTE) \
    \
    /* Long-standing JavaScript bytecodes. */ \
    /*
     * Pushes 'undefined' onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => undefined
     */ \
    macro(JSOP_UNDEFINED, 1,  js_undefined_str, "",       1,  0,  1, JOF_BYTE) \
    /*
     * Pushes stack frame's 'rval' onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: => rval
     */ \
    macro(JSOP_GETRVAL,   2,  "getrval",    NULL,         1,  0,  1, JOF_BYTE) \
    /*
     * Pops the top of stack value, converts it to an object, and adds a
     * 'WithEnvironmentObject' wrapping that object to the environment chain.
     *
     * There is a matching JSOP_LEAVEWITH instruction later. All name
     * lookups between the two that may need to consult the With object
     * are deoptimized.
     *   Category: Statements
     *   Type: With Statement
     *   Operands: uint32_t staticWithIndex
     *   Stack: val =>
     */ \
    macro(JSOP_ENTERWITH, 3,  "enterwith",  NULL,         5,  1,  0, JOF_SCOPE) \
    /*
     * Pops the environment chain object pushed by JSOP_ENTERWITH.
     *   Category: Statements
     *   Type: With Statement
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_LEAVEWITH, 4,  "leavewith",  NULL,         1,  0,  0, JOF_BYTE) \
    /*
     * Pops the top of stack value as 'rval', stops interpretation of current
     * script and returns 'rval'.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: rval =>
     */ \
    macro(JSOP_RETURN,    5,  "return",     NULL,         1,  1,  0, JOF_BYTE) \
    /*
     * Jumps to a 32-bit offset from the current bytecode.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: =>
     */ \
    macro(JSOP_GOTO,      6,  "goto",       NULL,         5,  0,  0, JOF_JUMP) \
    /*
     * Pops the top of stack value, converts it into a boolean, if the result is
     * 'false', jumps to a 32-bit offset from the current bytecode.
     *
     * The idea is that a sequence like
     * JSOP_ZERO; JSOP_ZERO; JSOP_EQ; JSOP_IFEQ; JSOP_RETURN;
     * reads like a nice linear sequence that will execute the return.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: cond =>
     */ \
    macro(JSOP_IFEQ,      7,  "ifeq",       NULL,         5,  1,  0, JOF_JUMP|JOF_DETECTING) \
    /*
     * Pops the top of stack value, converts it into a boolean, if the result is
     * 'true', jumps to a 32-bit offset from the current bytecode.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: cond =>
     */ \
    macro(JSOP_IFNE,      8,  "ifne",       NULL,         5,  1,  0, JOF_JUMP) \
    \
    /*
     * Pushes the 'arguments' object for the current function activation.
     *
     * If 'JSScript' is not marked 'needsArgsObj', then a
     * JS_OPTIMIZED_ARGUMENTS magic value is pushed. Otherwise, a proper
     * arguments object is constructed and pushed.
     *
     * This opcode requires that the function does not have rest parameter.
     *   Category: Variables and Scopes
     *   Type: Arguments
     *   Operands:
     *   Stack: => arguments
     */ \
    macro(JSOP_ARGUMENTS, 9,  "arguments",  NULL,         1,  0,  1, JOF_BYTE) \
    \
    /*
     * Swaps the top two values on the stack. This is useful for things like
     * post-increment/decrement.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands:
     *   Stack: v1, v2 => v2, v1
     */ \
    macro(JSOP_SWAP,      10, "swap",       NULL,         1,  2,  2, JOF_BYTE) \
    /*
     * Pops the top 'n' values from the stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands: uint16_t n
     *   Stack: v[n-1], ..., v[1], v[0] =>
     *   nuses: n
     */ \
    macro(JSOP_POPN,      11, "popn",       NULL,         3, -1,  0, JOF_UINT16) \
    \
    /* More long-standing bytecodes. */ \
    /*
     * Pushes a copy of the top value on the stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands:
     *   Stack: v => v, v
     */ \
    macro(JSOP_DUP,       12, "dup",        NULL,         1,  1,  2, JOF_BYTE) \
    /*
     * Duplicates the top two values on the stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands:
     *   Stack: v1, v2 => v1, v2, v1, v2
     */ \
    macro(JSOP_DUP2,      13, "dup2",       NULL,         1,  2,  4, JOF_BYTE) \
    \
    /*
     * Checks that the top value on the stack is an object, and throws a
     * TypeError if not. The operand 'kind' is used only to generate an
     * appropriate error message.
     *   Category: Statements
     *   Type: Generator
     *   Operands: uint8_t kind
     *   Stack: result => result
     */ \
    macro(JSOP_CHECKISOBJ,14, "checkisobj", NULL,         2,  1,  1, JOF_UINT8) \
    \
    /*
     * Pops the top two values 'lval' and 'rval' from the stack, then pushes
     * the result of the operation applied to the two operands, converting
     * both to 32-bit signed integers if necessary.
     *   Category: Operators
     *   Type: Bitwise Logical Operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    macro(JSOP_BITOR,     15, "bitor",      "|",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_BITXOR,    16, "bitxor",     "^",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_BITAND,    17, "bitand",     "&",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    /*
     * Pops the top two values from the stack and pushes the result of
     * comparing them.
     *   Category: Operators
     *   Type: Comparison Operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    macro(JSOP_EQ,        18, "eq",         "==",         1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH|JOF_DETECTING) \
    macro(JSOP_NE,        19, "ne",         "!=",         1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH|JOF_DETECTING) \
    macro(JSOP_LT,        20, "lt",         "<",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_LE,        21, "le",         "<=",         1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_GT,        22, "gt",         ">",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_GE,        23, "ge",         ">=",         1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    /*
     * Pops the top two values 'lval' and 'rval' from the stack, then pushes
     * the result of the operation applied to the operands.
     *   Category: Operators
     *   Type: Bitwise Shift Operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    macro(JSOP_LSH,       24, "lsh",        "<<",         1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_RSH,       25, "rsh",        ">>",         1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    /*
     * Pops the top two values 'lval' and 'rval' from the stack, then pushes
     * 'lval >>> rval'.
     *   Category: Operators
     *   Type: Bitwise Shift Operators
     *   Operands:
     *   Stack: lval, rval => (lval >>> rval)
     */ \
    macro(JSOP_URSH,      26, "ursh",       ">>>",        1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    /*
     * Pops the top two values 'lval' and 'rval' from the stack, then pushes
     * the result of 'lval + rval'.
     *   Category: Operators
     *   Type: Arithmetic Operators
     *   Operands:
     *   Stack: lval, rval => (lval + rval)
     */ \
    macro(JSOP_ADD,       27, "add",        "+",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    /*
     * Pops the top two values 'lval' and 'rval' from the stack, then pushes
     * the result of applying the arithmetic operation to them.
     *   Category: Operators
     *   Type: Arithmetic Operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    macro(JSOP_SUB,       28, "sub",        "-",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_MUL,       29, "mul",        "*",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_DIV,       30, "div",        "/",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_MOD,       31, "mod",        "%",          1,  2,  1, JOF_BYTE|JOF_LEFTASSOC|JOF_ARITH) \
    /*
     * Pops the value 'val' from the stack, then pushes '!val'.
     *   Category: Operators
     *   Type: Logical Operators
     *   Operands:
     *   Stack: val => (!val)
     */ \
    macro(JSOP_NOT,       32, "not",        "!",          1,  1,  1, JOF_BYTE|JOF_ARITH|JOF_DETECTING) \
    /*
     * Pops the value 'val' from the stack, then pushes '~val'.
     *   Category: Operators
     *   Type: Bitwise Logical Operators
     *   Operands:
     *   Stack: val => (~val)
     */ \
    macro(JSOP_BITNOT,    33, "bitnot",     "~",          1,  1,  1, JOF_BYTE|JOF_ARITH) \
    /*
     * Pops the value 'val' from the stack, then pushes '-val'.
     *   Category: Operators
     *   Type: Arithmetic Operators
     *   Operands:
     *   Stack: val => (-val)
     */ \
    macro(JSOP_NEG,       34, "neg",        "- ",         1,  1,  1, JOF_BYTE|JOF_ARITH) \
    /*
     * Pops the value 'val' from the stack, then pushes '+val'.
     * ('+val' is the value converted to a number.)
     *   Category: Operators
     *   Type: Arithmetic Operators
     *   Operands:
     *   Stack: val => (+val)
     */ \
    macro(JSOP_POS,       35, "pos",        "+ ",         1,  1,  1, JOF_BYTE|JOF_ARITH) \
    /*
     * Looks up name on the environment chain and deletes it, pushes 'true'
     * onto the stack if succeeded (if the property was present and deleted or
     * if the property wasn't present in the first place), 'false' if not.
     *
     * Strict mode code should never contain this opcode.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: => succeeded
     */ \
    macro(JSOP_DELNAME,   36, "delname",    NULL,         5,  0,  1, JOF_ATOM|JOF_NAME|JOF_CHECKSLOPPY) \
    /*
     * Pops the top of stack value, deletes property from it, pushes 'true' onto
     * the stack if succeeded, 'false' if not.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands: uint32_t nameIndex
     *   Stack: obj => succeeded
     */ \
    macro(JSOP_DELPROP,   37, "delprop",    NULL,         5,  1,  1, JOF_ATOM|JOF_PROP|JOF_CHECKSLOPPY) \
    /*
     * Pops the top two values on the stack as 'propval' and 'obj',
     * deletes 'propval' property from 'obj', pushes 'true'  onto the stack if
     * succeeded, 'false' if not.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands:
     *   Stack: obj, propval => succeeded
     */ \
    macro(JSOP_DELELEM,   38, "delelem",    NULL,         1,  2,  1, JOF_BYTE |JOF_ELEM|JOF_CHECKSLOPPY) \
    /*
     * Pops the value 'val' from the stack, then pushes 'typeof val'.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands:
     *   Stack: val => (typeof val)
     */ \
    macro(JSOP_TYPEOF,    39, js_typeof_str,NULL,         1,  1,  1, JOF_BYTE|JOF_DETECTING) \
    /*
     * Pops the top value on the stack and pushes 'undefined'.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands:
     *   Stack: val => undefined
     */ \
    macro(JSOP_VOID,      40, js_void_str,  NULL,         1,  1,  1, JOF_BYTE) \
    \
    /*
     * spreadcall variant of JSOP_CALL.
     *
     * Invokes 'callee' with 'this' and 'args', pushes the return value onto
     * the stack.
     *
     * 'args' is an Array object which contains actual arguments.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: callee, this, args => rval
     */ \
    macro(JSOP_SPREADCALL,41, "spreadcall", NULL,         1,  3,  1, JOF_BYTE|JOF_INVOKE|JOF_TYPESET) \
    /*
     * spreadcall variant of JSOP_NEW
     *
     * Invokes 'callee' as a constructor with 'this' and 'args', pushes the
     * return value onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: callee, this, args, newTarget => rval
     */ \
    macro(JSOP_SPREADNEW, 42, "spreadnew",  NULL,         1,  4,  1, JOF_BYTE|JOF_INVOKE|JOF_TYPESET) \
    /*
     * spreadcall variant of JSOP_EVAL
     *
     * Invokes 'eval' with 'args' and pushes the return value onto the stack.
     *
     * If 'eval' in global scope is not original one, invokes the function
     * with 'this' and 'args', and pushes return value onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: callee, this, args => rval
     */ \
    macro(JSOP_SPREADEVAL,43, "spreadeval", NULL,         1,  3,  1, JOF_BYTE|JOF_INVOKE|JOF_TYPESET|JOF_CHECKSLOPPY) \
    \
    /*
     * Duplicates the Nth value from the top onto the stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands: uint24_t n
     *   Stack: v[n], v[n-1], ..., v[1], v[0] =>
     *          v[n], v[n-1], ..., v[1], v[0], v[n]
     */ \
    macro(JSOP_DUPAT,     44, "dupat",      NULL,         4,  0,  1,  JOF_UINT24) \
    \
    /*
     * Push a well-known symbol onto the operand stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: uint8_t symbol (the JS::SymbolCode of the symbol to use)
     *   Stack: => symbol
     */ \
    macro(JSOP_SYMBOL,    45, "symbol",     NULL,         2,  0,  1,  JOF_UINT8) \
    \
    /*
     * Pops the top of stack value and attempts to delete the given property
     * from it. Pushes 'true' onto success, else throws a TypeError per strict
     * mode property-deletion requirements.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands: uint32_t nameIndex
     *   Stack: obj => succeeded
     */ \
    macro(JSOP_STRICTDELPROP,   46, "strict-delprop",    NULL,         5,  1,  1, JOF_ATOM|JOF_PROP|JOF_CHECKSTRICT) \
    /*
     * Pops the top two values on the stack as 'propval' and 'obj',
     * and attempts to delete 'propval' property from 'obj'. Pushes 'true' onto
     * the stack on success, else throws a TypeError per strict mode property
     * deletion requirements.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, propval => succeeded
     */ \
    macro(JSOP_STRICTDELELEM,   47, "strict-delelem",    NULL,         1,  2,  1, JOF_BYTE|JOF_ELEM|JOF_CHECKSTRICT) \
    /*
     * Pops the top two values on the stack as 'val' and 'obj', and performs
     * 'obj.prop = val', pushing 'val' back onto the stack. Throws a TypeError
     * if the set-operation failed (per strict mode semantics).
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => val
     */ \
    macro(JSOP_STRICTSETPROP,   48, "strict-setprop",    NULL,         5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSTRICT) \
    /*
     * Pops a environment and value from the stack, assigns value to the given
     * name, and pushes the value back on the stack. If the set failed, then
     * throw a TypeError, per usual strict mode semantics.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    macro(JSOP_STRICTSETNAME,   49, "strict-setname",    NULL,         5,  2,  1,  JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSTRICT) \
    /*
     * spreadcall variant of JSOP_EVAL
     *
     * Invokes 'eval' with 'args' and pushes the return value onto the stack.
     *
     * If 'eval' in global scope is not original one, invokes the function
     * with 'this' and 'args', and pushes return value onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: callee, this, args => rval
     */ \
    macro(JSOP_STRICTSPREADEVAL,      50, "strict-spreadeval", NULL,         1,  3,  1, JOF_BYTE|JOF_INVOKE|JOF_TYPESET|JOF_CHECKSTRICT) \
    /*
     * Ensures the result of a class's heritage expression is either null or a constructor.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: heritage => heritage
     */ \
    macro(JSOP_CHECKCLASSHERITAGE,  51, "checkclassheritage",   NULL, 1,  1,  1,  JOF_BYTE) \
    /*
     * Pushes a clone of a function with a given [[Prototype]] onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint32_t funcIndex
     *   Stack: proto => obj
     */ \
    macro(JSOP_FUNWITHPROTO,   52, "funwithproto",    NULL,         5,  1,  1,  JOF_OBJECT) \
    \
    /*
     * Pops the top of stack value, pushes property of it onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj => obj[name]
     */ \
    macro(JSOP_GETPROP,   53, "getprop",    NULL,         5,  1,  1, JOF_ATOM|JOF_PROP|JOF_TYPESET) \
    /*
     * Pops the top two values on the stack as 'val' and 'obj' and performs
     * 'obj.prop = val', pushing 'val' back onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => val
     */ \
    macro(JSOP_SETPROP,   54, "setprop",    NULL,         5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSLOPPY) \
    /*
     * Pops the top two values on the stack as 'propval' and 'obj', pushes
     * 'propval' property of 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, propval => obj[propval]
     */ \
    macro(JSOP_GETELEM,   55, "getelem",    NULL,         1,  2,  1, JOF_BYTE |JOF_ELEM|JOF_TYPESET|JOF_LEFTASSOC) \
    /*
     * Pops the top three values on the stack as 'val', 'propval' and 'obj',
     * sets 'propval' property of 'obj' as 'val', pushes 'obj' onto the
     * stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, propval, val => val
     */ \
    macro(JSOP_SETELEM,   56, "setelem",    NULL,         1,  3,  1, JOF_BYTE |JOF_ELEM|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSLOPPY) \
    /*
     * Pops the top three values on the stack as 'val', 'propval' and 'obj',
     * sets 'propval' property of 'obj' as 'val', pushes 'obj' onto the
     * stack. Throws a TypeError if the set fails, per strict mode
     * semantics.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, propval, val => val
     */ \
    macro(JSOP_STRICTSETELEM,   57, "strict-setelem",    NULL,         1,  3,  1, JOF_BYTE |JOF_ELEM|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSTRICT) \
    /*
     * Invokes 'callee' with 'this' and 'args', pushes return value onto the
     * stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     *   nuses: (argc+2)
     */ \
    macro(JSOP_CALL,      58, "call",       NULL,         3, -1,  1, JOF_UINT16|JOF_INVOKE|JOF_TYPESET) \
    /*
     * Looks up name on the environment chain and pushes its value onto the stack.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: => val
     */ \
    macro(JSOP_GETNAME,   59, "getname",    NULL,         5,  0,  1, JOF_ATOM|JOF_NAME|JOF_TYPESET) \
    /*
     * Pushes numeric constant onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: uint32_t constIndex
     *   Stack: => val
     */ \
    macro(JSOP_DOUBLE,    60, "double",     NULL,         5,  0,  1, JOF_DOUBLE) \
    /*
     * Pushes string constant onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: uint32_t atomIndex
     *   Stack: => atom
     */ \
    macro(JSOP_STRING,    61, "string",     NULL,         5,  0,  1, JOF_ATOM) \
    /*
     * Pushes '0' onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => 0
     */ \
    macro(JSOP_ZERO,      62, "zero",       "0",          1,  0,  1, JOF_BYTE) \
    /*
     * Pushes '1' onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => 1
     */ \
    macro(JSOP_ONE,       63, "one",        "1",          1,  0,  1, JOF_BYTE) \
    /*
     * Pushes 'null' onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => null
     */ \
    macro(JSOP_NULL,      64, js_null_str,  js_null_str,  1,  0,  1, JOF_BYTE) \
    /*
     * Pushes 'JS_IS_CONSTRUCTING'
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => JS_IS_CONSTRUCTING
     */ \
    macro(JSOP_IS_CONSTRUCTING,  65, "is-constructing",   NULL,         1,  0,  1, JOF_BYTE) \
    /*
     * Pushes boolean value onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => true/false
     */ \
    macro(JSOP_FALSE,     66, js_false_str, js_false_str, 1,  0,  1, JOF_BYTE) \
    macro(JSOP_TRUE,      67, js_true_str,  js_true_str,  1,  0,  1, JOF_BYTE) \
    /*
     * Converts the top of stack value into a boolean, if the result is 'true',
     * jumps to a 32-bit offset from the current bytecode.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: cond => cond
     */ \
    macro(JSOP_OR,        68, "or",         NULL,         5,  1,  1, JOF_JUMP|JOF_DETECTING|JOF_LEFTASSOC) \
    /*
     * Converts the top of stack value into a boolean, if the result is 'false',
     * jumps to a 32-bit offset from the current bytecode.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: cond => cond
     */ \
    macro(JSOP_AND,       69, "and",        NULL,         5,  1,  1, JOF_JUMP|JOF_DETECTING|JOF_LEFTASSOC) \
    \
    /*
     * Pops the top of stack value as 'i', if 'low <= i <= high',
     * jumps to a 32-bit offset: 'offset[i - low]' from the current bytecode,
     * jumps to a 32-bit offset: 'len' from the current bytecode if not.
     *
     * This opcode has variable length.
     *   Category: Statements
     *   Type: Switch Statement
     *   Operands: int32_t len, int32_t low, int32_t high,
     *             int32_t offset[0], ..., int32_t offset[high-low]
     *   Stack: i =>
     *   len: len
     */ \
    macro(JSOP_TABLESWITCH, 70, "tableswitch", NULL,     -1,  1,  0,  JOF_TABLESWITCH|JOF_DETECTING) \
    \
    /*
     * Prologue emitted in scripts expected to run once, which deoptimizes code
     * if it executes multiple times.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_RUNONCE,   71, "runonce",    NULL,         1,  0,  0,  JOF_BYTE) \
    \
    /* New, infallible/transitive identity ops. */ \
    /*
     * Pops the top two values from the stack, then pushes the result of
     * applying the operator to the two values.
     *   Category: Operators
     *   Type: Comparison Operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    macro(JSOP_STRICTEQ,  72, "stricteq",   "===",        1,  2,  1, JOF_BYTE|JOF_DETECTING|JOF_LEFTASSOC|JOF_ARITH) \
    macro(JSOP_STRICTNE,  73, "strictne",   "!==",        1,  2,  1, JOF_BYTE|JOF_DETECTING|JOF_LEFTASSOC|JOF_ARITH) \
    \
    /*
     * Sometimes we know when emitting that an operation will always throw.
     *
     * Throws the indicated JSMSG.
     *
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands: uint16_t msgNumber
     *   Stack: =>
     */ \
    macro(JSOP_THROWMSG,   74, "throwmsg",    NULL,         3,  0,  0, JOF_UINT16) \
    \
    /*
     * Sets up a for-in loop. It pops the top of stack value as 'val' and pushes
     * 'iter' which is an iterator for 'val'.
     *   Category: Statements
     *   Type: For-In Statement
     *   Operands:
     *   Stack: val => iter
     */ \
    macro(JSOP_ITER,      75, "iter",       NULL,         1,  1,  1,  JOF_BYTE) \
    /*
     * Pushes the next iterated value onto the stack. If no value is available,
     * MagicValue(JS_NO_ITER_VALUE) is pushed.
     *
     *   Category: Statements
     *   Type: For-In Statement
     *   Operands:
     *   Stack: iter => iter, val
     */ \
    macro(JSOP_MOREITER,  76, "moreiter",   NULL,         1,  1,  2,  JOF_BYTE) \
    /*
     * Pushes a boolean indicating whether the value on top of the stack is
     * MagicValue(JS_NO_ITER_VALUE).
     *
     *   Category: Statements
     *   Type: For-In Statement
     *   Operands:
     *   Stack: val => val, res
     */ \
    macro(JSOP_ISNOITER,  77, "isnoiter",   NULL,         1,  1,  2,  JOF_BYTE) \
    /*
     * Exits a for-in loop by popping the iterator object from the stack and
     * closing it.
     *   Category: Statements
     *   Type: For-In Statement
     *   Operands:
     *   Stack: iter =>
     */ \
    macro(JSOP_ENDITER,   78, "enditer",    NULL,         1,  1,  0,  JOF_BYTE) \
    \
    /*
     * Invokes 'callee' with 'this' and 'args', pushes return value onto the
     * stack.
     *
     * This is for 'f.apply'.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     *   nuses: (argc+2)
     */ \
    macro(JSOP_FUNAPPLY,  79, "funapply",   NULL,         3, -1,  1,  JOF_UINT16|JOF_INVOKE|JOF_TYPESET) \
    \
    /*
     * Pushes deep-cloned object literal or singleton onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t objectIndex
     *   Stack: => obj
     */ \
    macro(JSOP_OBJECT,    80, "object",     NULL,         5,  0,  1,  JOF_OBJECT) \
    \
    /*
     * Pops the top value off the stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands:
     *   Stack: v =>
     */ \
    macro(JSOP_POP,       81, "pop",        NULL,         1,  1,  0,  JOF_BYTE) \
    \
    /*
     * Invokes 'callee' as a constructor with 'this' and 'args', pushes return
     * value onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1], newTarget => rval
     *   nuses: (argc+3)
     */ \
    macro(JSOP_NEW,       82, "new",   NULL,         3, -1,  1,  JOF_UINT16|JOF_INVOKE|JOF_TYPESET) \
    /*
     * Pushes newly created object onto the stack with provided [[Prototype]].
     *
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: proto => obj
     */ \
    macro(JSOP_OBJWITHPROTO,  83, "objwithproto",   NULL,         1,  1,  1,  JOF_BYTE) \
    \
    /*
     * Fast get op for function arguments and local variables.
     *
     * Pushes 'arguments[argno]' onto the stack.
     *   Category: Variables and Scopes
     *   Type: Arguments
     *   Operands: uint16_t argno
     *   Stack: => arguments[argno]
     */ \
    macro(JSOP_GETARG,    84, "getarg",     NULL,         3,  0,  1,  JOF_QARG |JOF_NAME) \
    /*
     * Fast set op for function arguments and local variables.
     *
     * Sets 'arguments[argno]' as the top of stack value.
     *   Category: Variables and Scopes
     *   Type: Arguments
     *   Operands: uint16_t argno
     *   Stack: v => v
     */ \
    macro(JSOP_SETARG,    85, "setarg",     NULL,         3,  1,  1,  JOF_QARG |JOF_NAME) \
    /*
     * Pushes the value of local variable onto the stack.
     *   Category: Variables and Scopes
     *   Type: Local Variables
     *   Operands: uint24_t localno
     *   Stack: => val
     */ \
    macro(JSOP_GETLOCAL,  86,"getlocal",    NULL,         4,  0,  1,  JOF_LOCAL|JOF_NAME) \
    /*
     * Stores the top stack value to the given local.
     *   Category: Variables and Scopes
     *   Type: Local Variables
     *   Operands: uint24_t localno
     *   Stack: v => v
     */ \
    macro(JSOP_SETLOCAL,  87,"setlocal",    NULL,         4,  1,  1,  JOF_LOCAL|JOF_NAME|JOF_DETECTING) \
    \
    /*
     * Pushes unsigned 16-bit int immediate integer operand onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: uint16_t val
     *   Stack: => val
     */ \
    macro(JSOP_UINT16,    88, "uint16",     NULL,         3,  0,  1,  JOF_UINT16) \
    \
    /* Object and array literal support. */ \
    /*
     * Pushes newly created object onto the stack.
     *
     * This opcode takes the kind of initializer (JSProto_Array or
     * JSProto_Object).
     *
     * This opcode has three extra bytes so it can be exchanged with
     * JSOP_NEWOBJECT during emit.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint8_t kind (, uint24_t extra)
     *   Stack: => obj
     */ \
    macro(JSOP_NEWINIT,   89, "newinit",    NULL,         5,  0,  1, JOF_UINT8) \
    /*
     * Pushes newly created array onto the stack.
     *
     * This opcode takes the final length, which is preallocated.
     *   Category: Literals
     *   Type: Array
     *   Operands: uint32_t length
     *   Stack: => obj
     */ \
    macro(JSOP_NEWARRAY,  90, "newarray",   NULL,         5,  0,  1, JOF_UINT32) \
    /*
     * Pushes newly created object onto the stack.
     *
     * This opcode takes an object with the final shape, which can be set at
     * the start and slots then filled in directly.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t baseobjIndex
     *   Stack: => obj
     */ \
    macro(JSOP_NEWOBJECT, 91, "newobject",  NULL,         5,  0,  1, JOF_OBJECT) \
    \
    /*
     * Initialize the home object for functions with super bindings.
     *
     * This opcode takes the function and the object to be the home object, does
     * the set, and leaves both on the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint8_t n
     *   Stack: homeObject, [...n], fun => homeObject, [...n], fun
     */\
    macro(JSOP_INITHOMEOBJECT,  92, "inithomeobject",   NULL,         2,  2,  2, JOF_UINT8) \
    \
    /*
     * Initialize a named property in an object literal, like '{a: x}'.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines
     * 'nameIndex' property of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITPROP,  93, "initprop",   NULL,         5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    \
    /*
     * Initialize a numeric property in an object literal, like '{1: x}'.
     *
     * Pops the top three values on the stack as 'val', 'id' and 'obj', defines
     * 'id' property of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    macro(JSOP_INITELEM,  94, "initelem",   NULL,         1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    \
    /*
     * Pops the top three values on the stack as 'val', 'index' and 'obj', sets
     * 'index' property of 'obj' as 'val', pushes 'obj' and 'index + 1' onto
     * the stack.
     *
     * This opcode is used in Array literals with spread and spreadcall
     * arguments.
     *   Category: Literals
     *   Type: Array
     *   Operands:
     *   Stack: obj, index, val => obj, (index + 1)
     */ \
    macro(JSOP_INITELEM_INC,95, "initelem_inc", NULL,     1,  3,  2, JOF_BYTE|JOF_ELEM|JOF_PROPINIT) \
    \
    /*
     * Initialize an array element.
     *
     * Pops the top two values on the stack as 'val' and 'obj', sets 'index'
     * property of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Array
     *   Operands: uint32_t index
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITELEM_ARRAY,96, "initelem_array", NULL, 5,  2,  1,  JOF_UINT32|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    \
    /*
     * Initialize a getter in an object literal.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines getter
     * of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITPROP_GETTER,  97, "initprop_getter",   NULL, 5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a setter in an object literal.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines setter
     * of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITPROP_SETTER,  98, "initprop_setter",   NULL, 5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a numeric getter in an object literal like
     * '{get 2() {}}'.
     *
     * Pops the top three values on the stack as 'val', 'id' and 'obj', defines
     * 'id' getter of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    macro(JSOP_INITELEM_GETTER,  99, "initelem_getter",   NULL, 1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a numeric setter in an object literal like
     * '{set 2(v) {}}'.
     *
     * Pops the top three values on the stack as 'val', 'id' and 'obj', defines
     * 'id' setter of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    macro(JSOP_INITELEM_SETTER, 100, "initelem_setter",   NULL, 1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Pushes the call site object specified by objectIndex onto the stack. Defines the raw
     * property specified by objectIndex + 1 on the call site object and freezes both the call site
     * object as well as its raw property.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t objectIndex
     *   Stack: => obj
     */ \
    macro(JSOP_CALLSITEOBJ,     101, "callsiteobj",     NULL,         5,  0,  1,  JOF_OBJECT) \
    \
    /*
     * Pushes a newly created array onto the stack, whose elements are the same
     * as that of a template object's copy on write elements.
     *
     *   Category: Literals
     *   Type: Array
     *   Operands: uint32_t objectIndex
     *   Stack: => obj
     */ \
    macro(JSOP_NEWARRAY_COPYONWRITE, 102, "newarray_copyonwrite", NULL, 5, 0, 1, JOF_OBJECT) \
    \
    /*
     * Pushes the prototype of the home object for current callee onto the
     * stack.
     *   Category: Variables and Scopes
     *   Type: Super
     *   Operands:
     *   Stack: => homeObjectProto
     */\
    macro(JSOP_SUPERBASE,  103, "superbase",   NULL,         1,  0,  1,  JOF_BYTE) \
    /*
     * Pops the top two values, and pushes the property of one, using the other
     * as the receiver.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: receiver, obj => obj[name]
     */ \
    macro(JSOP_GETPROP_SUPER,   104, "getprop-super", NULL, 5,  2,  1, JOF_ATOM|JOF_PROP|JOF_TYPESET) \
    /*
     * Pops the top three values on the stack as 'val' and 'obj', and 'receiver',
     * and performs 'obj.prop = val', pushing 'val' back onto the stack.
     * Throws a TypeError if the set-operation failed (per strict mode semantics).
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: receiver, obj, val => val
     */ \
    macro(JSOP_STRICTSETPROP_SUPER,   105, "strictsetprop-super",    NULL,         5,  3,  1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSTRICT) \
    \
    /*
     * This opcode precedes every labeled statement. It's a no-op.
     *
     * 'offset' is the offset to the next instruction after this statement,
     * the one 'break LABEL;' would jump to. IonMonkey uses this.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: =>
     */ \
    macro(JSOP_LABEL,     106,"label",     NULL,          5,  0,  0,  JOF_JUMP) \
    \
    /*
     * Pops the top three values on the stack as 'val', 'obj' and 'receiver',
     * and performs 'obj.prop = val', pushing 'val' back onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: receiver, obj, val => val
     */ \
    macro(JSOP_SETPROP_SUPER,   107, "setprop-super",    NULL,         5,  3,  1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSLOPPY) \
    \
    /*
     * Invokes 'callee' with 'this' and 'args', pushes return value onto the
     * stack.
     *
     * If 'callee' is determined to be the canonical 'Function.prototype.call'
     * function, then this operation is optimized to directly call 'callee'
     * with 'args[0]' as 'this', and the remaining arguments as formal args
     * to 'callee'.
     *
     * Like JSOP_FUNAPPLY but for 'f.call' instead of 'f.apply'.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     *   nuses: (argc+2)
     */ \
    macro(JSOP_FUNCALL,   108,"funcall",    NULL,         3, -1,  1, JOF_UINT16|JOF_INVOKE|JOF_TYPESET) \
    \
    /*
     * Another no-op.
     *
     * This opcode is the target of the backwards jump for some loop.
     *   Category: Statements
     *   Type: Jumps
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_LOOPHEAD,  109,"loophead",   NULL,         1,  0,  0,  JOF_BYTE) \
    \
    /* ECMA-compliant assignment ops. */ \
    /*
     * Looks up name on the environment chain and pushes the environment which
     * contains the name onto the stack. If not found, pushes global
     * lexical environment onto the stack.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: => env
     */ \
    macro(JSOP_BINDNAME,  110,"bindname",   NULL,         5,  0,  1,  JOF_ATOM|JOF_NAME) \
    /*
     * Pops an environment and value from the stack, assigns value to the
     * given name, and pushes the value back on the stack
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    macro(JSOP_SETNAME,   111,"setname",    NULL,         5,  2,  1,  JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSLOPPY) \
    \
    /* Exception handling ops. */ \
    /*
     * Pops the top of stack value as 'v', sets pending exception as 'v', then
     * raises error.
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands:
     *   Stack: v =>
     */ \
    macro(JSOP_THROW,     112,js_throw_str, NULL,         1,  1,  0,  JOF_BYTE) \
    \
    /*
     * Pops the top two values 'id' and 'obj' from the stack, then pushes
     * 'id in obj'.  This will throw a 'TypeError' if 'obj' is not an object.
     *
     * Note that 'obj' is the top value.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands:
     *   Stack: id, obj => (id in obj)
     */ \
    macro(JSOP_IN,        113,js_in_str,    js_in_str,    1,  2,  1, JOF_BYTE|JOF_LEFTASSOC) \
    /*
     * Pops the top two values 'obj' and 'ctor' from the stack, then pushes
     * 'obj instanceof ctor'.  This will throw a 'TypeError' if 'obj' is not an
     * object.
     *   Category: Operators
     *   Type: Special Operators
     *   Operands:
     *   Stack: obj, ctor => (obj instanceof ctor)
     */ \
    macro(JSOP_INSTANCEOF,114,js_instanceof_str,js_instanceof_str,1,2,1,JOF_BYTE|JOF_LEFTASSOC) \
    \
    /*
     * Invokes debugger.
     *   Category: Statements
     *   Type: Debugger
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_DEBUGGER,  115,"debugger",   NULL,         1,  0,  0, JOF_BYTE) \
    \
    /*
     * Pushes 'false' and next bytecode's PC onto the stack, and jumps to
     * a 32-bit offset from the current bytecode.
     *
     * This opcode is used for entering 'finally' block.
     * When the execution resumes from 'finally' block, those stack values are
     * popped.
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands: int32_t offset
     *   Stack: =>
     */ \
    macro(JSOP_GOSUB,     116,"gosub",      NULL,         5,  0,  0,  JOF_JUMP) \
    /*
     * Pops the top two values on the stack as 'rval' and 'lval', converts
     * 'lval' into a boolean, raises error if the result is 'true',
     * jumps to a 32-bit absolute PC: 'rval' if 'false'.
     *
     * This opcode is used for returning from 'finally' block.
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands:
     *   Stack: lval, rval =>
     */ \
    macro(JSOP_RETSUB,    117,"retsub",     NULL,         1,  2,  0,  JOF_BYTE) \
    \
    /* More exception handling ops. */ \
    /*
     * Pushes the current pending exception onto the stack and clears the
     * pending exception. This is only emitted at the beginning of code for a
     * catch-block, so it is known that an exception is pending. It is used to
     * implement catch-blocks and 'yield*'.
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands:
     *   Stack: => exception
     */ \
    macro(JSOP_EXCEPTION, 118,"exception",  NULL,         1,  0,  1,  JOF_BYTE) \
    \
    /*
     * Embedded lineno to speedup 'pc->line' mapping.
     *   Category: Other
     *   Operands: uint32_t lineno
     *   Stack: =>
     */ \
    macro(JSOP_LINENO,    119,"lineno",     NULL,         5,  0,  0,  JOF_UINT32) \
    \
    /*
     * This no-op appears after the bytecode for EXPR in 'switch (EXPR) {...}'
     * if the switch cannot be optimized using JSOP_TABLESWITCH.
     * For a non-optimized switch statement like this:
     *
     *     switch (EXPR) {
     *       case V0:
     *         C0;
     *       ...
     *       default:
     *         D;
     *     }
     *
     * the bytecode looks like this:
     *
     *     (EXPR)
     *     condswitch
     *     (V0)
     *     case ->C0
     *     ...
     *     default ->D
     *     (C0)
     *     ...
     *     (D)
     *
     * Note that code for all case-labels is emitted first, then code for
     * the body of each case clause.
     *   Category: Statements
     *   Type: Switch Statement
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_CONDSWITCH,120,"condswitch", NULL,         1,  0,  0,  JOF_BYTE) \
    /*
     * Pops the top two values on the stack as 'rval' and 'lval', compare them
     * with '===', if the result is 'true', jumps to a 32-bit offset from the
     * current bytecode, re-pushes 'lval' onto the stack if 'false'.
     *   Category: Statements
     *   Type: Switch Statement
     *   Operands: int32_t offset
     *   Stack: lval, rval => lval(if lval !== rval)
     */ \
    macro(JSOP_CASE,      121,"case",       NULL,         5,  2,  1,  JOF_JUMP) \
    /*
     * This appears after all cases in a JSOP_CONDSWITCH, whether there is a
     * 'default:' label in the switch statement or not. Pop the switch operand
     * from the stack and jump to a 32-bit offset from the current bytecode.
     * offset from the current bytecode.
     *   Category: Statements
     *   Type: Switch Statement
     *   Operands: int32_t offset
     *   Stack: lval =>
     */ \
    macro(JSOP_DEFAULT,   122,"default",    NULL,         5,  1,  0,  JOF_JUMP) \
    \
    /* ECMA-compliant call to eval op. */ \
    /*
     * Invokes 'eval' with 'args' and pushes return value onto the stack.
     *
     * If 'eval' in global scope is not original one, invokes the function
     * with 'this' and 'args', and pushes return value onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     *   nuses: (argc+2)
     */ \
    macro(JSOP_EVAL,      123,"eval",       NULL,         3, -1,  1, JOF_UINT16|JOF_INVOKE|JOF_TYPESET|JOF_CHECKSLOPPY) \
    \
    /* ECMA-compliant call to eval op. */ \
    /*
     * Invokes 'eval' with 'args' and pushes return value onto the stack.
     *
     * If 'eval' in global scope is not original one, invokes the function
     * with 'this' and 'args', and pushes return value onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     *   nuses: (argc+2)
     */ \
    macro(JSOP_STRICTEVAL,       124, "strict-eval",       NULL,         3, -1,  1, JOF_UINT16|JOF_INVOKE|JOF_TYPESET|JOF_CHECKSTRICT) \
    /*
     * LIKE JSOP_GETELEM but takes receiver on stack, and the propval is
     * evaluated before the obj.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: propval, receiver, obj => obj[propval]
     */ \
    macro(JSOP_GETELEM_SUPER, 125, "getelem-super", NULL, 1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_TYPESET|JOF_LEFTASSOC) \
    macro(JSOP_UNUSED126, 126, "unused126", NULL, 5,  0,  1, JOF_UINT32) \
    \
    /*
     * Defines the given function on the current scope.
     *
     * This is used for global scripts and also in some cases for function
     * scripts where use of dynamic scoping inhibits optimization.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands:
     *   Stack: fun =>
     */ \
    macro(JSOP_DEFFUN,    127,"deffun",     NULL,         1,  1,  0,  JOF_BYTE) \
    /* Defines the new constant binding on global lexical environment.
     *
     * Throws if a binding with the same name already exists on the
     * environment, or if a var binding with the same name exists on the
     * global.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: =>
     */ \
    macro(JSOP_DEFCONST,  128,"defconst",   NULL,         5,  0,  0,  JOF_ATOM) \
    /*
     * Defines the new binding on the frame's current variables-object (the
     * environment on the environment chain designated to receive new
     * variables).
     *
     * Throws if the current variables-object is the global object and a
     * binding with the same name exists on the global lexical environment.
     *
     * This is used for global scripts and also in some cases for function
     * scripts where use of dynamic scoping inhibits optimization.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: =>
     */ \
    macro(JSOP_DEFVAR,    129,"defvar",     NULL,         5,  0,  0,  JOF_ATOM) \
    \
    /*
     * Pushes a closure for a named or anonymous function expression onto the
     * stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint32_t funcIndex
     *   Stack: => obj
     */ \
    macro(JSOP_LAMBDA,    130, "lambda",    NULL,         5,  0,  1, JOF_OBJECT) \
    /*
     * Pops the top of stack value as 'new.target', pushes an arrow function with
     * lexical 'new.target' onto the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint32_t funcIndex
     *   Stack: new.target => obj
     */ \
    macro(JSOP_LAMBDA_ARROW, 131, "lambda_arrow", NULL,   5,  1,  1, JOF_OBJECT) \
    \
    /*
     * Pushes current callee onto the stack.
     *
     * Used for named function expression self-naming, if lightweight.
     *   Category: Variables and Scopes
     *   Type: Arguments
     *   Operands:
     *   Stack: => callee
     */ \
    macro(JSOP_CALLEE,    132, "callee",    NULL,         1,  0,  1, JOF_BYTE) \
    \
    /*
     * Picks the nth element from the stack and moves it to the top of the
     * stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands: uint8_t n
     *   Stack: v[n], v[n-1], ..., v[1], v[0] => v[n-1], ..., v[1], v[0], v[n]
     */ \
    macro(JSOP_PICK,        133, "pick",      NULL,       2,  0,  0,  JOF_UINT8) \
    \
    /*
     * This no-op appears at the top of the bytecode for a 'TryStatement'.
     *
     * Location information for catch/finally blocks is stored in a
     * side table, 'script->trynotes()'.
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_TRY,         134,"try",        NULL,       1,  0,  0,  JOF_BYTE) \
    /*
     * This opcode has a def count of 2, but these values are already on the
     * stack (they're pushed by JSOP_GOSUB).
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands:
     *   Stack: => false, (next bytecode's PC)
     */ \
    macro(JSOP_FINALLY,     135,"finally",    NULL,       1,  0,  2,  JOF_BYTE) \
    \
    /*
     * Pushes aliased variable onto the stack.
     *
     * An "aliased variable" is a var, let, or formal arg that is aliased.
     * Sources of aliasing include: nested functions accessing the vars of an
     * enclosing function, function statements that are conditionally executed,
     * 'eval', 'with', and 'arguments'. All of these cases require creating a
     * CallObject to own the aliased variable.
     *
     * An ALIASEDVAR opcode contains the following immediates:
     *  uint8 hops: the number of environment objects to skip to find the
     *               EnvironmentObject containing the variable being accessed
     *  uint24 slot: the slot containing the variable in the EnvironmentObject
     *               (this 'slot' does not include RESERVED_SLOTS).
     *   Category: Variables and Scopes
     *   Type: Aliased Variables
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: => aliasedVar
     */ \
    macro(JSOP_GETALIASEDVAR, 136,"getaliasedvar",NULL,      5,  0,  1,  JOF_ENVCOORD|JOF_NAME|JOF_TYPESET) \
    /*
     * Sets aliased variable as the top of stack value.
     *   Category: Variables and Scopes
     *   Type: Aliased Variables
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: v => v
     */ \
    macro(JSOP_SETALIASEDVAR, 137,"setaliasedvar",NULL,      5,  1,  1,  JOF_ENVCOORD|JOF_NAME|JOF_PROPSET|JOF_DETECTING) \
    \
    /*
     * Checks if the value of the local variable is the
     * JS_UNINITIALIZED_LEXICAL magic, throwing an error if so.
     *   Category: Variables and Scopes
     *   Type: Local Variables
     *   Operands: uint24_t localno
     *   Stack: =>
     */ \
    macro(JSOP_CHECKLEXICAL,  138, "checklexical", NULL,     4,  0,  0, JOF_LOCAL|JOF_NAME) \
    /*
     * Initializes an uninitialized local lexical binding with the top of stack
     * value.
     *   Category: Variables and Scopes
     *   Type: Local Variables
     *   Operands: uint24_t localno
     *   Stack: v => v
     */ \
    macro(JSOP_INITLEXICAL,   139, "initlexical",  NULL,      4,  1,  1, JOF_LOCAL|JOF_NAME|JOF_DETECTING) \
    /*
     * Checks if the value of the aliased variable is the
     * JS_UNINITIALIZED_LEXICAL magic, throwing an error if so.
     *   Category: Variables and Scopes
     *   Type: Aliased Variables
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: =>
     */ \
    macro(JSOP_CHECKALIASEDLEXICAL, 140, "checkaliasedlexical", NULL, 5,  0,  0, JOF_ENVCOORD|JOF_NAME) \
    /*
     * Initializes an uninitialized aliased lexical binding with the top of
     * stack value.
     *   Category: Variables and Scopes
     *   Type: Aliased Variables
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: v => v
     */ \
    macro(JSOP_INITALIASEDLEXICAL,  141, "initaliasedlexical",  NULL, 5,  1,  1, JOF_ENVCOORD|JOF_NAME|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Pushes a JS_UNINITIALIZED_LEXICAL value onto the stack, representing an
     * uninitialized lexical binding.
     *
     * This opcode is used with the JSOP_INITLET opcode.
     *   Category: Literals
     *   Type: Constants
     *   Operands:
     *   Stack: => uninitialized
     */ \
    macro(JSOP_UNINITIALIZED, 142, "uninitialized", NULL, 1,  0,  1, JOF_BYTE) \
    /* Pushes the value of the intrinsic onto the stack.
     *
     * Intrinsic names are emitted instead of JSOP_*NAME ops when the
     * 'CompileOptions' flag 'selfHostingMode' is set.
     *
     * They are used in self-hosted code to access other self-hosted values and
     * intrinsic functions the runtime doesn't give client JS code access to.
     *   Category: Variables and Scopes
     *   Type: Intrinsics
     *   Operands: uint32_t nameIndex
     *   Stack: => intrinsic[name]
     */ \
    macro(JSOP_GETINTRINSIC,  143, "getintrinsic",  NULL, 5,  0,  1, JOF_ATOM|JOF_NAME|JOF_TYPESET) \
    /*
     * Stores the top stack value in the specified intrinsic.
     *   Category: Variables and Scopes
     *   Type: Intrinsics
     *   Operands: uint32_t nameIndex
     *   Stack: val => val
     */ \
    macro(JSOP_SETINTRINSIC,  144, "setintrinsic",  NULL, 5,  1,  1, JOF_ATOM|JOF_NAME|JOF_DETECTING) \
    /*
     * Like JSOP_CALL, but used as part of for-of and destructuring bytecode
     * to provide better error messages.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc (must be 0)
     *   Stack: callee, this => rval
     *   nuses: 2
     */ \
    macro(JSOP_CALLITER,      145, "calliter",      NULL, 3, -1,  1, JOF_UINT16|JOF_INVOKE|JOF_TYPESET) \
    /*
     * Initialize a non-configurable, non-writable, non-enumerable data-property on an object.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines
     * 'nameIndex' property of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITLOCKEDPROP, 146, "initlockedprop", NULL, 5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a non-enumerable data-property on an object.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines
     * 'nameIndex' property of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITHIDDENPROP, 147,"inithiddenprop", NULL, 5,  2,  1,  JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Push "new.target"
     *
     *   Category: Variables and Scopes
     *   Type: Arguments
     *   Operands:
     *   Stack: => new.target
     */ \
    macro(JSOP_NEWTARGET,  148, "newtarget", NULL,      1,  0,  1,  JOF_BYTE) \
    /*
     * Pops the top of stack value as 'unwrapped', converts it to async
     * function 'wrapped', and pushes 'wrapped' back on the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: unwrapped => wrapped
     */ \
    macro(JSOP_TOASYNC,       149, "toasync", NULL,       1,  1,  1, JOF_BYTE) \
    /*
     * Pops the top two values 'lval' and 'rval' from the stack, then pushes
     * the result of 'Math.pow(lval, rval)'.
     *   Category: Operators
     *   Type: Arithmetic Operators
     *   Operands:
     *   Stack: lval, rval => (lval ** rval)
     */ \
    macro(JSOP_POW,           150, "pow",     "**",       1,  2,  1, JOF_BYTE|JOF_ARITH) \
    \
    /*
     * Pops the top of stack value as 'v', sets pending exception as 'v',
     * to trigger rethrow.
     *
     * This opcode is used in conditional catch clauses.
     *   Category: Statements
     *   Type: Exception Handling
     *   Operands:
     *   Stack: v =>
     */ \
    macro(JSOP_THROWING,      151,"throwing", NULL,       1,  1,  0,  JOF_BYTE) \
    \
    /*
     * Pops the top of stack value as 'rval', sets the return value in stack
     * frame as 'rval'.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: rval =>
     */ \
    macro(JSOP_SETRVAL,       152,"setrval",    NULL,       1,  1,  0,  JOF_BYTE) \
    /*
     * Stops interpretation and returns value set by JSOP_SETRVAL. When not set,
     * returns 'undefined'.
     *
     * Also emitted at end of script so interpreter don't need to check if
     * opcode is still in script range.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_RETRVAL,       153,"retrval",    NULL,       1,  0,  0,  JOF_BYTE) \
    \
    /*
     * Looks up name on global environment and pushes its value onto the stack,
     * unless the script has a non-syntactic global scope, in which case it
     * acts just like JSOP_NAME.
     *
     * Free variable references that must either be found on the global or a
     * ReferenceError.
     *   Category: Variables and Scopes
     *   Type: Free Variables
     *   Operands: uint32_t nameIndex
     *   Stack: => val
     */ \
    macro(JSOP_GETGNAME,      154,"getgname",  NULL,       5,  0,  1, JOF_ATOM|JOF_NAME|JOF_TYPESET|JOF_GNAME) \
    /*
     * Pops the top two values on the stack as 'val' and 'env', sets property
     * of 'env' as 'val' and pushes 'val' back on the stack.
     *
     * 'env' should be the global lexical environment unless the script has a
     * non-syntactic global scope, in which case acts like JSOP_SETNAME.
     *   Category: Variables and Scopes
     *   Type: Free Variables
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    macro(JSOP_SETGNAME,      155,"setgname",  NULL,       5,  2,  1, JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_DETECTING|JOF_GNAME|JOF_CHECKSLOPPY) \
    \
    /*
     * Pops the top two values on the stack as 'val' and 'env', sets property
     * of 'env' as 'val' and pushes 'val' back on the stack. Throws a
     * TypeError if the set fails, per strict mode semantics.
     *
     * 'env' should be the global lexical environment unless the script has a
     * non-syntactic global scope, in which case acts like JSOP_STRICTSETNAME.
     *   Category: Variables and Scopes
     *   Type: Free Variables
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    macro(JSOP_STRICTSETGNAME, 156, "strict-setgname",  NULL,       5,  2,  1, JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_DETECTING|JOF_GNAME|JOF_CHECKSTRICT) \
    /*
     * Pushes the implicit 'this' value for calls to the associated name onto
     * the stack; only used when the implicit this might be derived from a
     * non-syntactic scope (instead of the global itself).
     *
     * Note that code evaluated via the Debugger API uses DebugEnvironmentProxy
     * objects on its scope chain, which are non-syntactic environments that
     * refer to syntactic environments. As a result, the binding we want may be
     * held by a syntactic environments such as CallObject or
     * VarEnvrionmentObject.
     *
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands: uint32_t nameIndex
     *   Stack: => this
     */                                                                 \
    macro(JSOP_GIMPLICITTHIS, 157, "gimplicitthis", "",      5,  0,  1,  JOF_ATOM) \
    /*
     * LIKE JSOP_SETELEM, but takes receiver on the stack, and the propval is
     * evaluated before the base.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: propval, receiver, obj, val => val
     */ \
    macro(JSOP_SETELEM_SUPER,   158, "setelem-super", NULL, 1,  4,  1, JOF_BYTE |JOF_ELEM|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSLOPPY) \
    /*
     * LIKE JSOP_STRICTSETELEM, but takes receiver on the stack, and the
     * propval is evaluated before the base.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: propval, receiver, obj, val => val
     */ \
    macro(JSOP_STRICTSETELEM_SUPER, 159, "strict-setelem-super", NULL, 1,  4, 1, JOF_BYTE |JOF_ELEM|JOF_PROPSET|JOF_DETECTING|JOF_CHECKSTRICT) \
    \
    /*
     * Pushes a regular expression literal onto the stack.
     * It requires special "clone on exec" handling.
     *   Category: Literals
     *   Type: RegExp
     *   Operands: uint32_t regexpIndex
     *   Stack: => regexp
     */ \
    macro(JSOP_REGEXP,        160,"regexp",   NULL,       5,  0,  1, JOF_REGEXP) \
    \
    /*
     * Initializes an uninitialized global lexical binding with the top of
     * stack value.
     *   Category: Variables and Scopes
     *   Type: Free Variables
     *   Operands: uint32_t nameIndex
     *   Stack: val => val
     */ \
    macro(JSOP_INITGLEXICAL,  161,"initglexical", NULL,   5,  1,  1,  JOF_ATOM|JOF_NAME|JOF_PROPINIT|JOF_GNAME) \
    \
    /* Defines the new mutable binding on global lexical environment.
     *
     * Throws if a binding with the same name already exists on the
     * environment, or if a var binding with the same name exists on the
     * global.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: =>
     */ \
    macro(JSOP_DEFLET,        162,"deflet",     NULL,     5,  0,  0,  JOF_ATOM) \
    /*
     * Throw if the value on the stack is not coerscible to an object (is |null| or |undefined|).
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: val => val
     */ \
    macro(JSOP_CHECKOBJCOERCIBLE, 163, "checkobjcoercible", NULL, 1,  1,  1, JOF_BYTE) \
    /*
     * Find the function to invoke with |super()| on the environment chain.
     *
     *   Category: Variables and Scopes
     *   Type: Super
     *   Operands:
     *   Stack: => superFun
     */ \
    macro(JSOP_SUPERFUN,      164,"superfun",   NULL,     1,  0,  1,  JOF_BYTE) \
    /*
     * Behaves exactly like JSOP_NEW, but allows JITs to distinguish the two cases.
     *
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1], newTarget => rval
     *   nuses: (argc+3)
     */ \
    macro(JSOP_SUPERCALL,     165,"supercall",  NULL,     3, -1,  1,  JOF_UINT16|JOF_INVOKE|JOF_TYPESET) \
    /*
     * spreadcall variant of JSOP_SUPERCALL.
     *
     * Behaves exactly like JSOP_SPREADNEW.
     *
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: callee, this, args, newTarget => rval
     */ \
    macro(JSOP_SPREADSUPERCALL, 166, "spreadsupercall", NULL, 1,  4,  1, JOF_BYTE|JOF_INVOKE|JOF_TYPESET) \
    /*
     * Push a default constructor for a base class literal.
     *
     *   Category: Literals
     *   Type: Class
     *   Operands: atom className
     *   Stack: => constructor
     */ \
    macro(JSOP_CLASSCONSTRUCTOR, 167,"classconstructor", NULL, 5,  0,  1,  JOF_ATOM) \
    /*
     * Push a default constructor for a derived class literal.
     *
     *   Category: Literals
     *   Type: Class
     *   Operands: atom className
     *   Stack: proto => constructor
     */ \
    macro(JSOP_DERIVEDCONSTRUCTOR, 168,"derivedconstructor", NULL, 5,  1,  1,  JOF_ATOM) \
    /*
     * Throws a runtime TypeError for invalid assignment to 'const'. The
     * localno is used for better error messages.
     *
     *   Category: Variables and Scopes
     *   Type: Local Variables
     *   Operands: uint24_t localno
     *   Stack: v => v
     */ \
    macro(JSOP_THROWSETCONST,        169, "throwsetconst",        NULL, 4,  1,  1, JOF_LOCAL|JOF_NAME|JOF_DETECTING) \
    /*
     * Throws a runtime TypeError for invalid assignment to 'const'. The
     * environment coordinate is used for better error messages.
     *
     *   Category: Variables and Scopes
     *   Type: Aliased Variables
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: v => v
     */ \
    macro(JSOP_THROWSETALIASEDCONST, 170, "throwsetaliasedconst", NULL, 5,  1,  1, JOF_ENVCOORD|JOF_NAME|JOF_DETECTING) \
    /*
     * Initialize a non-enumerable getter in an object literal.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines
     * getter of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITHIDDENPROP_GETTER,  171, "inithiddenprop_getter",   NULL, 5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a non-enumerable setter in an object literal.
     *
     * Pops the top two values on the stack as 'val' and 'obj', defines
     * setter of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    macro(JSOP_INITHIDDENPROP_SETTER,  172, "inithiddenprop_setter",   NULL, 5,  2,  1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a non-enumerable numeric getter in an object literal like
     * '{get 2() {}}'.
     *
     * Pops the top three values on the stack as 'val', 'id' and 'obj', defines
     * 'id' getter of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    macro(JSOP_INITHIDDENELEM_GETTER,  173, "inithiddenelem_getter",   NULL, 1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a non-enumerable numeric setter in an object literal like
     * '{set 2(v) {}}'.
     *
     * Pops the top three values on the stack as 'val', 'id' and 'obj', defines
     * 'id' setter of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    macro(JSOP_INITHIDDENELEM_SETTER, 174, "inithiddenelem_setter",   NULL, 1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Initialize a non-enumerable numeric property in an object literal, like '{1: x}'.
     *
     * Pops the top three values on the stack as 'val', 'id' and 'obj', defines
     * 'id' property of 'obj' as 'val', pushes 'obj' onto the stack.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    macro(JSOP_INITHIDDENELEM,  175, "inithiddenelem",   NULL,         1,  3,  1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_DETECTING) \
    /*
     * Gets the value of a module import by name and pushes it onto the stack.
     *   Category: Variables and Scopes
     *   Type: Variables
     *   Operands: uint32_t nameIndex
     *   Stack: => val
     */ \
    macro(JSOP_GETIMPORT,     176,"getimport",  NULL,     5,  0,  1,  JOF_ATOM|JOF_NAME|JOF_TYPESET) \
    /*
     * Examines the top stack value, asserting that it's either a self-hosted
     * function or a self-hosted intrinsic. This opcode does nothing in a
     * non-debug build.
     *   Category: Other
     *   Operands:
     *   Stack: checkVal => checkVal
     */ \
    macro(JSOP_DEBUGCHECKSELFHOSTED, 177,"debug-checkselfhosted",  NULL, 1,  1,  1,  JOF_BYTE) \
    /*
     * Pops the top stack value, pushes the value and a boolean value that
     * indicates whether the spread operation for the value can be optimized
     * in spread call.
     *   Category: Statements
     *   Type: Function
     *   Operands:
     *   Stack: arr => arr, optimized
     */ \
    macro(JSOP_OPTIMIZE_SPREADCALL,178,"optimize-spreadcall", NULL, 1,  1,  2,  JOF_BYTE) \
    /*
     * Throws a runtime TypeError for invalid assignment to the callee in a
     * named lambda, which is always a 'const' binding. This is a different
     * bytecode than JSOP_SETCONST because the named lambda callee, if not
     * closed over, does not have a frame slot to look up the name with for
     * the error message.
     *
     *   Category: Variables and Scopes
     *   Type: Local Variables
     *   Operands:
     *   Stack: v => v
     */ \
    macro(JSOP_THROWSETCALLEE,     179, "throwsetcallee",        NULL, 1,  1,  1, JOF_BYTE) \
    /*
     * Pushes a var environment onto the env chain.
     *   Category: Variables and Scopes
     *   Type: Var Scope
     *   Operands: uint32_t scopeIndex
     *   Stack: =>
     */ \
    macro(JSOP_PUSHVARENV,         180, "pushvarenv",           NULL, 5,  0,  0,  JOF_SCOPE) \
    /*
     * Pops a var environment from the env chain.
     *   Category: Variables and Scopes
     *   Type: Var Scope
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_POPVARENV,          181, "popvarenv",            NULL,  1,  0,  0,  JOF_BYTE) \
    /*
     * Pops the top two values on the stack as 'name' and 'fun', defines the
     * name of 'fun' to 'name' with prefix if any, and pushes 'fun' back onto
     * the stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint8_t prefixKind
     *   Stack: fun, name => fun
     */ \
    macro(JSOP_SETFUNNAME,    182,"setfunname", NULL,     2,  2,  1,  JOF_UINT8) \
    /*
     * Moves the top of the stack value under the nth element of the stack.
     *   Category: Operators
     *   Type: Stack Operations
     *   Operands: uint8_t n
     *   Stack: v[n], v[n-1], ..., v[1], v[0] => v[0], v[n], v[n-1], ..., v[1]
     */ \
    macro(JSOP_UNPICK,        183,"unpick",     NULL,     2,  0,  0,  JOF_UINT8) \
    /*
     * Pops the top of stack value, pushes property of it onto the stack.
     *
     * Like JSOP_GETPROP but for call context.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: obj => obj[name]
     */ \
    macro(JSOP_CALLPROP,      184,"callprop",   NULL,     5,  1,  1, JOF_ATOM|JOF_PROP|JOF_TYPESET) \
    /*
     * Determines the 'this' value for current function frame and pushes it onto
     * the stack. Emitted in the prologue of functions with a this-binding.
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands:
     *   Stack: => this
     */ \
    macro(JSOP_FUNCTIONTHIS,  185,"functionthis",NULL,    1,  0,  1,  JOF_BYTE) \
    /*
     * Pushes 'this' value for current stack frame onto the stack. Emitted when
     * 'this' refers to the global 'this'.
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands:
     *   Stack: => this
     */ \
    macro(JSOP_GLOBALTHIS,    186,"globalthis", NULL,     1,  0,  1,  JOF_BYTE) \
    /*
     * Pushes a boolean indicating whether the top of the stack is
     * MagicValue(JS_GENERATOR_CLOSING).
     *
     *   Category: Statements
     *   Type: For-In Statement
     *   Operands:
     *   Stack: val => val, res
     */ \
    macro(JSOP_ISGENCLOSING,  187, "isgenclosing",   NULL,         1,  1,  2,  JOF_BYTE) \
    /*
     * Pushes unsigned 24-bit int immediate integer operand onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: uint24_t val
     *   Stack: => val
     */ \
    macro(JSOP_UINT24,        188,"uint24",     NULL,     4,  0,  1, JOF_UINT24) \
    /*
     * Throw if the value on top of the stack is the TDZ MagicValue. Used in
     * derived class constructors.
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands:
     *   Stack: this => this
     */ \
    macro(JSOP_CHECKTHIS,     189,"checkthis",   NULL,    1,  1,  1,  JOF_BYTE) \
    /*
     * Check if a derived class constructor has a valid return value and 'this'
     * value before it returns. If the return value is not an object, stores
     * the 'this' value to the return value slot.
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands:
     *   Stack: this =>
     */ \
    macro(JSOP_CHECKRETURN,   190,"checkreturn", NULL,    1,  1,  0,  JOF_BYTE) \
    /*
     * Throw an exception if the value on top of the stack is not the TDZ
     * MagicValue. Used in derived class constructors.
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands:
     *   Stack: this => this
     */ \
    macro(JSOP_CHECKTHISREINIT,191,"checkthisreinit",NULL,1,  1,  1,  JOF_BYTE) \
    /*
     * Pops the top of stack value as 'unwrapped', converts it to async
     * generator 'wrapped', and pushes 'wrapped' back on the stack.
     *   Category: Statements
     *   Type: Generator
     *   Operands:
     *   Stack: unwrapped => wrapped
     */ \
    macro(JSOP_TOASYNCGEN,    192, "toasyncgen", NULL,    1,  1,  1, JOF_BYTE) \
    \
    /*
     * Pops the top two values on the stack as 'propval' and 'obj', pushes
     * 'propval' property of 'obj' onto the stack.
     *
     * Like JSOP_GETELEM but for call context.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, propval => obj[propval]
     */ \
    macro(JSOP_CALLELEM,      193, "callelem",   NULL,    1,  2,  1, JOF_BYTE |JOF_ELEM|JOF_TYPESET|JOF_LEFTASSOC) \
    \
    /*
     * '__proto__: v' inside an object initializer.
     *
     * Pops the top two values on the stack as 'newProto' and 'obj', sets
     * prototype of 'obj' as 'newProto', pushes 'true' onto the stack if
     * succeeded, 'false' if not.
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: obj, newProto => succeeded
     */ \
    macro(JSOP_MUTATEPROTO,   194, "mutateproto",NULL,    1,  2,  1, JOF_BYTE) \
    \
    /*
     * Pops an environment, gets the value of a bound name on it. If the name
     * is not bound to the environment, throw a ReferenceError. Used in
     * conjunction with BINDNAME.
     *   Category: Literals
     *   Type: Object
     *   Operands: uint32_t nameIndex
     *   Stack: env => v
     */ \
    macro(JSOP_GETBOUNDNAME,  195,"getboundname",NULL,    5,  1,  1, JOF_ATOM|JOF_NAME|JOF_TYPESET) \
    \
    /*
     * Pops the top stack value as 'val' and pushes 'typeof val'.  Note that
     * this opcode isn't used when, in the original source code, 'val' is a
     * name -- see 'JSOP_TYPEOF' for that.
     * (This is because 'typeof undefinedName === "undefined"'.)
     *   Category: Operators
     *   Type: Special Operators
     *   Operands:
     *   Stack: val => (typeof val)
     */ \
    macro(JSOP_TYPEOFEXPR,    196,"typeofexpr",  NULL,    1,  1,  1, JOF_BYTE|JOF_DETECTING) \
    \
    /* Lexical environment support. */ \
    /*
     * Replaces the current block on the env chain with a fresh block
     * that copies all the bindings in the bock.  This operation implements the
     * behavior of inducing a fresh lexical environment for every iteration of a
     * for(let ...; ...; ...) loop, if any declarations induced by such a loop
     * are captured within the loop.
     *   Category: Variables and Scopes
     *   Type: Block-local Scope
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_FRESHENLEXICALENV,197,"freshenlexicalenv", NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Recreates the current block on the env chain with a fresh block
     * with uninitialized bindings.  This operation implements the behavior of
     * inducing a fresh lexical environment for every iteration of a for-in/of loop
     * whose loop-head has a (captured) lexical declaration.
     *   Category: Variables and Scopes
     *   Type: Block-local Scope
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_RECREATELEXICALENV,198,"recreatelexicalenv",NULL,1,0,0,JOF_BYTE) \
    /*
     * Pushes lexical environment onto the env chain.
     *   Category: Variables and Scopes
     *   Type: Block-local Scope
     *   Operands: uint32_t scopeIndex
     *   Stack: =>
     */ \
    macro(JSOP_PUSHLEXICALENV,199,"pushlexicalenv", NULL, 5,  0,  0,  JOF_SCOPE) \
    /*
     * Pops lexical environment from the env chain.
     *   Category: Variables and Scopes
     *   Type: Block-local Scope
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_POPLEXICALENV, 200,"poplexicalenv", NULL,  1,  0,  0,  JOF_BYTE) \
    /*
     * The opcode to assist the debugger.
     *   Category: Statements
     *   Type: Debugger
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_DEBUGLEAVELEXICALENV, 201,"debugleavelexicalenv", NULL, 1,  0,  0,  JOF_BYTE) \
    \
    /*
     * Pops the generator from the top of the stack, suspends it and stops
     * interpretation.
     *   Category: Statements
     *   Type: Generator
     *   Operands: uint24_t yieldAndAwaitIndex
     *   Stack: generator => generator
     */ \
    macro(JSOP_INITIALYIELD,  202,"initialyield", NULL,   4,  1,  1,  JOF_UINT24) \
    /*
     * Pops the generator and the return value 'rval1', stops interpretation and
     * returns 'rval1'. Pushes sent value from 'send()' onto the stack.
     *   Category: Statements
     *   Type: Generator
     *   Operands: uint24_t yieldAndAwaitIndex
     *   Stack: rval1, gen => rval2
     */ \
    macro(JSOP_YIELD,         203,"yield",       NULL,    4,  2,  1,  JOF_UINT24) \
    /*
     * Pops the generator and suspends and closes it. Yields the value in the
     * frame's return value slot.
     *   Category: Statements
     *   Type: Generator
     *   Operands:
     *   Stack: gen =>
     */ \
    macro(JSOP_FINALYIELDRVAL,204,"finalyieldrval",NULL,  1,  1,  0,  JOF_BYTE) \
    /*
     * Pops the generator and argument from the stack, pushes a new generator
     * frame and resumes execution of it. Pushes the return value after the
     * generator yields.
     *   Category: Statements
     *   Type: Generator
     *   Operands: resume kind (GeneratorObject::ResumeKind)
     *   Stack: gen, val => rval
     */ \
    macro(JSOP_RESUME,        205,"resume",      NULL,    3,  2,  1,  JOF_UINT8|JOF_INVOKE) \
    \
    macro(JSOP_UNUSED206,     206,"unused206",   NULL,    1,  0,  0,  JOF_BYTE) \
    \
    /*
     * No-op bytecode only emitted in some self-hosted functions. Not handled by
     * the JITs so the script always runs in the interpreter.
     *
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_FORCEINTERPRETER, 207, "forceinterpreter", NULL,  1,  0,  0,  JOF_BYTE) \
    \
    /*
     * Bytecode emitted after 'yield' expressions to help the Debugger
     * fix up the frame in the JITs. No-op in the interpreter.
     *
     *   Category: Operators
     *   Type: Debugger
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_DEBUGAFTERYIELD,  208, "debugafteryield",  NULL,  1,  0,  0,  JOF_BYTE) \
    /*
     * Pops the generator and the return value 'promise', stops interpretation
     * and returns 'promise'. Pushes resolved value onto the stack.
     *   Category: Statements
     *   Type: Generator
     *   Operands: uint24_t yieldAndAwaitIndex
     *   Stack: promise, gen => resolved
     */ \
    macro(JSOP_AWAIT,         209, "await",        NULL,  4,  2,  1,  JOF_UINT24) \
    /*
     * Pops the iterator and its next method from the top of the stack, and
     * create async iterator from it and push the async iterator back onto the
     * stack.
     *   Category: Statements
     *   Type: Generator
     *   Operands:
     *   Stack: iter, next => asynciter
     */ \
    macro(JSOP_TOASYNCITER,   210, "toasynciter",  NULL,  1,  2,  1,  JOF_BYTE) \
    /*
     * Pops the top two values 'id' and 'obj' from the stack, then pushes
     * obj.hasOwnProperty(id)
     *
     * Note that 'obj' is the top value.
     *   Category: Other
     *   Type:
     *   Operands:
     *   Stack: id, obj => (obj.hasOwnProperty(id))
     */ \
    macro(JSOP_HASOWN,        211, "hasown",     NULL,    1,  2,  1, JOF_BYTE) \
    /*
     * Initializes generator frame, creates a generator and pushes it on the
     * stack.
     *   Category: Statements
     *   Type: Generator
     *   Operands:
     *   Stack: => generator
     */ \
    macro(JSOP_GENERATOR,     212,"generator",   NULL,    1,  0,  1,  JOF_BYTE) \
    /*
     * Pushes the nearest 'var' environment.
     *
     *   Category: Variables and Scopes
     *   Type: Free Variables
     *   Operands:
     *   Stack: => env
     */ \
    macro(JSOP_BINDVAR,       213, "bindvar",      NULL,  1,  0,  1,  JOF_BYTE) \
    /*
     * Pushes the global environment onto the stack if the script doesn't have a
     * non-syntactic global scope.  Otherwise will act like JSOP_BINDNAME.
     *
     * 'nameIndex' is only used when acting like JSOP_BINDNAME.
     *   Category: Variables and Scopes
     *   Type: Free Variables
     *   Operands: uint32_t nameIndex
     *   Stack: => global
     */ \
    macro(JSOP_BINDGNAME,     214, "bindgname",    NULL,  5,  0,  1,  JOF_ATOM|JOF_NAME|JOF_GNAME) \
    \
    /*
     * Pushes 8-bit int immediate integer operand onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: int8_t val
     *   Stack: => val
     */ \
    macro(JSOP_INT8,          215, "int8",         NULL,  2,  0,  1, JOF_INT8) \
    /*
     * Pushes 32-bit int immediate integer operand onto the stack.
     *   Category: Literals
     *   Type: Constants
     *   Operands: int32_t val
     *   Stack: => val
     */ \
    macro(JSOP_INT32,         216, "int32",        NULL,  5,  0,  1, JOF_INT32) \
    \
    /*
     * Pops the top of stack value, pushes the 'length' property of it onto the
     * stack.
     *   Category: Literals
     *   Type: Array
     *   Operands: uint32_t nameIndex
     *   Stack: obj => obj['length']
     */ \
    macro(JSOP_LENGTH,        217, "length",       NULL,  5,  1,  1, JOF_ATOM|JOF_PROP|JOF_TYPESET) \
    \
    /*
     * Pushes a JS_ELEMENTS_HOLE value onto the stack, representing an omitted
     * property in an array literal (e.g. property 0 in the array '[, 1]').
     *
     * This opcode is used with the JSOP_NEWARRAY opcode.
     *   Category: Literals
     *   Type: Array
     *   Operands:
     *   Stack: => hole
     */ \
    macro(JSOP_HOLE,          218, "hole",         NULL,  1,  0,  1,  JOF_BYTE) \
    \
    /*
     * Checks that the top value on the stack is callable, and throws a
     * TypeError if not. The operand 'kind' is used only to generate an
     * appropriate error message.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint8_t kind
     *   Stack: obj => obj
     */ \
    macro(JSOP_CHECKISCALLABLE, 219, "checkiscallable", NULL, 2, 1, 1, JOF_UINT8) \
    \
    /*
     * No-op used by the exception unwinder to determine the correct
     * environment to unwind to when performing IteratorClose due to
     * destructuring.
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_TRY_DESTRUCTURING_ITERCLOSE, 220, "try-destructuring-iterclose", NULL, 1, 0, 0, JOF_BYTE) \
    \
    /*
     * Pushes the current global's builtin prototype for a given proto key
     *   Category: Literals
     *   Type: Constants
     *   Operands: uint8_t kind
     *   Stack: => %BuiltinPrototype%
     */ \
    macro(JSOP_BUILTINPROTO, 221, "builtinproto", NULL, 2,  0,  1,  JOF_UINT8) \
    \
    /*
     * NOP opcode to hint to IonBuilder that the value on top of the stack is
     * the (likely string) key in a for-in loop.
     *   Category: Other
     *   Operands:
     *   Stack: val => val
     */ \
    macro(JSOP_ITERNEXT,      222, "iternext",   NULL,  1,  1,  1,  JOF_BYTE) \
    macro(JSOP_UNUSED223,     223, "unused223",  NULL,  1,  0,  0,  JOF_BYTE) \
    \
    /*
     * Creates rest parameter array for current function call, and pushes it
     * onto the stack.
     *   Category: Variables and Scopes
     *   Type: Arguments
     *   Operands:
     *   Stack: => rest
     */ \
    macro(JSOP_REST,          224, "rest",         NULL,  1,  0,  1,  JOF_BYTE|JOF_TYPESET) \
    \
    /*
     * Replace the top-of-stack value propertyNameValue with
     * ToPropertyKey(propertyNameValue).
     *   Category: Literals
     *   Type: Object
     *   Operands:
     *   Stack: propertyNameValue => propertyKey
     */ \
    macro(JSOP_TOID,          225, "toid",         NULL,  1,  1,  1,  JOF_BYTE) \
    \
    /*
     * Pushes the implicit 'this' value for calls to the associated name onto
     * the stack.
     *   Category: Variables and Scopes
     *   Type: This
     *   Operands: uint32_t nameIndex
     *   Stack: => this
     */                                                                 \
    macro(JSOP_IMPLICITTHIS,  226, "implicitthis", "",    5,  0,  1,  JOF_ATOM) \
    \
    /*
     * This opcode is the target of the entry jump for some loop. The uint8
     * argument is a bitfield. The lower 7 bits of the argument indicate the
     * loop depth. This value starts at 1 and is just a hint: deeply nested
     * loops all have the same value.  The upper bit is set if Ion should be
     * able to OSR at this point, which is true unless there is non-loop state
     * on the stack.
     *   Category: Statements
     *   Type: Jumps
     *   Operands: uint8_t BITFIELD
     *   Stack: =>
     */ \
    macro(JSOP_LOOPENTRY,     227, "loopentry",    NULL,  2,  0,  0,  JOF_UINT8) \
    /*
     * Converts the value on the top of the stack to a String
     *   Category: Other
     *   Operands:
     *   Stack: val => ToString(val)
     */ \
    macro(JSOP_TOSTRING,    228, "tostring",       NULL,  1,  1,  1,  JOF_BYTE) \
    /*
     * No-op used by the decompiler to produce nicer error messages about
     * destructuring code.
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_NOP_DESTRUCTURING, 229, "nop-destructuring", NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * This opcode is a no-op and it indicates the location of a jump
     * instruction target. Some other opcodes act as jump targets, such as
     * LOOPENTRY, as well as all which are matched by BytecodeIsJumpTarget
     * function.
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    macro(JSOP_JUMPTARGET,  230, "jumptarget",     NULL,  1,  0,  0,  JOF_BYTE)\
    /*
     * Like JSOP_CALL, but tells the function that the return value is ignored.
     * stack.
     *   Category: Statements
     *   Type: Function
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     *   nuses: (argc+2)
     */ \
    macro(JSOP_CALL_IGNORES_RV, 231, "call-ignores-rv", NULL, 3, -1, 1, JOF_UINT16|JOF_INVOKE|JOF_TYPESET)

/*
 * In certain circumstances it may be useful to "pad out" the opcode space to
 * a power of two.  Use this macro to do so.
 */
#define FOR_EACH_TRAILING_UNUSED_OPCODE(macro) \
    macro(232) \
    macro(233) \
    macro(234) \
    macro(235) \
    macro(236) \
    macro(237) \
    macro(238) \
    macro(239) \
    macro(240) \
    macro(241) \
    macro(242) \
    macro(243) \
    macro(244) \
    macro(245) \
    macro(246) \
    macro(247) \
    macro(248) \
    macro(249) \
    macro(250) \
    macro(251) \
    macro(252) \
    macro(253) \
    macro(254) \
    macro(255)

namespace js {

// Sanity check that opcode values and trailing unused opcodes completely cover
// the [0, 256) range.  Avert your eyes!  You don't want to know how the
// sausage gets made.

#define VALUE_AND_VALUE_PLUS_ONE(op, val, ...) \
    val) && (val + 1 ==
#define TRAILING_VALUE_AND_VALUE_PLUS_ONE(val) \
    val) && (val + 1 ==
static_assert((0 ==
               FOR_EACH_OPCODE(VALUE_AND_VALUE_PLUS_ONE)
               FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_VALUE_AND_VALUE_PLUS_ONE)
               256),
              "opcode values and trailing unused opcode values monotonically "
              "increase from zero to 255");
#undef TRAILING_VALUE_AND_VALUE_PLUS_ONE
#undef VALUE_AND_VALUE_PLUS_ONE

// Define JSOP_*_LENGTH constants for all ops.
#define DEFINE_LENGTH_CONSTANT(op, val, name, image, len, ...) \
    constexpr size_t op##_LENGTH = len;
FOR_EACH_OPCODE(DEFINE_LENGTH_CONSTANT)
#undef DEFINE_LENGTH_CONSTANT

} // namespace js

#endif // vm_Opcodes_h
