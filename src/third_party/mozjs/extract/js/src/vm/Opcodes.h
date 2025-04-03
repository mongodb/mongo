/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sw=2 et tw=0 ft=c:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Opcodes_h
#define vm_Opcodes_h

#include <stddef.h>
#include <stdint.h>

#include "js/TypeDecls.h"

// clang-format off
/*
 * [SMDOC] Bytecode Definitions
 *
 * SpiderMonkey bytecode instructions.
 *
 * To use this header, define a macro of the form:
 *
 *     #define MACRO(op, op_snake, token, length, nuses, ndefs, format) ...
 *
 * Then `FOR_EACH_OPCODE(MACRO)` invokes `MACRO` for every opcode.
 *
 * Field        Description
 * -----        -----------
 * op           UpperCamelCase form of opcode id
 * op_snake     snake_case form of opcode id
 * token        Pretty-printer string, or null if ugly
 * length       Number of bytes including any immediate operands
 * nuses        Number of stack slots consumed by bytecode, -1 if variadic
 * ndefs        Number of stack slots produced by bytecode
 * format       JOF_ flags describing instruction operand layout, etc.
 *
 * For more about `format`, see the comments on the `JOF_` constants defined in
 * BytecodeUtil.h.
 *
 *
 * [SMDOC] Bytecode Invariants
 *
 * Creating scripts that do not follow the rules can lead to undefined
 * behavior. Bytecode has many consumers, not just the interpreter: JITs,
 * analyses, the debugger. That's why the rules below apply even to code that
 * can't be reached in ordinary execution (such as code after an infinite loop
 * or inside an `if (false)` block).
 *
 * The `code()` of a script must be a packed (not aligned) sequence of valid
 * instructions from start to end. Each instruction has a single byte opcode
 * followed by a number of operand bytes based on the opcode.
 *
 * ## Jump instructions
 *
 * Operands named `offset`, `forwardOffset`, or `defaultOffset` are jump
 * offsets, the distance in bytes from the start of the current instruction to
 * the start of another instruction in the same script. Operands named
 * `forwardOffset` or `defaultOffset` must be positive.
 *
 * Forward jumps must jump to a `JSOp::JumpTarget` instruction.  Backward jumps,
 * indicated by negative offsets, must jump to a `JSOp::LoopHead` instruction.
 * Jump offsets can't be zero.
 *
 * Needless to say, scripts must not contain overlapping instruction sequences
 * (in the sense of <https://en.wikipedia.org/wiki/Overlapping_gene>).
 *
 * A script's `trynotes` and `scopeNotes` impose further constraints. Each try
 * note and each scope note marks a region of the bytecode where some invariant
 * holds, or some cleanup behavior is needed--that there's a for-in iterator in
 * a particular stack slot, for instance, which must be closed on error. All
 * paths into the span must establish that invariant. In practice, this means
 * other code never jumps into the span: the only way in is to execute the
 * bytecode instruction that sets up the invariant (in our example,
 * `JSOp::Iter`).
 *
 * If a script's `trynotes` (see "Try Notes" in JSScript.h) contain a
 * `JSTRY_CATCH` or `JSTRY_FINALLY` span, there must be a `JSOp::Try`
 * instruction immediately before the span and a `JSOp::JumpTarget immediately
 * after it. Instructions must not jump to this `JSOp::JumpTarget`. (The VM puts
 * us there on exception.) Furthermore, the instruction sequence immediately
 * following a `JSTRY_CATCH` span must read `JumpTarget; Exception` or, in
 * non-function scripts, `JumpTarget; Undefined; SetRval; Exception`. (These
 * instructions run with an exception pending; other instructions aren't
 * designed to handle that.)
 *
 * Unreachable instructions are allowed, but they have to follow all the rules.
 *
 * Control must not reach the end of a script. (Currently, the last instruction
 * is always JSOp::RetRval.)
 *
 * ## Other operands
 *
 * Operands named `nameIndex` or `atomIndex` (which appear on instructions that
 * have `JOF_ATOM` in the `format` field) must be valid indexes into
 * `script->atoms()`.
 *
 * Operands named `argc` (`JOF_ARGC`) are argument counts for call
 * instructions. `argc` must be small enough that the instruction's nuses is <=
 * the current stack depth (see "Stack depth" below).
 *
 * Operands named `argno` (`JOF_QARG`) refer to an argument of the current
 * function. `argno` must be in the range `0..script->function()->nargs()`.
 * Instructions with these operands must appear only in function scripts.
 *
 * Operands named `localno` (`JOF_LOCAL`) refer to a local variable stored in
 * the stack frame. `localno` must be in the range `0..script->nfixed()`.
 *
 * Operands named `resumeIndex` (`JOF_RESUMEINDEX`) refer to a resume point in
 * the current script. `resumeIndex` must be a valid index into
 * `script->resumeOffsets()`.
 *
 * Operands named `hops` and `slot` (`JOF_ENVCOORD`) refer a slot in an
 * `EnvironmentObject`. At run time, they must point to a fixed slot in an
 * object on the current environment chain. See `EnvironmentCoordinates`.
 *
 * Operands with the following names must be valid indexes into
 * `script->gcthings()`, and the pointer in the vector must point to the right
 * type of thing:
 *
 * -   `objectIndex` (`JOF_OBJECT`): `PlainObject*` or `ArrayObject*`
 * -   `baseobjIndex` (`JOF_OBJECT`): `PlainObject*`
 * -   `funcIndex` (`JOF_OBJECT`): `JSFunction*`
 * -   `regexpIndex` (`JOF_REGEXP`): `RegExpObject*`
 * -   `shapeIndex` (`JOF_SHAPE`): `Shape*`
 * -   `scopeIndex` (`JOF_SCOPE`): `Scope*`
 * -   `lexicalScopeIndex` (`JOF_SCOPE`): `LexicalScope*`
 * -   `classBodyScopeIndex` (`JOF_SCOPE`): `ClassBodyScope*`
 * -   `withScopeIndex` (`JOF_SCOPE`): `WithScope*`
 * -   `bigIntIndex` (`JOF_BIGINT`): `BigInt*`
 *
 * Operands named `icIndex` (`JOF_ICINDEX`) must be exactly the number of
 * preceding instructions in the script that have the JOF_IC flag.
 * (Rationale: Each JOF_IC instruction has a unique entry in
 * `script->jitScript()->icEntries()`.  At run time, in the bytecode
 * interpreter, we have to find that entry. We could store the IC index as an
 * operand to each JOF_IC instruction, but it's more memory-efficient to use a
 * counter and reset the counter to `icIndex` after each jump.)
 *
 * ## Stack depth
 *
 * Each instruction has a compile-time stack depth, the number of values on the
 * interpreter stack just before executing the instruction. It isn't explicitly
 * present in the bytecode itself, but (for reachable instructions, anyway)
 * it's a function of the bytecode.
 *
 * -   The first instruction has stack depth 0.
 *
 * -   Each successor of an instruction X has a stack depth equal to
 *
 *         X's stack depth - `js::StackUses(X)` + `js::StackDefs(X)`
 *
 *     except for `JSOp::Case` (below).
 *
 *     X's "successors" are: the next instruction in the script, if
 *     `js::FlowsIntoNext(op)` is true for X's opcode; one or more
 *     `JSOp::JumpTarget`s elsewhere, if X is a forward jump or
 *     `JSOp::TableSwitch`; and/or a `JSOp::LoopHead` if it's a backward jump.
 *
 * -   `JSOp::Case` is a special case because its stack behavior is eccentric.
 *     The formula above is correct for the next instruction. The jump target
 *     has a stack depth that is 1 less.
 *
 * -   The `JSOp::JumpTarget` instruction immediately following a `JSTRY_CATCH`
 *     or `JSTRY_FINALLY` span has the same stack depth as the `JSOp::Try`
 *     instruction that precedes the span.
 *
 *     Every instruction covered by the `JSTRY_CATCH` or `JSTRY_FINALLY` span
 *     must have a stack depth >= that value, so that error recovery is
 *     guaranteed to find enough values on the stack to resume there.
 *
 * -   `script->nslots() - script->nfixed()` must be >= the maximum stack
 *     depth of any instruction in `script`.  (The stack frame must be big
 *     enough to run the code.)
 *
 * `BytecodeParser::parse()` computes stack depths for every reachable
 * instruction in a script.
 *
 * ## Scopes and environments
 *
 * As with stack depth, each instruction has a static scope, which is a
 * compile-time characterization of the eventual run-time environment chain
 * when that instruction executes. Just as every instruction has a stack budget
 * (nuses/ndefs), every instruction either pushes a scope, pops a scope, or
 * neither. The same successor relation applies as above.
 *
 * Every scope used in a script is stored in the `JSScript::gcthings()` vector.
 * They can be accessed using `getScope(index)` if you know what `index` to
 * pass.
 *
 * The scope of every instruction (that's reachable via the successor relation)
 * is given in two independent ways: by the bytecode itself and by the scope
 * notes. The two sources must agree.
 *
 * ## Further rules
 *
 * All reachable instructions must be reachable without taking any backward
 * edges.
 *
 * Instructions with the `JOF_CHECKSLOPPY` flag must not be used in strict mode
 * code. `JOF_CHECKSTRICT` instructions must not be used in nonstrict code.
 *
 * Many instructions have their own additional rules. These are documented on
 * the various opcodes below (look for the word "must").
 */
// clang-format on

// clang-format off
/*
 * SpiderMonkey bytecode categorization (as used in generated documentation):
 *
 * [Index]
 *   [Constants]
 *   [Compound primitives]
 *     Record literals
 *     Tuple literals
 *   [Expressions]
 *     Unary operators
 *     Binary operators
 *     Conversions
 *     Other expressions
 *   [Objects]
 *     Creating objects
 *     Defining properties
 *     Accessing properties
 *     Super
 *     Enumeration
 *     Iteration
 *     SetPrototype
 *     Array literals
 *     RegExp literals
 *     Built-in objects
 *   [Functions]
 *     Creating functions
 *     Creating constructors
 *     Calls
 *     Generators and async functions
 *   [Control flow]
 *     Jump targets
 *     Jumps
 *     Return
 *     Exceptions
 *   [Variables and scopes]
 *     Initialization
 *     Looking up bindings
 *     Getting binding values
 *     Setting binding values
 *     Entering and leaving environments
 *     Creating and deleting bindings
 *     Function environment setup
 *   [Stack operations]
 *   [Other]
 */
// clang-format on

// clang-format off
#define FOR_EACH_OPCODE(MACRO) \
    /*
     * Push `undefined`.
     *
     *   Category: Constants
     *   Operands:
     *   Stack: => undefined
     */ \
    MACRO(Undefined, undefined, "", 1, 0, 1, JOF_BYTE) \
    /*
     * Push `null`.
     *
     *   Category: Constants
     *   Operands:
     *   Stack: => null
     */ \
    MACRO(Null, null, "null", 1, 0, 1, JOF_BYTE) \
    /*
     * Push a boolean constant.
     *
     *   Category: Constants
     *   Operands:
     *   Stack: => true/false
     */ \
    MACRO(False, false_, "false", 1, 0, 1, JOF_BYTE) \
    MACRO(True, true_, "true", 1, 0, 1, JOF_BYTE) \
    /*
     * Push the `int32_t` immediate operand as an `Int32Value`.
     *
     * `JSOp::Zero`, `JSOp::One`, `JSOp::Int8`, `JSOp::Uint16`, and `JSOp::Uint24`
     * are all compact encodings for `JSOp::Int32`.
     *
     *   Category: Constants
     *   Operands: int32_t val
     *   Stack: => val
     */ \
    MACRO(Int32, int32, NULL, 5, 0, 1, JOF_INT32) \
    /*
     * Push the number `0`.
     *
     *   Category: Constants
     *   Operands:
     *   Stack: => 0
     */ \
    MACRO(Zero, zero, "0", 1, 0, 1, JOF_BYTE) \
    /*
     * Push the number `1`.
     *
     *   Category: Constants
     *   Operands:
     *   Stack: => 1
     */ \
    MACRO(One, one, "1", 1, 0, 1, JOF_BYTE) \
    /*
     * Push the `int8_t` immediate operand as an `Int32Value`.
     *
     *   Category: Constants
     *   Operands: int8_t val
     *   Stack: => val
     */ \
    MACRO(Int8, int8, NULL, 2, 0, 1, JOF_INT8) \
    /*
     * Push the `uint16_t` immediate operand as an `Int32Value`.
     *
     *   Category: Constants
     *   Operands: uint16_t val
     *   Stack: => val
     */ \
    MACRO(Uint16, uint16, NULL, 3, 0, 1, JOF_UINT16) \
    /*
     * Push the `uint24_t` immediate operand as an `Int32Value`.
     *
     *   Category: Constants
     *   Operands: uint24_t val
     *   Stack: => val
     */ \
    MACRO(Uint24, uint24, NULL, 4, 0, 1, JOF_UINT24) \
    /*
     * Push the 64-bit floating-point immediate operand as a `DoubleValue`.
     *
     * If the operand is a NaN, it must be the canonical NaN (see
     * `JS::detail::CanonicalizeNaN`).
     *
     *   Category: Constants
     *   Operands: double val
     *   Stack: => val
     */ \
    MACRO(Double, double_, NULL, 9, 0, 1, JOF_DOUBLE) \
    /*
     * Push the BigInt constant `script->getBigInt(bigIntIndex)`.
     *
     *   Category: Constants
     *   Operands: uint32_t bigIntIndex
     *   Stack: => bigint
     */ \
    MACRO(BigInt, big_int, NULL, 5, 0, 1, JOF_BIGINT) \
    /*
     * Push the string constant `script->getAtom(atomIndex)`.
     *
     *   Category: Constants
     *   Operands: uint32_t atomIndex
     *   Stack: => string
     */ \
    MACRO(String, string, NULL, 5, 0, 1, JOF_STRING) \
    /*
     * Push a well-known symbol.
     *
     * `symbol` must be in range for `JS::SymbolCode`.
     *
     *   Category: Constants
     *   Operands: uint8_t symbol (the JS::SymbolCode of the symbol to use)
     *   Stack: => symbol
     */ \
    MACRO(Symbol, symbol, NULL, 2, 0, 1, JOF_UINT8) \
    /*
     * Pop the top value on the stack, discard it, and push `undefined`.
     *
     * Implements: [The `void` operator][1], step 3.
     *
     * [1]: https://tc39.es/ecma262/#sec-void-operator
     *
     *   Category: Expressions
     *   Type: Unary operators
     *   Operands:
     *   Stack: val => undefined
     */ \
    MACRO(Void, void_, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * [The `typeof` operator][1].
     *
     * Infallible. The result is always a string that depends on the [type][2]
     * of `val`.
     *
     * `JSOp::Typeof` and `JSOp::TypeofExpr` are the same except
     * that--amazingly--`JSOp::Typeof` affects the behavior of an immediately
     * *preceding* `JSOp::GetName` or `JSOp::GetGName` instruction! This is how
     * we implement [`typeof`][1] step 2, making `typeof nonExistingVariable`
     * return `"undefined"` instead of throwing a ReferenceError.
     *
     * In a global scope:
     *
     * -   `typeof x` compiles to `GetGName "x"; Typeof`.
     * -   `typeof (0, x)` compiles to `GetGName "x"; TypeofExpr`.
     *
     * Emitting the same bytecode for these two expressions would be a bug.
     * Per spec, the latter throws a ReferenceError if `x` doesn't exist.
     *
     * [1]: https://tc39.es/ecma262/#sec-typeof-operator
     * [2]: https://tc39.es/ecma262/#sec-ecmascript-language-types
     *
     *   Category: Expressions
     *   Type: Unary operators
     *   Operands:
     *   Stack: val => (typeof val)
     */ \
    MACRO(Typeof, typeof_, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    MACRO(TypeofExpr, typeof_expr, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The unary `+` operator][1].
     *
     * `+val` doesn't do any actual math. It just calls [ToNumber][2](val).
     *
     * The conversion can call `.toString()`/`.valueOf()` methods and can
     * throw. The result on success is always a Number. (Per spec, unary `-`
     * supports BigInts, but unary `+` does not.)
     *
     * [1]: https://tc39.es/ecma262/#sec-unary-plus-operator
     * [2]: https://tc39.es/ecma262/#sec-tonumber
     *
     *   Category: Expressions
     *   Type: Unary operators
     *   Operands:
     *   Stack: val => (+val)
     */ \
    MACRO(Pos, pos, "+ ", 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The unary `-` operator][1].
     *
     * Convert `val` to a numeric value, then push `-val`. The conversion can
     * call `.toString()`/`.valueOf()` methods and can throw. The result on
     * success is always numeric.
     *
     * [1]: https://tc39.es/ecma262/#sec-unary-minus-operator
     *
     *   Category: Expressions
     *   Type: Unary operators
     *   Operands:
     *   Stack: val => (-val)
     */ \
    MACRO(Neg, neg, "- ", 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The bitwise NOT operator][1] (`~`).
     *
     * `val` is converted to an integer, then bitwise negated. The conversion
     * can call `.toString()`/`.valueOf()` methods and can throw. The result on
     * success is always an Int32 or BigInt value.
     *
     * [1]: https://tc39.es/ecma262/#sec-bitwise-not-operator
     *
     *   Category: Expressions
     *   Type: Unary operators
     *   Operands:
     *   Stack: val => (~val)
     */ \
    MACRO(BitNot, bit_not, "~", 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The logical NOT operator][1] (`!`).
     *
     * `val` is first converted with [ToBoolean][2], then logically
     * negated. The result is always a boolean value. This does not call
     * user-defined methods and can't throw.
     *
     * [1]: https://tc39.es/ecma262/#sec-logical-not-operator
     * [2]: https://tc39.es/ecma262/#sec-toboolean
     *
     *   Category: Expressions
     *   Type: Unary operators
     *   Operands:
     *   Stack: val => (!val)
     */ \
    MACRO(Not, not_, "!", 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * [Binary bitwise operations][1] (`|`, `^`, `&`).
     *
     * The arguments are converted to integers first. The conversion can call
     * `.toString()`/`.valueOf()` methods and can throw. The result on success
     * is always an Int32 or BigInt Value.
     *
     * [1]: https://tc39.es/ecma262/#sec-binary-bitwise-operators
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    MACRO(BitOr, bit_or, "|",  1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(BitXor, bit_xor, "^", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(BitAnd, bit_and, "&", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * Loose equality operators (`==` and `!=`).
     *
     * Pop two values, compare them, and push the boolean result. The
     * comparison may perform conversions that call `.toString()`/`.valueOf()`
     * methods and can throw.
     *
     * Implements: [Abstract Equality Comparison][1].
     *
     * [1]: https://tc39.es/ecma262/#sec-abstract-equality-comparison
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    MACRO(Eq, eq, "==", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Ne, ne, "!=", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * Strict equality operators (`===` and `!==`).
     *
     * Pop two values, check whether they're equal, and push the boolean
     * result. This does not call user-defined methods and can't throw
     * (except possibly due to OOM while flattening a string).
     *
     * Implements: [Strict Equality Comparison][1].
     *
     * [1]: https://tc39.es/ecma262/#sec-strict-equality-comparison
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    MACRO(StrictEq, strict_eq, "===", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(StrictNe, strict_ne, "!==", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * Relative operators (`<`, `>`, `<=`, `>=`).
     *
     * Pop two values, compare them, and push the boolean result. The
     * comparison may perform conversions that call `.toString()`/`.valueOf()`
     * methods and can throw.
     *
     * Implements: [Relational Operators: Evaluation][1].
     *
     * [1]: https://tc39.es/ecma262/#sec-relational-operators-runtime-semantics-evaluation
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    MACRO(Lt, lt, "<",  1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Gt, gt, ">",  1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Le, le, "<=", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Ge, ge, ">=", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The `instanceof` operator][1].
     *
     * This throws a `TypeError` if `target` is not an object. It calls
     * `target[Symbol.hasInstance](value)` if the method exists. On success,
     * the result is always a boolean value.
     *
     * [1]: https://tc39.es/ecma262/#sec-instanceofoperator
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: value, target => (value instanceof target)
     */ \
    MACRO(Instanceof, instanceof, "instanceof", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The `in` operator][1].
     *
     * Push `true` if `obj` has a property with the key `id`. Otherwise push `false`.
     *
     * This throws a `TypeError` if `obj` is not an object. This can fire
     * proxy hooks and can throw. On success, the result is always a boolean
     * value.
     *
     * [1]: https://tc39.es/ecma262/#sec-relational-operators-runtime-semantics-evaluation
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: id, obj => (id in obj)
     */ \
    MACRO(In, in_, "in", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * [Bitwise shift operators][1] (`<<`, `>>`, `>>>`).
     *
     * Pop two values, convert them to integers, perform a bitwise shift, and
     * push the result.
     *
     * Conversion can call `.toString()`/`.valueOf()` methods and can throw.
     * The result on success is always an Int32 or BigInt Value.
     *
     * [1]: https://tc39.es/ecma262/#sec-bitwise-shift-operators
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    MACRO(Lsh, lsh, "<<", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Rsh, rsh, ">>", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Ursh, ursh, ">>>", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The binary `+` operator][1].
     *
     * Pop two values, convert them to primitive values, add them, and push the
     * result. If both values are numeric, add them; if either is a
     * string, do string concatenation instead.
     *
     * The conversion can call `.toString()`/`.valueOf()` methods and can throw.
     *
     * [1]: https://tc39.es/ecma262/#sec-addition-operator-plus-runtime-semantics-evaluation
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval + rval)
     */ \
    MACRO(Add, add, "+", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The binary `-` operator][1].
     *
     * Pop two values, convert them to numeric values, subtract the top value
     * from the other one, and push the result.
     *
     * The conversion can call `.toString()`/`.valueOf()` methods and can
     * throw. On success, the result is always numeric.
     *
     * [1]: https://tc39.es/ecma262/#sec-subtraction-operator-minus-runtime-semantics-evaluation
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval - rval)
     */ \
    MACRO(Sub, sub, "-", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * Add or subtract 1.
     *
     * `val` must already be a numeric value, such as the result of
     * `JSOp::ToNumeric`.
     *
     * Implements: [The `++` and `--` operators][1], step 3 of each algorithm.
     *
     * [1]: https://tc39.es/ecma262/#sec-postfix-increment-operator
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: val => (val +/- 1)
     */ \
    MACRO(Inc, inc, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    MACRO(Dec, dec, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The multiplicative operators][1] (`*`, `/`, `%`).
     *
     * Pop two values, convert them to numeric values, do math, and push the
     * result.
     *
     * The conversion can call `.toString()`/`.valueOf()` methods and can
     * throw. On success, the result is always numeric.
     *
     * [1]: https://tc39.es/ecma262/#sec-multiplicative-operators-runtime-semantics-evaluation
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval OP rval)
     */ \
    MACRO(Mul, mul, "*", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Div, div, "/", 1, 2, 1, JOF_BYTE|JOF_IC) \
    MACRO(Mod, mod, "%", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * [The exponentiation operator][1] (`**`).
     *
     * Pop two values, convert them to numeric values, do exponentiation, and
     * push the result. The top value is the exponent.
     *
     * The conversion can call `.toString()`/`.valueOf()` methods and can
     * throw. This throws a RangeError if both values are BigInts and the
     * exponent is negative.
     *
     * [1]: https://tc39.es/ecma262/#sec-exp-operator
     *
     *   Category: Expressions
     *   Type: Binary operators
     *   Operands:
     *   Stack: lval, rval => (lval ** rval)
     */ \
    MACRO(Pow, pow, "**", 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * Convert a value to a property key.
     *
     * Implements: [ToPropertyKey][1], except that if the result would be the
     * string representation of some integer in the range 0..2^31, we push the
     * corresponding Int32 value instead. This is because the spec insists that
     * array indices are strings, whereas for us they are integers.
     *
     * This is used for code like `++obj[index]`, which must do both a
     * `JSOp::GetElem` and a `JSOp::SetElem` with the same property key. Both
     * instructions would convert `index` to a property key for us, but the
     * spec says to convert it only once.
     *
     * The conversion can call `.toString()`/`.valueOf()` methods and can
     * throw.
     *
     * [1]: https://tc39.es/ecma262/#sec-topropertykey
     *
     *   Category: Expressions
     *   Type: Conversions
     *   Operands:
     *   Stack: propertyNameValue => propertyKey
     */ \
    MACRO(ToPropertyKey, to_property_key, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * Convert a value to a numeric value (a Number or BigInt).
     *
     * Implements: [ToNumeric][1](val).
     *
     * Note: This is used to implement [`++` and `--`][2]. Surprisingly, it's
     * not possible to get the right behavior using `JSOp::Add` and `JSOp::Sub`
     * alone. For one thing, `JSOp::Add` sometimes does string concatenation,
     * while `++` always does numeric addition. More fundamentally, the result
     * of evaluating `x--` is ToNumeric(old value of `x`), a value that the
     * sequence `GetLocal "x"; One; Sub; SetLocal "x"` does not give us.
     *
     * [1]: https://tc39.es/ecma262/#sec-tonumeric
     * [2]: https://tc39.es/ecma262/#sec-postfix-increment-operator
     *
     *   Category: Expressions
     *   Type: Conversions
     *   Operands:
     *   Stack: val => ToNumeric(val)
     */ \
    MACRO(ToNumeric, to_numeric, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * Convert a value to a string.
     *
     * Implements: [ToString][1](val).
     *
     * Note: This is used in code for template literals, like `${x}${y}`. Each
     * substituted value must be converted using ToString. `JSOp::Add` by itself
     * would do a slightly wrong kind of conversion (hint="number" rather than
     * hint="string").
     *
     * [1]: https://tc39.es/ecma262/#sec-tostring
     *
     *   Category: Expressions
     *   Type: Conversions
     *   Stack: val => ToString(val)
     */ \
    MACRO(ToString, to_string, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Test whether the value on top of the stack is `NullValue` or
     * `UndefinedValue` and push the boolean result.
     *
     *   Category: Expressions
     *   Type: Other expressions
     *   Operands:
     *   Stack: val => val, IsNullOrUndefined(val)
     */ \
    MACRO(IsNullOrUndefined, is_null_or_undefined, NULL, 1, 1, 2, JOF_BYTE) \
    /*
     * Push the global `this` value. Not to be confused with the `globalThis`
     * property on the global.
     *
     * This must be used only in scopes where `this` refers to the global
     * `this`.
     *
     *   Category: Expressions
     *   Type: Other expressions
     *   Operands:
     *   Stack: => this
     */ \
    MACRO(GlobalThis, global_this, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Push the global `this` value for non-syntactic scope. Not to be confused
     * with the `globalThis` property on the global.
     *
     * This must be used only in scopes where `this` refers to the global
     * `this`.
     *
     *   Category: Expressions
     *   Type: Other expressions
     *   Operands:
     *   Stack: => this
     */ \
    MACRO(NonSyntacticGlobalThis, non_syntactic_global_this, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Push the value of `new.target`.
     *
     * The result is a constructor or `undefined`.
     *
     * This must be used only in non-arrow function scripts.
     *
     * Implements: [GetNewTarget][1].
     *
     * [1]: https://tc39.es/ecma262/#sec-getnewtarget
     *
     *   Category: Expressions
     *   Type: Other expressions
     *   Operands:
     *   Stack: => new.target
     */ \
    MACRO(NewTarget, new_target, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Dynamic import of the module specified by the string value on the top of
     * the stack.
     *
     * Implements: [Import Calls][1].
     *
     * [1]: https://tc39.es/ecma262/#sec-import-calls
     *
     *   Category: Expressions
     *   Type: Other expressions
     *   Operands:
     *   Stack: moduleId, options => promise
     */ \
    MACRO(DynamicImport, dynamic_import, NULL, 1, 2, 1, JOF_BYTE) \
    /*
     * Push the `import.meta` object.
     *
     * This must be used only in module code.
     *
     *   Category: Expressions
     *   Type: Other expressions
     *   Operands:
     *   Stack: => import.meta
     */ \
    MACRO(ImportMeta, import_meta, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Create and push a new object with no properties.
     *
     *   Category: Objects
     *   Type: Creating objects
     *   Operands:
     *   Stack: => obj
     */ \
    MACRO(NewInit, new_init, NULL, 1, 0, 1, JOF_BYTE|JOF_IC) \
    /*
     * Create and push a new object of a predetermined shape.
     *
     * The new object has the shape `script->getShape(shapeIndex)`.
     * Subsequent `InitProp` instructions must fill in all slots of the new
     * object before it is used in any other way.
     *
     *   Category: Objects
     *   Type: Creating objects
     *   Operands: uint32_t shapeIndex
     *   Stack: => obj
     */ \
    MACRO(NewObject, new_object, NULL, 5, 0, 1, JOF_SHAPE|JOF_IC) \
    /*
     * Push a preconstructed object.
     *
     * Going one step further than `JSOp::NewObject`, this instruction doesn't
     * just reuse the shape--it actually pushes the preconstructed object
     * `script->getObject(objectIndex)` right onto the stack. The object must
     * be a singleton `PlainObject` or `ArrayObject`.
     *
     * The spec requires that an *ObjectLiteral* or *ArrayLiteral* creates a
     * new object every time it's evaluated, so this instruction must not be
     * used anywhere it might be executed more than once.
     *
     * This may only be used in non-function run-once scripts. Care also must
     * be taken to not emit in loops or other constructs where it could run
     * more than once.
     *
     *   Category: Objects
     *   Type: Creating objects
     *   Operands: uint32_t objectIndex
     *   Stack: => obj
     */ \
    MACRO(Object, object, NULL, 5, 0, 1, JOF_OBJECT) \
    /*
     * Create and push a new ordinary object with the provided [[Prototype]].
     *
     * This is used to create the `.prototype` object for derived classes.
     *
     *   Category: Objects
     *   Type: Creating objects
     *   Operands:
     *   Stack: proto => obj
     */ \
    MACRO(ObjWithProto, obj_with_proto, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Define a data property on an object.
     *
     * `obj` must be an object.
     *
     * Implements: [CreateDataPropertyOrThrow][1] as used in
     * [PropertyDefinitionEvaluation][2] of regular and shorthand
     * *PropertyDefinition*s.
     *
     *    [1]: https://tc39.es/ecma262/#sec-createdatapropertyorthrow
     *    [2]: https://tc39.es/ecma262/#sec-object-initializer-runtime-semantics-propertydefinitionevaluation
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    MACRO(InitProp, init_prop, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_IC) \
    /*
     * Like `JSOp::InitProp`, but define a non-enumerable property.
     *
     * This is used to define class methods.
     *
     * Implements: [PropertyDefinitionEvaluation][1] for methods, steps 3 and
     * 4, when *enumerable* is false.
     *
     *    [1]: https://tc39.es/ecma262/#sec-method-definitions-runtime-semantics-propertydefinitionevaluation
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    MACRO(InitHiddenProp, init_hidden_prop, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_IC) \
    /*
     * Like `JSOp::InitProp`, but define a non-enumerable, non-writable,
     * non-configurable property.
     *
     * This is used to define the `.prototype` property on classes.
     *
     * Implements: [MakeConstructor][1], step 8, when *writablePrototype* is
     * false.
     *
     *    [1]: https://tc39.es/ecma262/#sec-makeconstructor
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => obj
     */ \
    MACRO(InitLockedProp, init_locked_prop, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT|JOF_IC) \
    /*
     * Define a data property on `obj` with property key `id` and value `val`.
     *
     * `obj` must be an object.
     *
     * Implements: [CreateDataPropertyOrThrow][1]. This instruction is used for
     * object literals like `{0: val}` and `{[id]: val}`, and methods like
     * `*[Symbol.iterator]() {}`.
     *
     * `JSOp::InitHiddenElem` is the same but defines a non-enumerable property,
     * for class methods.
     * `JSOp::InitLockedElem` is the same but defines a non-enumerable, non-writable, non-configurable property,
     * for private class methods.
     *
     *    [1]: https://tc39.es/ecma262/#sec-createdatapropertyorthrow
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands:
     *   Stack: obj, id, val => obj
     */ \
    MACRO(InitElem, init_elem, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_IC) \
    MACRO(InitHiddenElem, init_hidden_elem, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_IC) \
    MACRO(InitLockedElem, init_locked_elem, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_IC) \
    /*
     * Define an accessor property on `obj` with the given `getter`.
     * `nameIndex` gives the property name.
     *
     * `obj` must be an object and `getter` must be a function.
     *
     * `JSOp::InitHiddenPropGetter` is the same but defines a non-enumerable
     * property, for getters in classes.
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, getter => obj
     */ \
    MACRO(InitPropGetter, init_prop_getter, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT) \
    MACRO(InitHiddenPropGetter, init_hidden_prop_getter, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT) \
    /*
     * Define an accessor property on `obj` with property key `id` and the given `getter`.
     *
     * This is used to implement getters like `get [id]() {}` or `get 0() {}`.
     *
     * `obj` must be an object and `getter` must be a function.
     *
     * `JSOp::InitHiddenElemGetter` is the same but defines a non-enumerable
     * property, for getters in classes.
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands:
     *   Stack: obj, id, getter => obj
     */ \
    MACRO(InitElemGetter, init_elem_getter, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT) \
    MACRO(InitHiddenElemGetter, init_hidden_elem_getter, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT) \
    /*
     * Define an accessor property on `obj` with the given `setter`.
     *
     * This is used to implement ordinary setters like `set foo(v) {}`.
     *
     * `obj` must be an object and `setter` must be a function.
     *
     * `JSOp::InitHiddenPropSetter` is the same but defines a non-enumerable
     * property, for setters in classes.
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, setter => obj
     */ \
    MACRO(InitPropSetter, init_prop_setter, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT) \
    MACRO(InitHiddenPropSetter, init_hidden_prop_setter, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPINIT) \
    /*
     * Define an accesssor property on `obj` with property key `id` and the
     * given `setter`.
     *
     * This is used to implement setters with computed property keys or numeric
     * keys.
     *
     * `JSOp::InitHiddenElemSetter` is the same but defines a non-enumerable
     * property, for setters in classes.
     *
     *   Category: Objects
     *   Type: Defining properties
     *   Operands:
     *   Stack: obj, id, setter => obj
     */ \
    MACRO(InitElemSetter, init_elem_setter, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT) \
    MACRO(InitHiddenElemSetter, init_hidden_elem_setter, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPINIT) \
    /*
     * Get the value of the property `obj.name`. This can call getters and
     * proxy traps.
     *
     * Implements: [GetV][1], [GetValue][2] step 5.
     *
     * [1]: https://tc39.es/ecma262/#sec-getv
     * [2]: https://tc39.es/ecma262/#sec-getvalue
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj => obj[name]
     */ \
    MACRO(GetProp, get_prop, NULL, 5, 1, 1, JOF_ATOM|JOF_PROP|JOF_IC) \
    /*
     * Get the value of the property `obj[key]`.
     *
     * Implements: [GetV][1], [GetValue][2] step 5.
     *
     * [1]: https://tc39.es/ecma262/#sec-getv
     * [2]: https://tc39.es/ecma262/#sec-getvalue
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands:
     *   Stack: obj, key => obj[key]
     */ \
    MACRO(GetElem, get_elem, NULL, 1, 2, 1, JOF_BYTE|JOF_ELEM|JOF_IC) \
    /*
     * Non-strict assignment to a property, `obj.name = val`.
     *
     * This throws a TypeError if `obj` is null or undefined. If it's a
     * primitive value, the property is set on ToObject(`obj`), typically with
     * no effect.
     *
     * Implements: [PutValue][1] step 6 for non-strict code.
     *
     * [1]: https://tc39.es/ecma262/#sec-putvalue
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => val
     */ \
    MACRO(SetProp, set_prop, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_CHECKSLOPPY|JOF_IC) \
    /*
     * Like `JSOp::SetProp`, but for strict mode code. Throw a TypeError if
     * `obj[key]` exists but is non-writable, if it's an accessor property with
     * no setter, or if `obj` is a primitive value.
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj, val => val
     */ \
    MACRO(StrictSetProp, strict_set_prop, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_CHECKSTRICT|JOF_IC) \
    /*
     * Non-strict assignment to a property, `obj[key] = val`.
     *
     * Implements: [PutValue][1] step 6 for non-strict code.
     *
     * [1]: https://tc39.es/ecma262/#sec-putvalue
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands:
     *   Stack: obj, key, val => val
     */ \
    MACRO(SetElem, set_elem, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPSET|JOF_CHECKSLOPPY|JOF_IC) \
    /*
     * Like `JSOp::SetElem`, but for strict mode code. Throw a TypeError if
     * `obj[key]` exists but is non-writable, if it's an accessor property with
     * no setter, or if `obj` is a primitive value.
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands:
     *   Stack: obj, key, val => val
     */ \
    MACRO(StrictSetElem, strict_set_elem, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_PROPSET|JOF_CHECKSTRICT|JOF_IC) \
    /*
     * Delete a property from `obj`. Push true on success, false if the
     * property existed but could not be deleted. This implements `delete
     * obj.name` in non-strict code.
     *
     * Throws if `obj` is null or undefined. Can call proxy traps.
     *
     * Implements: [`delete obj.propname`][1] step 5 in non-strict code.
     *
     * [1]: https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj => succeeded
     */ \
    MACRO(DelProp, del_prop, NULL, 5, 1, 1, JOF_ATOM|JOF_PROP|JOF_CHECKSLOPPY) \
    /*
     * Like `JSOp::DelProp`, but for strict mode code. Push `true` on success,
     * else throw a TypeError.
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: uint32_t nameIndex
     *   Stack: obj => succeeded
     */ \
    MACRO(StrictDelProp, strict_del_prop, NULL, 5, 1, 1, JOF_ATOM|JOF_PROP|JOF_CHECKSTRICT) \
    /*
     * Delete the property `obj[key]` and push `true` on success, `false`
     * if the property existed but could not be deleted.
     *
     * This throws if `obj` is null or undefined. Can call proxy traps.
     *
     * Implements: [`delete obj[key]`][1] step 5 in non-strict code.
     *
     * [1]: https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands:
     *   Stack: obj, key => succeeded
     */ \
    MACRO(DelElem, del_elem, NULL, 1, 2, 1, JOF_BYTE|JOF_ELEM|JOF_CHECKSLOPPY) \
    /*
     * Like `JSOp::DelElem, but for strict mode code. Push `true` on success,
     * else throw a TypeError.
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands:
     *   Stack: obj, key => succeeded
     */ \
    MACRO(StrictDelElem, strict_del_elem, NULL, 1, 2, 1, JOF_BYTE|JOF_ELEM|JOF_CHECKSTRICT) \
    /*
     * Push true if `obj` has an own property `id`.
     *
     * Note that `obj` is the top value, like `JSOp::In`.
     *
     * This opcode is not used for normal JS. Self-hosted code uses it by
     * calling the intrinsic `hasOwn(id, obj)`. For example,
     * `Object.prototype.hasOwnProperty` is implemented this way (see
     * js/src/builtin/Object.js).
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands:
     *   Stack: id, obj => (obj.hasOwnProperty(id))
     */ \
    MACRO(HasOwn, has_own, NULL, 1, 2, 1, JOF_BYTE|JOF_IC) \
    /*
     * Push a bool representing the presence of private field id on obj.
     * May throw, depending on the ThrowCondition.
     *
     * Two arguments:
     *   - throwCondition: One of the ThrowConditions defined in
     *     ThrowMsgKind.h. Determines why (or if) this op will throw.
     *   - msgKind: One of the ThrowMsgKinds defined in ThrowMsgKind.h, which
     *     maps to one of the messages in js.msg. Note: It's not possible to
     *     pass arguments to the message at the moment.
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: ThrowCondition throwCondition, ThrowMsgKind msgKind
     *   Stack: obj, key => obj, key, (obj.hasOwnProperty(id))
     */ \
    MACRO(CheckPrivateField, check_private_field, NULL, 3, 2, 3, JOF_TWO_UINT8|JOF_CHECKSTRICT|JOF_IC) \
    /*
     * Push a new private name.
     *
     *   Category: Objects
     *   Type: Accessing properties
     *   Operands: uint32_t nameIndex
     *   Stack: => private_name
     */ \
    MACRO(NewPrivateName, new_private_name, NULL, 5, 0, 1, JOF_ATOM) \
    /*
     * Push the SuperBase of the method `callee`. The SuperBase is
     * `callee.[[HomeObject]].[[GetPrototypeOf]]()`, the object where `super`
     * property lookups should begin.
     *
     * `callee` must be a function that has a HomeObject that's an object,
     * typically produced by `JSOp::Callee` or `JSOp::EnvCallee`.
     *
     * Implements: [GetSuperBase][1], except that instead of the environment,
     * the argument supplies the callee.
     *
     * [1]: https://tc39.es/ecma262/#sec-getsuperbase
     *
     *   Category: Objects
     *   Type: Super
     *   Operands:
     *   Stack: callee => superBase
     */ \
    MACRO(SuperBase, super_base, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Get the value of `receiver.name`, starting the property search at `obj`.
     * In spec terms, `obj.[[Get]](name, receiver)`.
     *
     * Implements: [GetValue][1] for references created by [`super.name`][2].
     * The `receiver` is `this` and `obj` is the SuperBase of the enclosing
     * method.
     *
     * [1]: https://tc39.es/ecma262/#sec-getvalue
     * [2]: https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
     *
     *   Category: Objects
     *   Type: Super
     *   Operands: uint32_t nameIndex
     *   Stack: receiver, obj => super.name
     */ \
    MACRO(GetPropSuper, get_prop_super, NULL, 5, 2, 1, JOF_ATOM|JOF_PROP|JOF_IC) \
    /*
     * Get the value of `receiver[key]`, starting the property search at `obj`.
     * In spec terms, `obj.[[Get]](key, receiver)`.
     *
     * Implements: [GetValue][1] for references created by [`super[key]`][2]
     * (where the `receiver` is `this` and `obj` is the SuperBase of the enclosing
     * method); [`Reflect.get(obj, key, receiver)`][3].
     *
     * [1]: https://tc39.es/ecma262/#sec-getvalue
     * [2]: https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
     * [3]: https://tc39.es/ecma262/#sec-reflect.get
     *
     *   Category: Objects
     *   Type: Super
     *   Operands:
     *   Stack: receiver, key, obj => super[key]
     */ \
    MACRO(GetElemSuper, get_elem_super, NULL, 1, 3, 1, JOF_BYTE|JOF_ELEM|JOF_IC) \
    /*
     * Assign `val` to `receiver.name`, starting the search for an existing
     * property at `obj`. In spec terms, `obj.[[Set]](name, val, receiver)`.
     *
     * Implements: [PutValue][1] for references created by [`super.name`][2] in
     * non-strict code. The `receiver` is `this` and `obj` is the SuperBase of
     * the enclosing method.
     *
     * [1]: https://tc39.es/ecma262/#sec-putvalue
     * [2]: https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
     *
     *   Category: Objects
     *   Type: Super
     *   Operands: uint32_t nameIndex
     *   Stack: receiver, obj, val => val
     */ \
    MACRO(SetPropSuper, set_prop_super, NULL, 5, 3, 1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_CHECKSLOPPY) \
    /*
     * Like `JSOp::SetPropSuper`, but for strict mode code.
     *
     *   Category: Objects
     *   Type: Super
     *   Operands: uint32_t nameIndex
     *   Stack: receiver, obj, val => val
     */ \
    MACRO(StrictSetPropSuper, strict_set_prop_super, NULL, 5, 3, 1, JOF_ATOM|JOF_PROP|JOF_PROPSET|JOF_CHECKSTRICT) \
    /*
     * Assign `val` to `receiver[key]`, strating the search for an existing
     * property at `obj`. In spec terms, `obj.[[Set]](key, val, receiver)`.
     *
     * Implements: [PutValue][1] for references created by [`super[key]`][2] in
     * non-strict code. The `receiver` is `this` and `obj` is the SuperBase of
     * the enclosing method.
     *
     * [1]: https://tc39.es/ecma262/#sec-putvalue
     * [2]: https://tc39.es/ecma262/#sec-super-keyword-runtime-semantics-evaluation
     *
     *   Category: Objects
     *   Type: Super
     *   Operands:
     *   Stack: receiver, key, obj, val => val
     */ \
    MACRO(SetElemSuper, set_elem_super, NULL, 1, 4, 1, JOF_BYTE|JOF_ELEM|JOF_PROPSET|JOF_CHECKSLOPPY) \
    /*
     * Like `JSOp::SetElemSuper`, but for strict mode code.
     *
     *   Category: Objects
     *   Type: Super
     *   Operands:
     *   Stack: receiver, key, obj, val => val
     */ \
    MACRO(StrictSetElemSuper, strict_set_elem_super, NULL, 1, 4, 1, JOF_BYTE|JOF_ELEM|JOF_PROPSET|JOF_CHECKSTRICT) \
    /*
     * Set up a for-in loop by pushing a `PropertyIteratorObject` over the
     * enumerable properties of `val`.
     *
     * Implements: [ForIn/OfHeadEvaluation][1] step 6,
     * [EnumerateObjectProperties][1]. (The spec refers to an "Iterator object"
     * with a `next` method, but notes that it "is never directly accessible"
     * to scripts. The object we use for this has no public methods.)
     *
     * If `val` is null or undefined, this pushes an empty iterator.
     *
     * The `iter` object pushed by this instruction must not be used or removed
     * from the stack except by `JSOp::MoreIter` and `JSOp::EndIter`, or by error
     * handling.
     *
     * The script's `JSScript::trynotes()` must mark the body of the `for-in`
     * loop, i.e. exactly those instructions that begin executing with `iter`
     * on the stack, starting with the next instruction (always
     * `JSOp::LoopHead`). Code must not jump into or out of this region: control
     * can enter only by executing `JSOp::Iter` and can exit only by executing a
     * `JSOp::EndIter` or by exception unwinding. (A `JSOp::EndIter` is always
     * emitted at the end of the loop, and extra copies are emitted on "exit
     * slides", where a `break`, `continue`, or `return` statement exits the
     * loop.)
     *
     * Typically a single try note entry marks the contiguous chunk of bytecode
     * from the instruction after `JSOp::Iter` to `JSOp::EndIter` (inclusive);
     * but if that range contains any instructions on exit slides, after a
     * `JSOp::EndIter`, then those must be correctly noted as *outside* the
     * loop.
     *
     * [1]: https://tc39.es/ecma262/#sec-runtime-semantics-forin-div-ofheadevaluation-tdznames-expr-iterationkind
     * [2]: https://tc39.es/ecma262/#sec-enumerate-object-properties
     *
     *   Category: Objects
     *   Type: Enumeration
     *   Operands:
     *   Stack: val => iter
     */ \
    MACRO(Iter, iter, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * Get the next property name for a for-in loop.
     *
     * `iter` must be a `PropertyIteratorObject` produced by `JSOp::Iter`.  This
     * pushes the property name for the next loop iteration, or
     * `MagicValue(JS_NO_ITER_VALUE)` if there are no more enumerable
     * properties to iterate over. The magic value must be used only by
     * `JSOp::IsNoIter` and `JSOp::EndIter`.
     *
     *   Category: Objects
     *   Type: Enumeration
     *   Operands:
     *   Stack: iter => iter, name
     */ \
    MACRO(MoreIter, more_iter, NULL, 1, 1, 2, JOF_BYTE) \
    /*
     * Test whether the value on top of the stack is
     * `MagicValue(JS_NO_ITER_VALUE)` and push the boolean result.
     *
     *   Category: Objects
     *   Type: Enumeration
     *   Operands:
     *   Stack: val => val, done
     */ \
    MACRO(IsNoIter, is_no_iter, NULL, 1, 1, 2, JOF_BYTE) \
    /*
     * Exit a for-in loop, closing the iterator.
     *
     * `iter` must be a `PropertyIteratorObject` pushed by `JSOp::Iter`.
     *
     *   Category: Objects
     *   Type: Enumeration
     *   Operands:
     *   Stack: iter, iterval =>
     */ \
    MACRO(EndIter, end_iter, NULL, 1, 2, 0, JOF_BYTE) \
    /*
     * If the iterator object on top of the stack has a `return` method,
     * call that method. If the method exists but does not return an object,
     * and `kind` is not `CompletionKind::Throw`, throw a TypeError. (If
     * `kind` is `Throw`, the error we are already throwing takes precedence.)
     *
     * `iter` must be an object conforming to the [Iterator][1] interface.
     *
     * Implements: [IteratorClose][2]
     *
     * [1]: https://tc39.es/ecma262/#sec-iterator-interface
     * [2]: https://tc39.es/ecma262/#sec-iteratorclose
     *   Category: Objects
     *   Type: Iteration
     *   Operands: CompletionKind kind
     *   Stack: iter =>
     */ \
    MACRO(CloseIter, close_iter, NULL, 2, 1, 0, JOF_UINT8|JOF_IC) \
    /*
     * Check that the top value on the stack is an object, and throw a
     * TypeError if not. `kind` is used only to generate an appropriate error
     * message.
     *
     * Implements: [GetIterator][1] step 5, [IteratorNext][2] step 3. Both
     * operations call a JS method which scripts can define however they want,
     * so they check afterwards that the method returned an object.
     *
     * [1]: https://tc39.es/ecma262/#sec-getiterator
     * [2]: https://tc39.es/ecma262/#sec-iteratornext
     *
     *   Category: Objects
     *   Type: Iteration
     *   Operands: CheckIsObjectKind kind
     *   Stack: result => result
     */ \
    MACRO(CheckIsObj, check_is_obj, NULL, 2, 1, 1, JOF_UINT8) \
    /*
     * Throw a TypeError if `val` is `null` or `undefined`.
     *
     * Implements: [RequireObjectCoercible][1]. But most instructions that
     * require an object will perform this check for us, so of the dozens of
     * calls to RequireObjectCoercible in the spec, we need this instruction
     * only for [destructuring assignment][2] and [initialization][3].
     *
     * [1]: https://tc39.es/ecma262/#sec-requireobjectcoercible
     * [2]: https://tc39.es/ecma262/#sec-runtime-semantics-destructuringassignmentevaluation
     * [3]: https://tc39.es/ecma262/#sec-destructuring-binding-patterns-runtime-semantics-bindinginitialization
     *
     *   Category: Objects
     *   Type: Iteration
     *   Operands:
     *   Stack: val => val
     */ \
    MACRO(CheckObjCoercible, check_obj_coercible, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Create and push an async iterator wrapping the sync iterator `iter`.
     * `next` should be `iter`'s `.next` method.
     *
     * Implements: [CreateAsyncToSyncIterator][1]. The spec says this operation
     * takes one argument, but that argument is a Record with two relevant
     * fields, `[[Iterator]]` and `[[NextMethod]]`.
     *
     * Used for `for await` loops.
     *
     * [1]: https://tc39.es/ecma262/#sec-createasyncfromsynciterator
     *
     *   Category: Objects
     *   Type: Iteration
     *   Operands:
     *   Stack: iter, next => asynciter
     */ \
    MACRO(ToAsyncIter, to_async_iter, NULL, 1, 2, 1, JOF_BYTE) \
    /*
     * Set the prototype of `obj`.
     *
     * `obj` must be an object.
     *
     * Implements: [B.3.1 __proto__ Property Names in Object Initializers][1], step 7.a.
     *
     * [1]: https://tc39.es/ecma262/#sec-__proto__-property-names-in-object-initializers
     *
     *   Category: Objects
     *   Type: SetPrototype
     *   Operands:
     *   Stack: obj, protoVal => obj
     */ \
    MACRO(MutateProto, mutate_proto, NULL, 1, 2, 1, JOF_BYTE) \
    /*
     * Create and push a new Array object with the given `length`,
     * preallocating enough memory to hold that many elements.
     *
     *   Category: Objects
     *   Type: Array literals
     *   Operands: uint32_t length
     *   Stack: => array
     */ \
    MACRO(NewArray, new_array, NULL, 5, 0, 1, JOF_UINT32|JOF_IC) \
    /*
     * Initialize an array element `array[index]` with value `val`.
     *
     * `val` may be `MagicValue(JS_ELEMENTS_HOLE)` pushed by `JSOp::Hole`.
     *
     * This never calls setters or proxy traps.
     *
     * `array` must be an Array object created by `JSOp::NewArray` with length >
     * `index`, and never used except by `JSOp::InitElemArray`.
     *
     * Implements: [ArrayAccumulation][1], the third algorithm, step 4, in the
     * common case where *nextIndex* is known.
     *
     * [1]: https://tc39.es/ecma262/#sec-runtime-semantics-arrayaccumulation
     *
     *   Category: Objects
     *   Type: Array literals
     *   Operands: uint32_t index
     *   Stack: array, val => array
     */ \
    MACRO(InitElemArray, init_elem_array, NULL, 5, 2, 1, JOF_UINT32|JOF_ELEM|JOF_PROPINIT) \
    /*
     * Initialize an array element `array[index++]` with value `val`.
     *
     * `val` may be `MagicValue(JS_ELEMENTS_HOLE)` pushed by `JSOp::Hole`. If it
     * is, no element is defined, but the array length and the stack value
     * `index` are still incremented.
     *
     * This never calls setters or proxy traps.
     *
     * `array` must be an Array object created by `JSOp::NewArray` and never used
     * except by `JSOp::InitElemArray` and `JSOp::InitElemInc`.
     *
     * `index` must be an integer, `0 <= index <= INT32_MAX`. If `index` is
     * `INT32_MAX`, this throws a RangeError. Unlike `InitElemArray`, it is not
     * necessary that the `array` length > `index`.
     *
     * This instruction is used when an array literal contains a
     * *SpreadElement*. In `[a, ...b, c]`, `InitElemArray 0` is used to put
     * `a` into the array, but `InitElemInc` is used for the elements of `b`
     * and for `c`.
     *
     * Implements: Several steps in [ArrayAccumulation][1] that call
     * CreateDataProperty, set the array length, and/or increment *nextIndex*.
     *
     * [1]: https://tc39.es/ecma262/#sec-runtime-semantics-arrayaccumulation
     *
     *   Category: Objects
     *   Type: Array literals
     *   Operands:
     *   Stack: array, index, val => array, (index + 1)
     */ \
    MACRO(InitElemInc, init_elem_inc, NULL, 1, 3, 2, JOF_BYTE|JOF_ELEM|JOF_PROPINIT|JOF_IC) \
    /*
     * Push `MagicValue(JS_ELEMENTS_HOLE)`, representing an *Elision* in an
     * array literal (like the missing property 0 in the array `[, 1]`).
     *
     * This magic value must be used only by `JSOp::InitElemArray` or
     * `JSOp::InitElemInc`.
     *
     *   Category: Objects
     *   Type: Array literals
     *   Operands:
     *   Stack: => hole
     */ \
    MACRO(Hole, hole, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Clone and push a new RegExp object.
     *
     * Implements: [Evaluation for *RegularExpressionLiteral*][1].
     *
     * [1]: https://tc39.es/ecma262/#sec-regular-expression-literals-runtime-semantics-evaluation
     *
     *   Category: Objects
     *   Type: RegExp literals
     *   Operands: uint32_t regexpIndex
     *   Stack: => regexp
     */ \
    MACRO(RegExp, reg_exp, NULL, 5, 0, 1, JOF_REGEXP) \
    /*
     * Initialize a new record, preallocating `length` memory slots. `length` can still grow
     * if needed, for example when using the spread operator.
     *
     * Implements: [RecordLiteral Evaluation][1] step 1.
     *
     * [1]: https://tc39.es/proposal-record-tuple/#sec-record-initializer-runtime-semantics-evaluation
     *
     *   Category: Compound primitives
     *   Type: Record literals
     *   Operands: uint32_t length
     *   Stack: => rval
     */ \
    IF_RECORD_TUPLE(MACRO(InitRecord, init_record, NULL, 5, 0, 1, JOF_UINT32)) \
    /*
     * Add the last element in the stack to the preceding tuple.
     *
     * Implements: [AddPropertyIntoRecordEntriesList][1].
     *
     * [1]: https://tc39.es/proposal-record-tuple/#sec-addpropertyintorecordentrieslist
     *
     *   Category: Compound primitives
     *   Type: Record literals
     *   Operands:
     *   Stack: record, key, value => record
     */ \
    IF_RECORD_TUPLE(MACRO(AddRecordProperty, add_record_property, NULL, 1, 3, 1, JOF_BYTE)) \
    /*
     * Add the last element in the stack to the preceding tuple.
     *
     * Implements: [RecordPropertyDefinitionEvaluation][1] for
     *   RecordPropertyDefinition : ... AssignmentExpression
     *
     * [1]: https://tc39.es/proposal-record-tuple/#sec-addpropertyintorecordentrieslist
     *
     *   Category: Compound primitives
     *   Type: Record literals
     *   Operands:
     *   Stack: record, value => record
     */ \
    IF_RECORD_TUPLE(MACRO(AddRecordSpread, add_record_spread, NULL, 1, 2, 1, JOF_BYTE)) \
    /*
     * Mark a record as "initialized", going from "write-only" mode to
     * "read-only" mode.
     *
     *   Category: Compound primitives
     *   Type: Record literals
     *   Operands:
     *   Stack: record => record
     */ \
    IF_RECORD_TUPLE(MACRO(FinishRecord, finish_record, NULL, 1, 1, 1, JOF_BYTE)) \
    /*
     * Initialize a new tuple, preallocating `length` memory slots. `length` can still grow
     * if needed, for example when using the spread operator.
     *
     * Implements: [TupleLiteral Evaluation][1] step 1.
     *
     * [1]: https://tc39.es/proposal-record-tuple/#sec-tuple-initializer-runtime-semantics-evaluation
     *
     *   Category: Compound primitives
     *   Type: Tuple literals
     *   Operands: uint32_t length
     *   Stack: => rval
     */ \
    IF_RECORD_TUPLE(MACRO(InitTuple, init_tuple, NULL, 5, 0, 1, JOF_UINT32)) \
    /*
     * Add the last element in the stack to the preceding tuple.
     *
     * Implements: [AddValueToTupleSequenceList][1].
     *
     * [1]: https://tc39.es/proposal-record-tuple/#sec-addvaluetotuplesequencelist
     *
     *   Category: Compound primitives
     *   Type: Tuple literals
     *   Operands:
     *   Stack: tuple, element => tuple
     */ \
    IF_RECORD_TUPLE(MACRO(AddTupleElement, add_tuple_element, NULL, 1, 2, 1, JOF_BYTE)) \
    /*
     * Mark a tuple as "initialized", going from "write-only" mode to
     * "read-only" mode.
     *
     *   Category: Compound primitives
     *   Type: Tuple literals
     *   Operands:
     *   Stack: tuple => tuple
     */ \
    IF_RECORD_TUPLE(MACRO(FinishTuple, finish_tuple, NULL, 1, 1, 1, JOF_BYTE)) \
    /*
     * Push a new function object.
     *
     * The new function inherits the current environment chain.
     *
     * Used to create most JS functions. Notable exceptions are derived or
     * default class constructors.
     *
     * Implements: [InstantiateFunctionObject][1], [Evaluation for
     * *FunctionExpression*][2], and so on.
     *
     * [1]: https://tc39.es/ecma262/#sec-function-definitions-runtime-semantics-instantiatefunctionobject
     * [2]: https://tc39.es/ecma262/#sec-function-definitions-runtime-semantics-evaluation
     *
     *   Category: Functions
     *   Type: Creating functions
     *   Operands: uint32_t funcIndex
     *   Stack: => fn
     */ \
    MACRO(Lambda, lambda, NULL, 5, 0, 1, JOF_OBJECT|JOF_USES_ENV) \
    /*
     * Set the name of a function.
     *
     * `fun` must be a function object. `name` must be a string, Int32 value,
     * or symbol (like the result of `JSOp::ToId`).
     *
     * Implements: [SetFunctionName][1], used e.g. to name methods with
     * computed property names.
     *
     * [1]: https://tc39.es/ecma262/#sec-setfunctionname
     *
     *   Category: Functions
     *   Type: Creating functions
     *   Operands: FunctionPrefixKind prefixKind
     *   Stack: fun, name => fun
     */ \
    MACRO(SetFunName, set_fun_name, NULL, 2, 2, 1, JOF_UINT8) \
    /*
     * Initialize the home object for functions with super bindings.
     *
     * `fun` must be a method, getter, or setter, so that it has a
     * [[HomeObject]] slot. `homeObject` must be a plain object or (for static
     * methods) a constructor.
     *
     *   Category: Functions
     *   Type: Creating functions
     *   Operands:
     *   Stack: fun, homeObject => fun
     */ \
    MACRO(InitHomeObject, init_home_object, NULL, 1, 2, 1, JOF_BYTE) \
    /*
     * Throw a TypeError if `baseClass` isn't either `null` or a constructor.
     *
     * Implements: [ClassDefinitionEvaluation][1] step 6.f.
     *
     * [1]: https://tc39.es/ecma262/#sec-runtime-semantics-classdefinitionevaluation
     *
     *   Category: Functions
     *   Type: Creating constructors
     *   Operands:
     *   Stack: baseClass => baseClass
     */ \
    MACRO(CheckClassHeritage, check_class_heritage, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Like `JSOp::Lambda`, but using `proto` as the new function's
     * `[[Prototype]]` (or `%FunctionPrototype%` if `proto` is `null`).
     *
     * `proto` must be either a constructor or `null`. We use
     * `JSOp::CheckClassHeritage` to check.
     *
     * This is used to create the constructor for a derived class.
     *
     * Implements: [ClassDefinitionEvaluation][1] steps 6.e.ii, 6.g.iii, and
     * 12 for derived classes.
     *
     * [1]: https://tc39.es/ecma262/#sec-runtime-semantics-classdefinitionevaluation
     *
     *   Category: Functions
     *   Type: Creating constructors
     *   Operands: uint32_t funcIndex
     *   Stack: proto => obj
     */ \
    MACRO(FunWithProto, fun_with_proto, NULL, 5, 1, 1, JOF_OBJECT|JOF_USES_ENV) \
    /*
     * Pushes the current global's %BuiltinObject%.
     *
     * `kind` must be a valid `BuiltinObjectKind` (and must not be
     * `BuiltinObjectKind::None`).
     *
     *   Category: Objects
     *   Type: Built-in objects
     *   Operands: uint8_t kind
     *   Stack: => %BuiltinObject%
     */ \
    MACRO(BuiltinObject, builtin_object, NULL, 2, 0, 1, JOF_UINT8) \
    /*
     * Invoke `callee` with `this` and `args`, and push the return value. Throw
     * a TypeError if `callee` isn't a function.
     *
     * `JSOp::CallContent` is for `callContentFunction` in self-hosted JS, and
     * this is for handling it differently in debugger's `onNativeCall` hook.
     * `onNativeCall` hook disables all JITs, and `JSOp::CallContent` is
     * treated exactly the same as `JSOP::Call` in JIT.
     *
     * `JSOp::CallIter` is used for implicit calls to @@iterator methods, to
     * ensure error messages are formatted with `JSMSG_NOT_ITERABLE` ("x is not
     * iterable") rather than `JSMSG_NOT_FUNCTION` ("x[Symbol.iterator] is not
     * a function"). The `argc` operand must be 0 for this variation.
     *
     * `JSOp::CallContentIter` is `JSOp::CallContent` variant of
     * `JSOp::CallIter`.
     *
     * `JSOp::CallIgnoresRv` hints to the VM that the return value is ignored.
     * This allows alternate faster implementations to be used that avoid
     * unnecesary allocations.
     *
     * Implements: [EvaluateCall][1] steps 4, 5, and 7.
     *
     * [1]: https://tc39.es/ecma262/#sec-evaluatecall
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     */ \
    MACRO(Call, call, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_IC) \
    MACRO(CallContent, call_content, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_IC) \
    MACRO(CallIter, call_iter, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_IC) \
    MACRO(CallContentIter, call_content_iter, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_IC) \
    MACRO(CallIgnoresRv, call_ignores_rv, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_IC) \
    /*
     * Like `JSOp::Call`, but the arguments are provided in an array rather than
     * a span of stack slots. Used to implement spread-call syntax:
     * `f(...args)`.
     *
     * `args` must be an Array object containing the actual arguments. The
     * array must be packed (dense and free of holes; see IsPackedArray).
     * This can be ensured by creating the array with `JSOp::NewArray` and
     * populating it using `JSOp::InitElemArray`.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: callee, this, args => rval
     */ \
    MACRO(SpreadCall, spread_call, NULL, 1, 3, 1, JOF_BYTE|JOF_INVOKE|JOF_SPREAD|JOF_IC) \
    /*
     * Push an array object that can be passed directly as the `args` argument
     * to `JSOp::SpreadCall`. If the operation can't be optimized, push
     * `undefined` instead.
     *
     * This instruction and the branch around the iterator loop are emitted
     * only when `iterable` is the sole argument in a call, as in `f(...arr)`.
     *
     * See `js::OptimizeSpreadCall`.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: iterable => array_or_undefined
     */ \
    MACRO(OptimizeSpreadCall, optimize_spread_call, NULL, 1, 1, 1, JOF_BYTE|JOF_IC) \
    /*
     * Perform a direct eval in the current environment if `callee` is the
     * builtin `eval` function, otherwise follow same behaviour as `JSOp::Call`.
     *
     * All direct evals use one of the JSOp::*Eval instructions here and these
     * opcodes are only used when the syntactic conditions for a direct eval
     * are met. If the builtin `eval` function is called though other means, it
     * becomes an indirect eval.
     *
     * Direct eval causes all bindings in *enclosing* non-global scopes to be
     * marked "aliased". The optimization that puts bindings in stack slots has
     * to prove that the bindings won't need to be captured by closures or
     * accessed using `JSOp::{Get,Bind,Set,Del}Name` instructions. Direct eval
     * makes that analysis impossible.
     *
     * The instruction immediately following any `JSOp::*Eval` instruction must
     * be `JSOp::Lineno`.
     *
     * Implements: [Function Call Evaluation][1], steps 5-7 and 9, when the
     * syntactic critera for direct eval in step 6 are all met.
     *
     * [1]: https://tc39.es/ecma262/#sec-function-calls-runtime-semantics-evaluation
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands: uint16_t argc
     *   Stack: callee, this, args[0], ..., args[argc-1] => rval
     */ \
    MACRO(Eval, eval, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_CHECKSLOPPY|JOF_IC) \
    /*
     * Spread-call variant of `JSOp::Eval`.
     *
     * See `JSOp::SpreadCall` for restrictions on `args`.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: callee, this, args => rval
     */ \
    MACRO(SpreadEval, spread_eval, NULL, 1, 3, 1, JOF_BYTE|JOF_INVOKE|JOF_SPREAD|JOF_CHECKSLOPPY|JOF_IC) \
    /*
     * Like `JSOp::Eval`, but for strict mode code.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands: uint16_t argc
     *   Stack: evalFn, this, args[0], ..., args[argc-1] => rval
     */ \
    MACRO(StrictEval, strict_eval, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_CHECKSTRICT|JOF_IC) \
    /*
     * Spread-call variant of `JSOp::StrictEval`.
     *
     * See `JSOp::SpreadCall` for restrictions on `args`.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: callee, this, args => rval
     */ \
    MACRO(StrictSpreadEval, strict_spread_eval, NULL, 1, 3, 1, JOF_BYTE|JOF_INVOKE|JOF_SPREAD|JOF_CHECKSTRICT|JOF_IC) \
    /*
     * Push the implicit `this` value for an unqualified function call, like
     * `foo()`. `nameIndex` gives the name of the function we're calling.
     *
     * The result is always `undefined` except when the name refers to a `with`
     * binding.  For example, in `with (date) { getFullYear(); }`, the
     * implicit `this` passed to `getFullYear` is `date`, not `undefined`.
     *
     * This walks the run-time environment chain looking for the environment
     * record that contains the function. If the function call definitely
     * refers to a local binding, use `JSOp::Undefined`.
     *
     * Implements: [EvaluateCall][1] step 1.b. But not entirely correctly.
     * See [bug 1166408][2].
     *
     * [1]: https://tc39.es/ecma262/#sec-evaluatecall
     * [2]: https://bugzilla.mozilla.org/show_bug.cgi?id=1166408
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands: uint32_t nameIndex
     *   Stack: => this
     */ \
    MACRO(ImplicitThis, implicit_this, "", 5, 0, 1, JOF_ATOM|JOF_USES_ENV) \
    /*
     * Push the call site object for a tagged template call.
     *
     * `script->getObject(objectIndex)` is the call site object.
     *
     * The call site object will already have the `.raw` property defined on it
     * and will be frozen.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands: uint32_t objectIndex
     *   Stack: => callSiteObj
     */ \
    MACRO(CallSiteObj, call_site_obj, NULL, 5, 0, 1, JOF_OBJECT) \
    /*
     * Push `MagicValue(JS_IS_CONSTRUCTING)`.
     *
     * This magic value is a required argument to the `JSOp::New` and
     * `JSOp::SuperCall` instructions and must not be used any other way.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: => JS_IS_CONSTRUCTING
     */ \
    MACRO(IsConstructing, is_constructing, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Invoke `callee` as a constructor with `args` and `newTarget`, and push
     * the return value. Throw a TypeError if `callee` isn't a constructor.
     *
     * `isConstructing` must be the value pushed by `JSOp::IsConstructing`.
     *
     * `JSOp::SuperCall` behaves exactly like `JSOp::New`, but is used for
     * *SuperCall* expressions, to allow JITs to distinguish them from `new`
     * expressions.
     *
     * `JSOp::NewContent` is for `constructContentFunction` in self-hosted JS.
     * See the comment for `JSOp::CallContent` for more details.
     *
     * Implements: [EvaluateConstruct][1] steps 7 and 8.
     *
     * [1]: https://tc39.es/ecma262/#sec-evaluatenew
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands: uint16_t argc
     *   Stack: callee, isConstructing, args[0], ..., args[argc-1], newTarget => rval
     */ \
    MACRO(New, new_, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_CONSTRUCT|JOF_IC) \
    MACRO(NewContent, new_content, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_CONSTRUCT|JOF_IC) \
    MACRO(SuperCall, super_call, NULL, 3, -1, 1, JOF_ARGC|JOF_INVOKE|JOF_CONSTRUCT|JOF_IC) \
    /*
     * Spread-call variant of `JSOp::New`.
     *
     * Invokes `callee` as a constructor with `args` and `newTarget`, and
     * pushes the return value onto the stack.
     *
     * `isConstructing` must be the value pushed by `JSOp::IsConstructing`.
     * See `JSOp::SpreadCall` for restrictions on `args`.
     *
     * `JSOp::SpreadSuperCall` behaves exactly like `JSOp::SpreadNew`, but is
     * used for *SuperCall* expressions.
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: callee, isConstructing, args, newTarget => rval
     */ \
    MACRO(SpreadNew, spread_new, NULL, 1, 4, 1, JOF_BYTE|JOF_INVOKE|JOF_CONSTRUCT|JOF_SPREAD|JOF_IC) \
    MACRO(SpreadSuperCall, spread_super_call, NULL, 1, 4, 1, JOF_BYTE|JOF_INVOKE|JOF_CONSTRUCT|JOF_SPREAD|JOF_IC) \
    /*
     * Push the prototype of `callee` in preparation for calling `super()`.
     *
     * `callee` must be a derived class constructor.
     *
     * Implements: [GetSuperConstructor][1], steps 4-7.
     *
     * [1]: https://tc39.es/ecma262/#sec-getsuperconstructor
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: callee => superFun
     */ \
    MACRO(SuperFun, super_fun, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Throw a ReferenceError if `thisval` is not
     * `MagicValue(JS_UNINITIALIZED_LEXICAL)`. Used in derived class
     * constructors to prohibit calling `super` more than once.
     *
     * Implements: [BindThisValue][1], step 3.
     *
     * [1]: https://tc39.es/ecma262/#sec-bindthisvalue
     *
     *   Category: Functions
     *   Type: Calls
     *   Operands:
     *   Stack: thisval => thisval
     */ \
    MACRO(CheckThisReinit, check_this_reinit, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Create and push a generator object for the current frame.
     *
     * This instruction must appear only in scripts for generators, async
     * functions, and async generators. There must not already be a generator
     * object for the current frame (that is, this instruction must execute at
     * most once per generator or async call).
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: => gen
     */ \
    MACRO(Generator, generator, NULL, 1, 0, 1, JOF_BYTE|JOF_USES_ENV) \
    /*
     * Suspend the current generator and return to the caller.
     *
     * When a generator is called, its script starts running, like any other JS
     * function, because [FunctionDeclarationInstantation][1] and other
     * [generator object setup][2] are implemented mostly in bytecode. However,
     * the *FunctionBody* of the generator is not supposed to start running
     * until the first `.next()` call, so after setup the script suspends
     * itself: the "initial yield".
     *
     * Later, when resuming execution, `rval`, `gen` and `resumeKind` will
     * receive the values passed in by `JSOp::Resume`. `resumeKind` is the
     * `GeneratorResumeKind` stored as an Int32 value.
     *
     * This instruction must appear only in scripts for generators and async
     * generators. `gen` must be the generator object for the current frame. It
     * must not have been previously suspended. The resume point indicated by
     * `resumeIndex` must be the next instruction in the script, which must be
     * `AfterYield`.
     *
     * Implements: [GeneratorStart][3], steps 4-7.
     *
     * [1]: https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
     * [2]: https://tc39.es/ecma262/#sec-generator-function-definitions-runtime-semantics-evaluatebody
     * [3]: https://tc39.es/ecma262/#sec-generatorstart
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands: uint24_t resumeIndex
     *   Stack: gen => rval, gen, resumeKind
     */ \
    MACRO(InitialYield, initial_yield, NULL, 4, 1, 3, JOF_RESUMEINDEX) \
    /*
     * Bytecode emitted after `yield` expressions. This is useful for the
     * Debugger and `AbstractGeneratorObject::isAfterYieldOrAwait`. It's
     * treated as jump target op so that the Baseline Interpreter can
     * efficiently restore the frame's interpreterICEntry when resuming a
     * generator.
     *
     * The preceding instruction in the script must be `Yield`, `InitialYield`,
     * or `Await`.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands: uint32_t icIndex
     *   Stack: =>
     */ \
    MACRO(AfterYield, after_yield, NULL, 5, 0, 0, JOF_ICINDEX) \
    /*
     * Suspend and close the current generator, async function, or async
     * generator.
     *
     * `gen` must be the generator object for the current frame.
     *
     * If the current function is a non-async generator, then the value in the
     * frame's return value slot is returned to the caller. It should be an
     * object of the form `{value: returnValue, done: true}`.
     *
     * If the current function is an async function or async generator, the
     * frame's return value slot must contain the current frame's result
     * promise, which must already be resolved or rejected.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: gen =>
     */ \
    MACRO(FinalYieldRval, final_yield_rval, NULL, 1, 1, 0, JOF_BYTE) \
    /*
     * Suspend execution of the current generator or async generator, returning
     * `rval1`.
     *
     * For non-async generators, `rval1` should be an object of the form
     * `{value: valueToYield, done: true}`. For async generators, `rval1`
     * should be the value to yield, and the caller is responsible for creating
     * the iterator result object (under `js::AsyncGeneratorYield`).
     *
     * This instruction must appear only in scripts for generators and async
     * generators. `gen` must be the generator object for the current stack
     * frame. The resume point indicated by `resumeIndex` must be the next
     * instruction in the script, which must be `AfterYield`.
     *
     * When resuming execution, `rval2`, `gen` and `resumeKind` receive the
     * values passed in by `JSOp::Resume`.
     *
     * Implements: [GeneratorYield][1] and [AsyncGeneratorYield][2].
     *
     * [1]: https://tc39.es/ecma262/#sec-generatoryield
     * [2]: https://tc39.es/ecma262/#sec-asyncgeneratoryield
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands: uint24_t resumeIndex
     *   Stack: rval1, gen => rval2, gen, resumeKind
     */ \
    MACRO(Yield, yield, NULL, 4, 2, 3, JOF_RESUMEINDEX) \
    /*
     * Pushes a boolean indicating whether the top of the stack is
     * `MagicValue(JS_GENERATOR_CLOSING)`.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: val => val, res
     */ \
    MACRO(IsGenClosing, is_gen_closing, NULL, 1, 1, 2, JOF_BYTE) \
    /*
     * Arrange for this async function to resume asynchronously when `value`
     * becomes resolved.
     *
     * This is the last thing an async function does before suspending for an
     * `await` expression. It coerces the awaited `value` to a promise and
     * effectively calls `.then()` on it, passing handler functions that will
     * resume this async function call later. See `js::AsyncFunctionAwait`.
     *
     * This instruction must appear only in non-generator async function
     * scripts. `gen` must be the internal generator object for the current
     * frame. After this instruction, the script should suspend itself with
     * `Await` (rather than exiting any other way).
     *
     * The result `promise` is the async function's result promise,
     * `gen->as<AsyncFunctionGeneratorObject>().promise()`.
     *
     * Implements: [Await][1], steps 2-9.
     *
     * [1]: https://tc39.github.io/ecma262/#await
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: value, gen => promise
     */ \
    MACRO(AsyncAwait, async_await, NULL, 1, 2, 1, JOF_BYTE) \
    /*
     * Resolve or reject the current async function's result promise with
     * 'valueOrReason'.
     *
     * This instruction must appear only in non-generator async function
     * scripts. `gen` must be the internal generator object for the current
     * frame. This instruction must run at most once per async function call,
     * as resolving/rejecting an already resolved/rejected promise is not
     * permitted.
     *
     * The result `promise` is the async function's result promise,
     * `gen->as<AsyncFunctionGeneratorObject>().promise()`.
     *
     * Implements: [AsyncFunctionStart][1], step 4.d.i. and 4.e.i.
     *
     * [1]: https://tc39.es/ecma262/#sec-async-functions-abstract-operations-async-function-start
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands: AsyncFunctionResolveKind fulfillOrReject
     *   Stack: valueOrReason, gen => promise
     */ \
    MACRO(AsyncResolve, async_resolve, NULL, 2, 2, 1, JOF_UINT8) \
    /*
     * Suspend the current frame for an `await` expression.
     *
     * This instruction must appear only in scripts for async functions and
     * async generators. `gen` must be the internal generator object for the
     * current frame.
     *
     * This returns `promise` to the caller. Later, when this async call is
     * resumed, `resolved`, `gen` and `resumeKind` receive the values passed in
     * by `JSOp::Resume`, and execution continues at the next instruction,
     * which must be `AfterYield`.
     *
     * This instruction is used in two subtly different ways.
     *
     * 1.  In async functions:
     *
     *         ...                          # valueToAwait
     *         GetAliasedVar ".generator"   # valueToAwait gen
     *         AsyncAwait                   # resultPromise
     *         GetAliasedVar ".generator"   # resultPromise gen
     *         Await                        # resolved gen resumeKind
     *         AfterYield
     *
     *     `AsyncAwait` arranges for this frame to be resumed later and pushes
     *     its result promise. `Await` then suspends the frame and removes it
     *     from the stack, returning the result promise to the caller. (If this
     *     async call hasn't awaited before, the caller may be user code.
     *     Otherwise, the caller is self-hosted code using `resumeGenerator`.)
     *
     * 2.  In async generators:
     *
     *         ...                          # valueToAwait
     *         GetAliasedVar ".generator"   # valueToAwait gen
     *         Await                        # resolved gen resumeKind
     *         AfterYield
     *
     *     `AsyncAwait` is not used, so (1) the value returned to the caller by
     *     `Await` is `valueToAwait`, not `resultPromise`; and (2) the caller
     *     is responsible for doing the async-generator equivalent of
     *     `AsyncAwait` (namely, `js::AsyncGeneratorAwait`, called from
     *     `js::AsyncGeneratorResume` after `js::CallSelfHostedFunction`
     *     returns).
     *
     * Implements: [Await][1], steps 10-12.
     *
     * [1]: https://tc39.es/ecma262/#await
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands: uint24_t resumeIndex
     *   Stack: promise, gen => resolved, gen, resumeKind
     */ \
    MACRO(Await, await, NULL, 4, 2, 3, JOF_RESUMEINDEX) \
    /*
     * Test if the re-entry to the microtask loop may be skipped.
     *
     * This is part of an optimization for `await` expressions. Programs very
     * often await values that aren't promises, or promises that are already
     * resolved. We can then sometimes skip suspending the current frame and
     * returning to the microtask loop. If the circumstances permit the
     * optimization, `CanSkipAwait` pushes true if the optimization is allowed,
     * and false otherwise.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: value => value, can_skip
     */ \
    MACRO(CanSkipAwait, can_skip_await, NULL, 1, 1, 2, JOF_BYTE) \
    /*
     * Potentially extract an awaited value, if the await is skippable
     *
     * If re-entering the microtask loop is skippable (as checked by CanSkipAwait)
     * if can_skip is true,  `MaybeExtractAwaitValue` replaces `value` with the result of the
     * `await` expression (unwrapping the resolved promise, if any). Otherwise, value remains
     * as is.
     *
     * In both cases, can_skip remains the same.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: value, can_skip => value_or_resolved, can_skip
     */ \
    MACRO(MaybeExtractAwaitValue, maybe_extract_await_value, NULL, 1, 2, 2, JOF_BYTE) \
    /*
     * Pushes one of the GeneratorResumeKind values as Int32Value.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands: GeneratorResumeKind resumeKind (encoded as uint8_t)
     *   Stack: => resumeKind
     */ \
    MACRO(ResumeKind, resume_kind, NULL, 2, 0, 1, JOF_UINT8) \
    /*
     * Handle Throw and Return resumption.
     *
     * `gen` must be the generator object for the current frame. `resumeKind`
     * must be a `GeneratorResumeKind` stored as an `Int32` value. If it is
     * `Next`, continue to the next instruction. If `resumeKind` is `Throw` or
     * `Return`, these completions are handled by throwing an exception. See
     * `GeneratorThrowOrReturn`.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: rval, gen, resumeKind => rval
     */ \
    MACRO(CheckResumeKind, check_resume_kind, NULL, 1, 3, 1, JOF_BYTE) \
    /*
     * Resume execution of a generator, async function, or async generator.
     *
     * This behaves something like a call instruction. It pushes a stack frame
     * (the one saved when `gen` was suspended, rather than a fresh one) and
     * runs instructions in it. Once `gen` returns or yields, its return value
     * is pushed to this frame's stack and execution continues in this script.
     *
     * This instruction is emitted only for the `resumeGenerator` self-hosting
     * intrinsic. It is used in the implementation of
     * `%GeneratorPrototype%.next`, `.throw`, and `.return`.
     *
     * `gen` must be a suspended generator object. `resumeKind` must be in
     * range for `GeneratorResumeKind`.
     *
     *   Category: Functions
     *   Type: Generators and async functions
     *   Operands:
     *   Stack: gen, val, resumeKind => rval
     */ \
    MACRO(Resume, resume, NULL, 1, 3, 1, JOF_BYTE|JOF_INVOKE) \
    /*
     * No-op instruction marking the target of a jump instruction.
     *
     * This instruction and a few others (see `js::BytecodeIsJumpTarget`) are
     * jump target instructions. The Baseline Interpreter uses these
     * instructions to sync the frame's `interpreterICEntry` after a jump. Ion
     * uses them to find block boundaries when translating bytecode to MIR.
     *
     *   Category: Control flow
     *   Type: Jump targets
     *   Operands: uint32_t icIndex
     *   Stack: =>
     */ \
    MACRO(JumpTarget, jump_target, NULL, 5, 0, 0, JOF_ICINDEX) \
    /*
     * Marks the target of the backwards jump for some loop.
     *
     * This is a jump target instruction (see `JSOp::JumpTarget`). Additionally,
     * it checks for interrupts and handles JIT tiering.
     *
     * The `depthHint` operand is a loop depth hint for Ion. It starts at 1 and
     * deeply nested loops all have the same value.
     *
     * For the convenience of the JITs, scripts must not start with this
     * instruction. See bug 1602390.
     *
     *   Category: Control flow
     *   Type: Jump targets
     *   Operands: uint32_t icIndex, uint8_t depthHint
     *   Stack: =>
     */ \
    MACRO(LoopHead, loop_head, NULL, 6, 0, 0, JOF_LOOPHEAD) \
    /*
     * Jump to a 32-bit offset from the current bytecode.
     *
     * See "Jump instructions" above for details.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: =>
     */ \
    MACRO(Goto, goto_, NULL, 5, 0, 0, JOF_JUMP) \
    /*
     * If ToBoolean(`cond`) is false, jumps to a 32-bit offset from the current
     * instruction.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t forwardOffset
     *   Stack: cond =>
     */ \
    MACRO(JumpIfFalse, jump_if_false, NULL, 5, 1, 0, JOF_JUMP|JOF_IC) \
    /*
     * If ToBoolean(`cond`) is true, jump to a 32-bit offset from the current
     * instruction.
     *
     * `offset` may be positive or negative. This is the instruction used at the
     * end of a do-while loop to jump back to the top.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t offset
     *   Stack: cond =>
     */ \
    MACRO(JumpIfTrue, jump_if_true, NULL, 5, 1, 0, JOF_JUMP|JOF_IC) \
    /*
     * Short-circuit for logical AND.
     *
     * If ToBoolean(`cond`) is false, jump to a 32-bit offset from the current
     * instruction. The value remains on the stack.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t forwardOffset
     *   Stack: cond => cond
     */ \
    MACRO(And, and_, NULL, 5, 1, 1, JOF_JUMP|JOF_IC) \
    /*
     * Short-circuit for logical OR.
     *
     * If ToBoolean(`cond`) is true, jump to a 32-bit offset from the current
     * instruction. The value remains on the stack.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t forwardOffset
     *   Stack: cond => cond
     */ \
    MACRO(Or, or_, NULL, 5, 1, 1, JOF_JUMP|JOF_IC) \
    /*
     * Short-circuiting for nullish coalescing.
     *
     * If `val` is not null or undefined, jump to a 32-bit offset from the
     * current instruction.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t forwardOffset
     *   Stack: val => val
     */ \
    MACRO(Coalesce, coalesce, NULL, 5, 1, 1, JOF_JUMP) \
     /*
     * Like `JSOp::JumpIfTrue`, but if the branch is taken, pop and discard an
     * additional stack value.
     *
     * This is used to implement `switch` statements when the
     * `JSOp::TableSwitch` optimization is not possible. The switch statement
     *
     *     switch (expr) {
     *         case A: stmt1;
     *         case B: stmt2;
     *     }
     *
     * compiles to this bytecode:
     *
     *         # dispatch code - evaluate expr, check it against each `case`,
     *         # jump to the right place in the body or to the end.
     *         <expr>
     *         Dup; <A>; StrictEq; Case L1; JumpTarget
     *         Dup; <B>; StrictEq; Case L2; JumpTarget
     *         Default LE
     *
     *         # body code
     *     L1: JumpTarget; <stmt1>
     *     L2: JumpTarget; <stmt2>
     *     LE: JumpTarget
     *
     * This opcode is weird: it's the only one whose ndefs varies depending on
     * which way a conditional branch goes. We could implement switch
     * statements using `JSOp::JumpIfTrue` and `JSOp::Pop`, but that would also
     * be awkward--putting the `JSOp::Pop` inside the `switch` body would
     * complicate fallthrough.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t forwardOffset
     *   Stack: val, cond => val (if !cond)
     */ \
    MACRO(Case, case_, NULL, 5, 2, 1, JOF_JUMP) \
    /*
     * Like `JSOp::Goto`, but pop and discard an additional stack value.
     *
     * This appears after all cases for a non-optimized `switch` statement. If
     * there's a `default:` label, it jumps to that point in the body;
     * otherwise it jumps to the next statement.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t forwardOffset
     *   Stack: lval =>
     */ \
    MACRO(Default, default_, NULL, 5, 1, 0, JOF_JUMP) \
    /*
     * Optimized switch-statement dispatch, used when all `case` labels are
     * small integer constants.
     *
     * If `low <= i <= high`, jump to the instruction at the offset given by
     * `script->resumeOffsets()[firstResumeIndex + i - low]`, in bytes from the
     * start of the current script's bytecode. Otherwise, jump to the
     * instruction at `defaultOffset` from the current instruction. All of
     * these offsets must be in range for the current script and must point to
     * `JSOp::JumpTarget` instructions.
     *
     * The following inequalities must hold: `low <= high` and
     * `firstResumeIndex + high - low < resumeOffsets().size()`.
     *
     *   Category: Control flow
     *   Type: Jumps
     *   Operands: int32_t defaultOffset, int32_t low, int32_t high,
     *             uint24_t firstResumeIndex
     *   Stack: i =>
     */ \
    MACRO(TableSwitch, table_switch, NULL, 16, 1, 0, JOF_TABLESWITCH) \
    /*
     * Return `rval`.
     *
     * This must not be used in derived class constructors. Instead use
     * `JSOp::SetRval`, `JSOp::CheckReturn`, and `JSOp::RetRval`.
     *
     *   Category: Control flow
     *   Type: Return
     *   Operands:
     *   Stack: rval =>
     */ \
    MACRO(Return, return_, NULL, 1, 1, 0, JOF_BYTE) \
    /*
     * Push the current stack frame's `returnValue`. If no `JSOp::SetRval`
     * instruction has been executed in this stack frame, this is `undefined`.
     *
     * Every stack frame has a `returnValue` slot, used by top-level scripts,
     * generators, async functions, and derived class constructors. Plain
     * functions usually use `JSOp::Return` instead.
     *
     *   Category: Control flow
     *   Type: Return
     *   Operands:
     *   Stack: => rval
     */ \
    MACRO(GetRval, get_rval, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Store `rval` in the current stack frame's `returnValue` slot.
     *
     * This instruction must not be used in a toplevel script compiled with the
     * `noScriptRval` option.
     *
     *   Category: Control flow
     *   Type: Return
     *   Operands:
     *   Stack: rval =>
     */ \
    MACRO(SetRval, set_rval, NULL, 1, 1, 0, JOF_BYTE) \
    /*
     * Stop execution and return the current stack frame's `returnValue`. If no
     * `JSOp::SetRval` instruction has been executed in this stack frame, this
     * is `undefined`.
     *
     * Also emitted at end of every script so consumers don't need to worry
     * about running off the end.
     *
     * If the current script is a derived class constructor, `returnValue` must
     * be an object. The script can use `JSOp::CheckReturn` to ensure this.
     *
     *   Category: Control flow
     *   Type: Return
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(RetRval, ret_rval, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Check the return value in a derived class constructor.
     *
     * -   If the current stack frame's `returnValue` is an object, push
     *     `returnValue` onto the stack.
     *
     * -   Otherwise, if the `returnValue` is undefined and `thisval` is an
     *     object, push `thisval` onto the stack.
     *
     * -   Otherwise, throw a TypeError.
     *
     * This is exactly what has to happen when a derived class constructor
     * returns. `thisval` should be the current value of `this`, or
     * `MagicValue(JS_UNINITIALIZED_LEXICAL)` if `this` is uninitialized.
     *
     * Implements: [The [[Construct]] internal method of JS functions][1],
     * steps 13 and 15.
     *
     * [1]: https://tc39.es/ecma262/#sec-ecmascript-function-objects-construct-argumentslist-newtarget
     *
     *   Category: Control flow
     *   Type: Return
     *   Operands:
     *   Stack: thisval => rval
     */ \
    MACRO(CheckReturn, check_return, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Throw `exc`. (ノಠ益ಠ)ノ彡┴──┴
     *
     * This sets the pending exception to `exc` and jumps to error-handling
     * code. If we're in a `try` block, error handling adjusts the stack and
     * environment chain and resumes execution at the top of the `catch` or
     * `finally` block. Otherwise it starts unwinding the stack.
     *
     * Implements: [*ThrowStatement* Evaluation][1], step 3.
     *
     * This is also used in for-of loops. If the body of the loop throws an
     * exception, we catch it, close the iterator, then use `JSOp::Throw` to
     * rethrow.
     *
     * [1]: https://tc39.es/ecma262/#sec-throw-statement-runtime-semantics-evaluation
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands:
     *   Stack: exc =>
     */ \
    MACRO(Throw, throw_, NULL, 1, 1, 0, JOF_BYTE) \
    /*
     * Create and throw an Error object.
     *
     * Sometimes we know at emit time that an operation always throws. For
     * example, `delete super.prop;` is allowed in methods, but always throws a
     * ReferenceError.
     *
     * `msgNumber` determines the `.message` and [[Prototype]] of the new Error
     * object.  It must be an error number in js/public/friend/ErrorNumbers.msg.
     * The number of arguments in the error message must be 0.
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands: ThrowMsgKind msgNumber
     *   Stack: =>
     */ \
    MACRO(ThrowMsg, throw_msg, NULL, 2, 0, 0, JOF_UINT8) \
    /*
     * Throws a runtime TypeError for invalid assignment to a `const` binding.
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands: uint32_t nameIndex
     *   Stack:
     */ \
    MACRO(ThrowSetConst, throw_set_const, NULL, 5, 0, 0, JOF_ATOM|JOF_NAME) \
    /*
     * No-op instruction that marks the top of the bytecode for a
     * *TryStatement*.
     *
     * Location information for catch/finally blocks is stored in a side table,
     * `script->trynotes()`.
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(Try, try_, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * No-op instruction used by the exception unwinder to determine the
     * correct environment to unwind to when performing IteratorClose due to
     * destructuring.
     *
     * This instruction must appear immediately before each
     * `JSTRY_DESTRUCTURING` span in a script's try notes.
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(TryDestructuring, try_destructuring, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Push and clear the pending exception. ┬──┬◡ﾉ(° -°ﾉ)
     *
     * This must be used only in the fixed sequence of instructions following a
     * `JSTRY_CATCH` span (see "Bytecode Invariants" above), as that's the only
     * way instructions would run with an exception pending.
     *
     * Used to implement catch-blocks, including the implicit ones generated as
     * part of for-of iteration.
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands:
     *   Stack: => exception
     */ \
    MACRO(Exception, exception, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * No-op instruction that marks the start of a `finally` block.
     *
     *   Category: Control flow
     *   Type: Exceptions
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(Finally, finally, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Push `MagicValue(JS_UNINITIALIZED_LEXICAL)`, a magic value used to mark
     * a binding as uninitialized.
     *
     * This magic value must be used only by `JSOp::InitLexical`.
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands:
     *   Stack: => uninitialized
     */ \
    MACRO(Uninitialized, uninitialized, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Initialize an optimized local lexical binding; or mark it as
     * uninitialized.
     *
     * This stores the value `v` in the fixed slot `localno` in the current
     * stack frame. If `v` is the magic value produced by `JSOp::Uninitialized`,
     * this marks the binding as uninitialized. Otherwise this initializes the
     * binding with value `v`.
     *
     * Implements: [CreateMutableBinding][1] step 3, substep "record that it is
     * uninitialized", and [InitializeBinding][2], for optimized locals. (Note:
     * this is how `const` bindings are initialized.)
     *
     * [1]: https://tc39.es/ecma262/#sec-declarative-environment-records-createmutablebinding-n-d
     * [2]: https://tc39.es/ecma262/#sec-declarative-environment-records-initializebinding-n-v
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands: uint24_t localno
     *   Stack: v => v
     */ \
    MACRO(InitLexical, init_lexical, NULL, 4, 1, 1, JOF_LOCAL|JOF_NAME) \
    /*
     * Initialize a global lexical binding.
     *
     * The binding must already have been created by
     * `GlobalOrEvalDeclInstantiation` and must be uninitialized.
     *
     * Like `JSOp::InitLexical` but for global lexicals. Unlike `InitLexical`
     * this can't be used to mark a binding as uninitialized.
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands: uint32_t nameIndex
     *   Stack: val => val
     */ \
    MACRO(InitGLexical, init_g_lexical, NULL, 5, 1, 1, JOF_ATOM|JOF_NAME|JOF_PROPINIT|JOF_GNAME|JOF_IC) \
    /*
     * Initialize an aliased lexical binding; or mark it as uninitialized.
     *
     * Like `JSOp::InitLexical` but for aliased bindings.
     *
     * Note: There is no even-less-optimized `InitName` instruction because JS
     * doesn't need it. We always know statically which binding we're
     * initializing.
     *
     * `hops` is usually 0, but in `function f(a=eval("var b;")) { }`, the
     * argument `a` is initialized from inside a nested scope, so `hops == 1`.
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: v => v
     */ \
    MACRO(InitAliasedLexical, init_aliased_lexical, NULL, 5, 1, 1, JOF_ENVCOORD|JOF_NAME|JOF_PROPINIT) \
    /*
     * Throw a ReferenceError if the value on top of the stack is uninitialized.
     *
     * Typically used after `JSOp::GetLocal` with the same `localno`.
     *
     * Implements: [GetBindingValue][1] step 3 and [SetMutableBinding][2] step
     * 4 for declarative Environment Records.
     *
     * [1]: https://tc39.es/ecma262/#sec-declarative-environment-records-getbindingvalue-n-s
     * [2]: https://tc39.es/ecma262/#sec-declarative-environment-records-setmutablebinding-n-v-s
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands: uint24_t localno
     *   Stack: v => v
     */ \
    MACRO(CheckLexical, check_lexical, NULL, 4, 1, 1, JOF_LOCAL|JOF_NAME) \
    /*
     * Like `JSOp::CheckLexical` but for aliased bindings.
     *
     * Typically used after `JSOp::GetAliasedVar` with the same hops/slot.
     *
     * Note: There are no `CheckName` or `CheckGName` instructions because
     * they're unnecessary. `JSOp::{Get,Set}{Name,GName}` all check for
     * uninitialized lexicals and throw if needed.
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: v => v
     */ \
    MACRO(CheckAliasedLexical, check_aliased_lexical, NULL, 5, 1, 1, JOF_ENVCOORD|JOF_NAME) \
    /*
     * Throw a ReferenceError if the value on top of the stack is
     * `MagicValue(JS_UNINITIALIZED_LEXICAL)`. Used in derived class
     * constructors to check `this` (which needs to be initialized before use,
     * by calling `super()`).
     *
     * Implements: [GetThisBinding][1] step 3.
     *
     * [1]: https://tc39.es/ecma262/#sec-function-environment-records-getthisbinding
     *
     *   Category: Variables and scopes
     *   Type: Initialization
     *   Operands:
     *   Stack: this => this
     */ \
    MACRO(CheckThis, check_this, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Look up a name on the global lexical environment's chain and push the
     * environment which contains a binding for that name. If no such binding
     * exists, push the global lexical environment.
     *
     *   Category: Variables and scopes
     *   Type: Looking up bindings
     *   Operands: uint32_t nameIndex
     *   Stack: => global
     */ \
    MACRO(BindGName, bind_g_name, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME|JOF_GNAME|JOF_IC) \
    /*
     * Look up a name on the environment chain and push the environment which
     * contains a binding for that name. If no such binding exists, push the
     * global lexical environment.
     *
     *   Category: Variables and scopes
     *   Type: Looking up bindings
     *   Operands: uint32_t nameIndex
     *   Stack: => env
     */ \
    MACRO(BindName, bind_name, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME|JOF_IC|JOF_USES_ENV) \
    /*
     * Find a binding on the environment chain and push its value.
     *
     * If the binding is an uninitialized lexical, throw a ReferenceError. If
     * no such binding exists, throw a ReferenceError unless the next
     * instruction is `JSOp::Typeof`, in which case push `undefined`.
     *
     * Implements: [ResolveBinding][1] followed by [GetValue][2]
     * (adjusted hackily for `typeof`).
     *
     * This is the fallback `Get` instruction that handles all unoptimized
     * cases. Optimized instructions follow.
     *
     * [1]: https://tc39.es/ecma262/#sec-resolvebinding
     * [2]: https://tc39.es/ecma262/#sec-getvalue
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: => val
     */ \
    MACRO(GetName, get_name, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME|JOF_IC|JOF_USES_ENV) \
    /*
     * Find a global binding and push its value.
     *
     * This searches the global lexical environment and, failing that, the
     * global object. (Unlike most declarative environments, the global lexical
     * environment can gain more bindings after compilation, possibly shadowing
     * global object properties.)
     *
     * This is an optimized version of `JSOp::GetName` that skips all local
     * scopes, for use when the name doesn't refer to any local binding.
     * `NonSyntacticVariablesObject`s break this optimization, so if the
     * current script has a non-syntactic global scope, use `JSOp::GetName`
     * instead.
     *
     * Like `JSOp::GetName`, this throws a ReferenceError if no such binding is
     * found (unless the next instruction is `JSOp::Typeof`) or if the binding
     * is an uninitialized lexical.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: => val
     */ \
    MACRO(GetGName, get_g_name, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME|JOF_GNAME|JOF_IC) \
    /*
     * Push the value of an argument that is stored in the stack frame
     * or in an `ArgumentsObject`.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint16_t argno
     *   Stack: => arguments[argno]
     */ \
    MACRO(GetArg, get_arg, NULL, 3, 0, 1, JOF_QARG|JOF_NAME) \
    /*
     * Push the value of an argument that is stored in the stack frame. Like
     * `JSOp::GetArg`, but ignores the frame's `ArgumentsObject` and doesn't
     * assert the argument is unaliased.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint16_t argno
     *   Stack: => arguments[argno]
     */ \
    MACRO(GetFrameArg, get_frame_arg, NULL, 3, 0, 1, JOF_QARG|JOF_NAME) \
    /*
     * Push the value of an optimized local variable.
     *
     * If the variable is an uninitialized lexical, push
     * `MagicValue(JS_UNINIITALIZED_LEXICAL)`.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint24_t localno
     *   Stack: => val
     */ \
    MACRO(GetLocal, get_local, NULL, 4, 0, 1, JOF_LOCAL|JOF_NAME) \
    /*
     * Push the number of actual arguments as Int32Value.
     *
     * This is emitted for the ArgumentsLength() intrinsic in self-hosted code.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands:
     *   Stack: => arguments.length
     */ \
    MACRO(ArgumentsLength, arguments_length, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Push the value of an argument that is stored in the stack frame. The
     * value on top of the stack must be an Int32Value storing the index. The
     * index must be less than the number of actual arguments.
     *
     * This is emitted for the GetArgument(i) intrinsic in self-hosted code.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands:
     *   Stack: index => arguments[index]
     */ \
    MACRO(GetActualArg, get_actual_arg, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Push the value of an aliased binding.
     *
     * Local bindings that aren't closed over or dynamically accessed are
     * stored in stack slots. Global and `with` bindings are object properties.
     * All other bindings are called "aliased" and stored in
     * `EnvironmentObject`s.
     *
     * Where possible, `Aliased` instructions are used to access aliased
     * bindings.  (There's no difference in meaning between `AliasedVar` and
     * `AliasedLexical`.) Each of these instructions has operands `hops` and
     * `slot` that encode an [`EnvironmentCoordinate`][1], directions to the
     * binding from the current environment object.
     *
     * `Aliased` instructions can't be used when there's a dynamic scope (due
     * to non-strict `eval` or `with`) that might shadow the aliased binding.
     *
     * [1]: https://searchfox.org/mozilla-central/search?q=symbol:T_js%3A%3AEnvironmentCoordinate
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: => aliasedVar
     */ \
    MACRO(GetAliasedVar, get_aliased_var, NULL, 5, 0, 1, JOF_ENVCOORD|JOF_NAME|JOF_USES_ENV) \
    /*
     * Push the value of an aliased binding, which may have to bypass a DebugEnvironmentProxy
     * on the environment chain.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: => aliasedVar
     */ \
    MACRO(GetAliasedDebugVar, get_aliased_debug_var, NULL, 5, 0, 1, JOF_DEBUGCOORD|JOF_NAME) \
    /*
     * Get the value of a module import by name and pushes it onto the stack.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: => val
     */ \
    MACRO(GetImport, get_import, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME) \
    /*
     * Get the value of a binding from the environment `env`. If the name is
     * not bound in `env`, throw a ReferenceError.
     *
     * `env` must be an environment currently on the environment chain, pushed
     * by `JSOp::BindName` or `JSOp::BindVar`.
     *
     * Note: `JSOp::BindName` and `JSOp::GetBoundName` are the two halves of the
     * `JSOp::GetName` operation: finding and reading a variable. This
     * decomposed version is needed to implement the compound assignment and
     * increment/decrement operators, which get and then set a variable. The
     * spec says the variable lookup is done only once. If we did the lookup
     * twice, there would be observable bugs, thanks to dynamic scoping. We
     * could set the wrong variable or call proxy traps incorrectly.
     *
     * Implements: [GetValue][1] steps 4 and 6.
     *
     * [1]: https://tc39.es/ecma262/#sec-getvalue
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: env => v
     */ \
    MACRO(GetBoundName, get_bound_name, NULL, 5, 1, 1, JOF_ATOM|JOF_NAME|JOF_IC) \
    /*
     * Push the value of an intrinsic onto the stack.
     *
     * Non-standard. Intrinsics are slots in the intrinsics holder object (see
     * `GlobalObject::getIntrinsicsHolder`), which is used in lieu of global
     * bindings in self-hosting code.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: => intrinsic[name]
     */ \
    MACRO(GetIntrinsic, get_intrinsic, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME|JOF_IC) \
    /*
     * Pushes the currently executing function onto the stack.
     *
     * The current script must be a function script.
     *
     * Used to implement `super`. This is also used sometimes as a minor
     * optimization when a named function expression refers to itself by name:
     *
     *     f = function fac(n) {  ... fac(n - 1) ... };
     *
     * This lets us optimize away a lexical environment that contains only the
     * binding for `fac`, unless it's otherwise observable (via `with`, `eval`,
     * or a nested closure).
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands:
     *   Stack: => callee
     */ \
    MACRO(Callee, callee, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Load the callee stored in a CallObject on the environment chain. The
     * `numHops` operand is the number of environment objects to skip on the
     * environment chain. The environment chain element indicated by `numHops`
     * must be a CallObject.
     *
     *   Category: Variables and scopes
     *   Type: Getting binding values
     *   Operands: uint8_t numHops
     *   Stack: => callee
     */ \
    MACRO(EnvCallee, env_callee, NULL, 2, 0, 1, JOF_UINT8) \
    /*
     * Assign `val` to the binding in `env` with the name given by `nameIndex`.
     * Throw a ReferenceError if the binding is an uninitialized lexical.
     * This can call setters and/or proxy traps.
     *
     * `env` must be an environment currently on the environment chain,
     * pushed by `JSOp::BindName` or `JSOp::BindVar`.
     *
     * This is the fallback `Set` instruction that handles all unoptimized
     * cases. Optimized instructions follow.
     *
     * Implements: [PutValue][1] steps 5 and 7 for unoptimized bindings.
     *
     * Note: `JSOp::BindName` and `JSOp::SetName` are the two halves of simple
     * assignment: finding and setting a variable. They are two separate
     * instructions because, per spec, the "finding" part happens before
     * evaluating the right-hand side of the assignment, and the "setting" part
     * after. Optimized cases don't need a `Bind` instruction because the
     * "finding" is done statically.
     *
     * [1]: https://tc39.es/ecma262/#sec-putvalue
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    MACRO(SetName, set_name, NULL, 5, 2, 1, JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_CHECKSLOPPY|JOF_IC|JOF_USES_ENV) \
    /*
     * Like `JSOp::SetName`, but throw a TypeError if there is no binding for
     * the specified name in `env`, or if the binding is immutable (a `const`
     * or read-only property).
     *
     * Implements: [PutValue][1] steps 5 and 7 for strict mode code.
     *
     * [1]: https://tc39.es/ecma262/#sec-putvalue
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    MACRO(StrictSetName, strict_set_name, NULL, 5, 2, 1, JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_CHECKSTRICT|JOF_IC|JOF_USES_ENV) \
    /*
     * Like `JSOp::SetName`, but for assigning to globals. `env` must be an
     * environment pushed by `JSOp::BindGName`.
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    MACRO(SetGName, set_g_name, NULL, 5, 2, 1, JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_GNAME|JOF_CHECKSLOPPY|JOF_IC) \
    /*
     * Like `JSOp::StrictSetGName`, but for assigning to globals. `env` must be
     * an environment pushed by `JSOp::BindGName`.
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: env, val => val
     */ \
    MACRO(StrictSetGName, strict_set_g_name, NULL, 5, 2, 1, JOF_ATOM|JOF_NAME|JOF_PROPSET|JOF_GNAME|JOF_CHECKSTRICT|JOF_IC) \
    /*
     * Assign `val` to an argument binding that's stored in the stack frame or
     * in an `ArgumentsObject`.
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint16_t argno
     *   Stack: val => val
     */ \
    MACRO(SetArg, set_arg, NULL, 3, 1, 1, JOF_QARG|JOF_NAME) \
    /*
     * Assign to an optimized local binding.
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint24_t localno
     *   Stack: v => v
     */ \
    MACRO(SetLocal, set_local, NULL, 4, 1, 1, JOF_LOCAL|JOF_NAME) \
    /*
     * Assign to an aliased binding.
     *
     * Implements: [SetMutableBinding for declarative Environment Records][1],
     * in certain cases where it's known that the binding exists, is mutable,
     * and has been initialized.
     *
     * [1]: https://tc39.es/ecma262/#sec-declarative-environment-records-setmutablebinding-n-v-s
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint8_t hops, uint24_t slot
     *   Stack: val => val
     */ \
    MACRO(SetAliasedVar, set_aliased_var, NULL, 5, 1, 1, JOF_ENVCOORD|JOF_NAME|JOF_PROPSET|JOF_USES_ENV) \
    /*
     * Assign to an intrinsic.
     *
     * Nonstandard. Intrinsics are used in lieu of global bindings in self-
     * hosted code. The value is actually stored in the intrinsics holder
     * object, `GlobalObject::getIntrinsicsHolder`. (Self-hosted code doesn't
     * have many global `var`s, but it has many `function`s.)
     *
     *   Category: Variables and scopes
     *   Type: Setting binding values
     *   Operands: uint32_t nameIndex
     *   Stack: val => val
     */ \
    MACRO(SetIntrinsic, set_intrinsic, NULL, 5, 1, 1, JOF_ATOM|JOF_NAME) \
    /*
     * Push a lexical environment onto the environment chain.
     *
     * The `LexicalScope` indicated by `lexicalScopeIndex` determines the shape
     * of the new `BlockLexicalEnvironmentObject`. All bindings in the new
     * environment are marked as uninitialized.
     *
     * Implements: [Evaluation of *Block*][1], steps 1-4.
     *
     * #### Fine print for environment chain instructions
     *
     * The following rules for `JSOp::{Push,Pop}LexicalEnv` also apply to
     * `JSOp::PushClassBodyEnv`, `JSOp::PushVarEnv`, and
     * `JSOp::{Enter,Leave}With`.
     *
     * Each `JSOp::PopLexicalEnv` instruction matches a particular
     * `JSOp::PushLexicalEnv` instruction in the same script and must have the
     * same scope and stack depth as the instruction immediately after that
     * `PushLexicalEnv`.
     *
     * `JSOp::PushLexicalEnv` enters a scope that extends to some set of
     * instructions in the script. Code must not jump into or out of this
     * region: control can enter only by executing `PushLexicalEnv` and can
     * exit only by executing a `PopLexicalEnv` or by exception unwinding. (A
     * `JSOp::PopLexicalEnv` is always emitted at the end of the block, and
     * extra copies are emitted on "exit slides", where a `break`, `continue`,
     * or `return` statement exits the scope.)
     *
     * The script's `JSScript::scopeNotes()` must identify exactly which
     * instructions begin executing in this scope. Typically this means a
     * single entry marking the contiguous chunk of bytecode from the
     * instruction after `JSOp::PushLexicalEnv` to `JSOp::PopLexicalEnv`
     * (inclusive); but if that range contains any instructions on exit slides,
     * after a `JSOp::PopLexicalEnv`, then those must be correctly noted as
     * *outside* the scope.
     *
     * [1]: https://tc39.es/ecma262/#sec-block-runtime-semantics-evaluation
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands: uint32_t lexicalScopeIndex
     *   Stack: =>
     */ \
    MACRO(PushLexicalEnv, push_lexical_env, NULL, 5, 0, 0, JOF_SCOPE|JOF_USES_ENV) \
    /*
     * Pop a lexical or class-body environment from the environment chain.
     *
     * See `JSOp::PushLexicalEnv` for the fine print.
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(PopLexicalEnv, pop_lexical_env, NULL, 1, 0, 0, JOF_BYTE|JOF_USES_ENV) \
    /*
     * No-op instruction that indicates leaving an optimized lexical scope.
     *
     * If all bindings in a lexical scope are optimized into stack slots, then
     * the runtime environment objects for that scope are optimized away. No
     * `JSOp::{Push,Pop}LexicalEnv` instructions are emitted. However, the
     * debugger still needs to be notified when control exits a scope; that's
     * what this instruction does.
     *
     * The last instruction in a lexical or class-body scope, as indicated by
     * scope notes, must be either this instruction (if the scope is optimized)
     * or `JSOp::PopLexicalEnv` (if not).
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(DebugLeaveLexicalEnv, debug_leave_lexical_env, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Replace the current block on the environment chain with a fresh block
     * with uninitialized bindings. This implements the behavior of inducing a
     * fresh lexical environment for every iteration of a for-in/of loop whose
     * loop-head declares lexical variables that may be captured.
     *
     * The current environment must be a BlockLexicalEnvironmentObject.
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands: uint32_t lexicalScopeIndex
     *   Stack: =>
     */ \
    MACRO(RecreateLexicalEnv, recreate_lexical_env, NULL, 5, 0, 0, JOF_SCOPE) \
    /*
     * Like `JSOp::RecreateLexicalEnv`, but the values of all the bindings are
     * copied from the old block to the new one. This is used for C-style
     * `for(let ...; ...; ...)` loops.
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands: uint32_t lexicalScopeIndex
     *   Stack: =>
     */ \
    MACRO(FreshenLexicalEnv, freshen_lexical_env, NULL, 5, 0, 0, JOF_SCOPE) \
    /*
     * Push a ClassBody environment onto the environment chain.
     *
     * Like `JSOp::PushLexicalEnv`, but pushes a `ClassBodyEnvironmentObject`
     * rather than a `BlockLexicalEnvironmentObject`.  `JSOp::PopLexicalEnv` is
     * used to pop class-body environments as well as lexical environments.
     *
     * See `JSOp::PushLexicalEnv` for the fine print.
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands: uint32_t lexicalScopeIndex
     *   Stack: =>
     */ \
    MACRO(PushClassBodyEnv, push_class_body_env, NULL, 5, 0, 0, JOF_SCOPE) \
    /*
     * Push a var environment onto the environment chain.
     *
     * Like `JSOp::PushLexicalEnv`, but pushes a `VarEnvironmentObject` rather
     * than a `BlockLexicalEnvironmentObject`. The difference is that
     * non-strict direct `eval` can add bindings to a var environment; see
     * `VarScope` in Scope.h.
     *
     * See `JSOp::PushLexicalEnv` for the fine print.
     *
     * There is no corresponding `JSOp::PopVarEnv` operation, because a
     * `VarEnvironmentObject` is never popped from the environment chain.
     *
     * Implements: Places in the spec where the VariableEnvironment is set:
     *
     * -   The bit in [PerformEval][1] where, in strict direct eval, the new
     *     eval scope is taken as *varEnv* and becomes "*runningContext*'s
     *     VariableEnvironment".
     *
     * -   The weird scoping rules for functions with default parameter
     *     expressions, as specified in [FunctionDeclarationInstantiation][2]
     *     step 28 ("NOTE: A separate Environment Record is needed...").
     *
     * Note: The spec also pushes a new VariableEnvironment on entry to every
     * function, but the VM takes care of that as part of pushing the stack
     * frame, before the function script starts to run, so `JSOp::PushVarEnv` is
     * not needed.
     *
     * [1]: https://tc39.es/ecma262/#sec-performeval
     * [2]: https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands: uint32_t scopeIndex
     *   Stack: =>
     */ \
    MACRO(PushVarEnv, push_var_env, NULL, 5, 0, 0, JOF_SCOPE|JOF_USES_ENV) \
    /*
     * Push a `WithEnvironmentObject` wrapping ToObject(`val`) to the
     * environment chain.
     *
     * Implements: [Evaluation of `with` statements][1], steps 2-6.
     *
     * Operations that may need to consult a WithEnvironment can't be correctly
     * implemented using optimized instructions like `JSOp::GetLocal`. A script
     * must use the deoptimized `JSOp::GetName`, `BindName`, `SetName`, and
     * `DelName` instead. Since those instructions don't work correctly with
     * optimized locals and arguments, all bindings in scopes enclosing a
     * `with` statement are marked as "aliased" and deoptimized too.
     *
     * See `JSOp::PushLexicalEnv` for the fine print.
     *
     * [1]: https://tc39.es/ecma262/#sec-with-statement-runtime-semantics-evaluation
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands: uint32_t staticWithIndex
     *   Stack: val =>
     */ \
    MACRO(EnterWith, enter_with, NULL, 5, 1, 0, JOF_SCOPE) \
    /*
     * Pop a `WithEnvironmentObject` from the environment chain.
     *
     * See `JSOp::PushLexicalEnv` for the fine print.
     *
     * Implements: [Evaluation of `with` statements][1], step 8.
     *
     * [1]: https://tc39.es/ecma262/#sec-with-statement-runtime-semantics-evaluation
     *
     *   Category: Variables and scopes
     *   Type: Entering and leaving environments
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(LeaveWith, leave_with, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Push the current VariableEnvironment (the environment on the environment
     * chain designated to receive new variables).
     *
     * Implements: [Annex B.3.3.1, changes to FunctionDeclarationInstantiation
     * for block-level functions][1], step 1.a.ii.3.a, and similar steps in
     * other Annex B.3.3 algorithms, when setting the function's second binding
     * can't be optimized.
     *
     * [1]: https://tc39.es/ecma262/#sec-web-compat-functiondeclarationinstantiation
     *
     *   Category: Variables and scopes
     *   Type: Creating and deleting bindings
     *   Operands:
     *   Stack: => env
     */ \
    MACRO(BindVar, bind_var, NULL, 1, 0, 1, JOF_BYTE|JOF_USES_ENV) \
    /*
     * Check for conflicting bindings and then initialize them in global or
     * sloppy eval scripts. This is required for global scripts with any
     * top-level bindings, or any sloppy-eval scripts with any non-lexical
     * top-level bindings.
     *
     * Implements: [GlobalDeclarationInstantiation][1] and
     *             [EvalDeclarationInstantiation][2] (except step 12).
     *
     * The `lastFun` argument is a GCThingIndex of the last hoisted top-level
     * function that is part of top-level script initialization. The gcthings
     * from index `0` thru `lastFun` contain only scopes and hoisted functions.
     *
     * [1]: https://tc39.es/ecma262/#sec-globaldeclarationinstantiation
     * [2]: https://tc39.es/ecma262/#sec-evaldeclarationinstantiation
     *
     *   Category: Variables and scopes
     *   Type: Creating and deleting bindings
     *   Operands: uint32_t lastFun
     *   Stack: =>
     */ \
    MACRO(GlobalOrEvalDeclInstantiation, global_or_eval_decl_instantiation, NULL, 5, 0, 0, JOF_GCTHING|JOF_USES_ENV) \
    /*
     * Look up a variable on the environment chain and delete it. Push `true`
     * on success (if a binding was deleted, or if no such binding existed in
     * the first place), `false` otherwise (most kinds of bindings can't be
     * deleted).
     *
     * Implements: [`delete` *Identifier*][1], which [is a SyntaxError][2] in
     * strict mode code.
     *
     *    [1]: https://tc39.es/ecma262/#sec-delete-operator-runtime-semantics-evaluation
     *    [2]: https://tc39.es/ecma262/#sec-delete-operator-static-semantics-early-errors
     *
     *   Category: Variables and scopes
     *   Type: Creating and deleting bindings
     *   Operands: uint32_t nameIndex
     *   Stack: => succeeded
     */ \
    MACRO(DelName, del_name, NULL, 5, 0, 1, JOF_ATOM|JOF_NAME|JOF_CHECKSLOPPY|JOF_USES_ENV) \
    /*
     * Create and push the `arguments` object for the current function activation.
     *
     * When it exists, `arguments` is stored in an ordinary local variable.
     * `JSOp::Arguments` is used in function preludes, to populate that variable
     * before the function body runs, *not* each time `arguments` appears in a
     * function.
     *
     * If a function clearly doesn't use `arguments`, we optimize it away when
     * emitting bytecode. The function's script won't use `JSOp::Arguments` at
     * all.
     *
     * The current script must be a function script. This instruction must
     * execute at most once per function activation.
     *
     *   Category: Variables and scopes
     *   Type: Function environment setup
     *   Operands:
     *   Stack: => arguments
     */ \
    MACRO(Arguments, arguments, NULL, 1, 0, 1, JOF_BYTE|JOF_USES_ENV) \
    /*
     * Create and push the rest parameter array for current function call.
     *
     * This must appear only in a script for a function that has a rest
     * parameter.
     *
     *   Category: Variables and scopes
     *   Type: Function environment setup
     *   Operands:
     *   Stack: => rest
     */ \
    MACRO(Rest, rest, NULL, 1, 0, 1, JOF_BYTE|JOF_IC) \
    /*
     * Determines the `this` value for current function frame and pushes it
     * onto the stack.
     *
     * In functions, `this` is stored in a local variable. This instruction is
     * used in the function prologue to get the value to initialize that
     * variable.  (This doesn't apply to arrow functions, becauses they don't
     * have a `this` binding; also, `this` is optimized away if it's unused.)
     *
     * Functions that have a `this` binding have a local variable named
     * `".this"`, which is initialized using this instruction in the function
     * prologue.
     *
     * In non-strict functions, `this` is always an object. Undefined/null
     * `this` is converted into the global `this` value. Other primitive values
     * are boxed. See `js::BoxNonStrictThis`.
     *
     *   Category: Variables and scopes
     *   Type: Function environment setup
     *   Operands:
     *   Stack: => this
     */ \
    MACRO(FunctionThis, function_this, NULL, 1, 0, 1, JOF_BYTE) \
    /*
     * Pop the top value from the stack and discard it.
     *
     *   Category: Stack operations
     *   Operands:
     *   Stack: v =>
     */ \
    MACRO(Pop, pop, NULL, 1, 1, 0, JOF_BYTE) \
    /*
     * Pop the top `n` values from the stack. `n` must be <= the current stack
     * depth.
     *
     *   Category: Stack operations
     *   Operands: uint16_t n
     *   Stack: v[n-1], ..., v[1], v[0] =>
     */ \
    MACRO(PopN, pop_n, NULL, 3, -1, 0, JOF_UINT16) \
    /*
     * Push a copy of the top value on the stack.
     *
     *   Category: Stack operations
     *   Operands:
     *   Stack: v => v, v
     */ \
    MACRO(Dup, dup, NULL, 1, 1, 2, JOF_BYTE) \
    /*
     * Duplicate the top two values on the stack.
     *
     *   Category: Stack operations
     *   Operands:
     *   Stack: v1, v2 => v1, v2, v1, v2
     */ \
    MACRO(Dup2, dup2, NULL, 1, 2, 4, JOF_BYTE) \
    /*
     * Push a copy of the nth value from the top of the stack.
     *
     * `n` must be less than the current stack depth.
     *
     *   Category: Stack operations
     *   Operands: uint24_t n
     *   Stack: v[n], v[n-1], ..., v[1], v[0] =>
     *          v[n], v[n-1], ..., v[1], v[0], v[n]
     */ \
    MACRO(DupAt, dup_at, NULL, 4, 0, 1, JOF_UINT24) \
    /*
     * Swap the top two values on the stack.
     *
     *   Category: Stack operations
     *   Operands:
     *   Stack: v1, v2 => v2, v1
     */ \
    MACRO(Swap, swap, NULL, 1, 2, 2, JOF_BYTE) \
    /*
     * Pick the nth element from the stack and move it to the top of the stack.
     *
     *   Category: Stack operations
     *   Operands: uint8_t n
     *   Stack: v[n], v[n-1], ..., v[1], v[0] => v[n-1], ..., v[1], v[0], v[n]
     */ \
    MACRO(Pick, pick, NULL, 2, 0, 0, JOF_UINT8) \
    /*
     * Move the top of the stack value under the `n`th element of the stack.
     * `n` must not be 0.
     *
     *   Category: Stack operations
     *   Operands: uint8_t n
     *   Stack: v[n], v[n-1], ..., v[1], v[0] => v[0], v[n], v[n-1], ..., v[1]
     */ \
    MACRO(Unpick, unpick, NULL, 2, 0, 0, JOF_UINT8) \
    /*
     * Do nothing. This is used when we need distinct bytecode locations for
     * various mechanisms.
     *
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(Nop, nop, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * No-op instruction emitted immediately after `JSOp::*Eval` so that direct
     * eval does not have to do slow pc-to-line mapping.
     *
     * The `lineno` operand should agree with this script's source notes about
     * the line number of the preceding `*Eval` instruction.
     *
     *   Category: Other
     *   Operands: uint32_t lineno
     *   Stack: =>
     */ \
    MACRO(Lineno, lineno, NULL, 5, 0, 0, JOF_UINT32) \
    /*
     * No-op instruction to hint that the top stack value is uninteresting.
     *
     * This affects only debug output and some error messages.
     * In array destructuring, we emit bytecode that is roughly equivalent to
     * `result.done ? undefined : result.value`.
     * `NopDestructuring` is emitted after the `undefined`, so that the
     * expression decompiler and disassembler know to casually ignore the
     * possibility of `undefined`, and render the result of the conditional
     * expression simply as "`result.value`".
     *
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(NopDestructuring, nop_destructuring, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * No-op instruction only emitted in some self-hosted functions. Not
     * handled by the JITs or Baseline Interpreter so the script always runs in
     * the C++ interpreter.
     *
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(ForceInterpreter, force_interpreter, NULL, 1, 0, 0, JOF_BYTE) \
    /*
     * Examine the top stack value, asserting that it's either a self-hosted
     * function or a self-hosted intrinsic. This does nothing in a non-debug
     * build.
     *
     *   Category: Other
     *   Operands:
     *   Stack: checkVal => checkVal
     */ \
    MACRO(DebugCheckSelfHosted, debug_check_self_hosted, NULL, 1, 1, 1, JOF_BYTE) \
    /*
     * Break in the debugger, if one is attached. Otherwise this is a no-op.
     *
     * The [`Debugger` API][1] offers a way to hook into this instruction.
     *
     * Implements: [Evaluation for *DebuggerStatement*][2].
     *
     * [1]: https://developer.mozilla.org/en-US/docs/Tools/Debugger-API/Debugger
     * [2]: https://tc39.es/ecma262/#sec-debugger-statement-runtime-semantics-evaluation
     *
     *   Category: Other
     *   Operands:
     *   Stack: =>
     */ \
    MACRO(Debugger, debugger, NULL, 1, 0, 0, JOF_BYTE)

// clang-format on

/*
 * In certain circumstances it may be useful to "pad out" the opcode space to
 * a power of two.  Use this macro to do so.
 */
#define FOR_EACH_TRAILING_UNUSED_OPCODE(MACRO) \
  IF_RECORD_TUPLE(/* empty */, MACRO(230))     \
  IF_RECORD_TUPLE(/* empty */, MACRO(231))     \
  IF_RECORD_TUPLE(/* empty */, MACRO(232))     \
  IF_RECORD_TUPLE(/* empty */, MACRO(233))     \
  IF_RECORD_TUPLE(/* empty */, MACRO(234))     \
  IF_RECORD_TUPLE(/* empty */, MACRO(235))     \
  IF_RECORD_TUPLE(/* empty */, MACRO(236))     \
  MACRO(237)                                   \
  MACRO(238)                                   \
  MACRO(239)                                   \
  MACRO(240)                                   \
  MACRO(241)                                   \
  MACRO(242)                                   \
  MACRO(243)                                   \
  MACRO(244)                                   \
  MACRO(245)                                   \
  MACRO(246)                                   \
  MACRO(247)                                   \
  MACRO(248)                                   \
  MACRO(249)                                   \
  MACRO(250)                                   \
  MACRO(251)                                   \
  MACRO(252)                                   \
  MACRO(253)                                   \
  MACRO(254)                                   \
  MACRO(255)

namespace js {

// Sanity check that opcode values and trailing unused opcodes completely cover
// the [0, 256) range.  Avert your eyes!  You don't want to know how the
// sausage gets made.

// clang-format off
#define PLUS_ONE(...) \
    + 1
constexpr int JSOP_LIMIT = 0 FOR_EACH_OPCODE(PLUS_ONE);
#undef PLUS_ONE

#define TRAILING_VALUE_AND_VALUE_PLUS_ONE(val) \
    val) && (val + 1 ==
static_assert((JSOP_LIMIT ==
               FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_VALUE_AND_VALUE_PLUS_ONE)
               256),
              "trailing unused opcode values monotonically increase "
              "from JSOP_LIMIT to 255");
#undef TRAILING_VALUE_AND_VALUE_PLUS_ONE
// clang-format on

// Define JSOpLength_* constants for all ops.
#define DEFINE_LENGTH_CONSTANT(op, op_snake, image, len, ...) \
  constexpr size_t JSOpLength_##op = len;
FOR_EACH_OPCODE(DEFINE_LENGTH_CONSTANT)
#undef DEFINE_LENGTH_CONSTANT

}  // namespace js

/*
 * JS operation bytecodes.
 */
enum class JSOp : uint8_t {
#define ENUMERATE_OPCODE(op, ...) op,
  FOR_EACH_OPCODE(ENUMERATE_OPCODE)
#undef ENUMERATE_OPCODE
};

#endif  // vm_Opcodes_h
