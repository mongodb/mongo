/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StencilEnums_h
#define vm_StencilEnums_h

#include <stdint.h>  // uint8_t

//
// Enum definitions shared between frontend, stencil, and the VM.
//

namespace js {

// [SMDOC] Try Notes
//
// Trynotes are attached to regions that are involved with
// exception unwinding. They can be broken up into four categories:
//
// 1. Catch and Finally: Basic exception handling. A Catch trynote
//    covers the range of the associated try. A Finally trynote covers
//    the try and the catch.
//
// 2. ForIn and Destructuring: These operations create an iterator
//    which must be cleaned up (by calling IteratorClose) during
//    exception unwinding.
//
// 3. ForOf and ForOfIterclose: For-of loops handle unwinding using
//    catch blocks. These trynotes are used for for-of breaks/returns,
//    which create regions that are lexically within a for-of block,
//    but logically outside of it. See TryNoteIter::settle for more
//    details.
//
// 4. Loop: This represents normal for/while/do-while loops. It is
//    unnecessary for exception unwinding, but storing the boundaries
//    of loops here is helpful for heuristics that need to know
//    whether a given op is inside a loop.
enum class TryNoteKind : uint8_t {
  Catch,
  Finally,
  ForIn,
  Destructuring,
  ForOf,
  ForOfIterClose,
  Loop
};

// [SMDOC] Script Flags
//
// Interpreted scripts represented by the BaseScript type use two flag words to
// encode an assortment of conditions and attributes about the script.
//
// The "immutable" flags are a combination of input flags describing aspects of
// the execution context that affect parsing (such as if we are an ES module or
// normal script), and flags derived from source text. These flags are preserved
// during cloning and serializing. As well, they should never change after the
// BaseScript is created (although there are currently a few exceptions for
// de-/re-lazification that remain).
//
// The "mutable" flags are temporary flags that are used by subsystems in the
// engine such as the debugger or JITs. These flags are not preserved through
// serialization or cloning since the attributes are generally associated with
// one specific instance of a BaseScript.

enum class ImmutableScriptFlagsEnum : uint32_t {
  // Input Flags
  //
  // These flags are from CompileOptions or the Parser entry point. They
  // generally cannot be derived from the source text alone.
  // ----

  // A script may have one of the following kinds: Global, Eval, Module,
  // Function. At most one flag can be set, with a default of Global.
  IsForEval = 1 << 0,
  IsModule = 1 << 1,
  IsFunction = 1 << 2,

  // The script is compiled as engine-internal self-hosted JavaScript. This mode
  // is used to implement certain library functions and has special parse,
  // bytecode, and runtime behaviour that differs from normal script.
  SelfHosted = 1 << 3,

  // The script was compiled with the default mode set to strict mode. Note that
  // this tracks the default value, while the actual mode used (after processing
  // source and its directives) is the `Strict` flag below.
  ForceStrict = 1 << 4,

  // The script has a non-syntactic scope on its environment chain. That is,
  // there may be objects about which we know nothing between the outermost
  // syntactic scope and the global.
  HasNonSyntacticScope = 1 << 5,

  // The script return value will not be used and simplified code will be
  // generated. This can only be applied to top-level scripts. The value this
  // script returns will be UndefinedValue instead of what the spec normally
  // prescribes.
  NoScriptRval = 1 << 6,

  // TreatAsRunOnce roughly indicates that a script is expected to be run no
  // more than once. This affects optimizations and heuristics.
  //
  // On top-level global/eval/module scripts, this is set when the embedding
  // ensures this script will not be re-used. In this case, parser literals may
  // be exposed directly instead of being cloned.
  TreatAsRunOnce = 1 << 7,
  // ----

  // Parser Flags
  //
  // Flags computed by the Parser from the source text and input flags.
  // ----

  // Generated code will execute in strict mode. This is due to either the
  // ForceStrict flag being specified above, or due to source text itself (such
  // as "use strict" directives).
  Strict = 1 << 8,

  // Script is parsed with a top-level goal of Module. This may be a top-level
  // or an inner-function script.
  HasModuleGoal = 1 << 9,

  // Script contains inner functions.
  //
  // Note: This prevents relazification since inner function close-over the
  // current scripts scopes.
  HasInnerFunctions = 1 << 10,

  // There is a direct eval statement in this script OR in any of its inner
  // functions.
  //
  // Note: This prevents relazification since it can introduce inner functions.
  HasDirectEval = 1 << 11,

  // The (static) bindings of this script must support dynamic name access for
  // read/write. The environment chain is used to do these dynamic lookups and
  // optimizations to avoid allocating environments are suppressed.
  //
  // This includes direct-eval, `with`, and `delete` in this script OR in any of
  // its inner functions.
  //
  // Note: Access through the arguments object is not considered dynamic binding
  // access since it does not go through the normal name lookup mechanism.
  BindingsAccessedDynamically = 1 << 12,

  // A tagged template exists in the body (which will use JSOp::CallSiteObj in
  // bytecode).
  //
  // Note: This prevents relazification since the template's object is
  // observable to the user and cannot be recreated.
  HasCallSiteObj = 1 << 13,

  // Parser Flags for Functions
  // ----

  // This function's initial prototype is one of Function, GeneratorFunction,
  // AsyncFunction, or AsyncGeneratorFunction as indicated by these flags.
  //
  // If either of these flags is set, the script may suspend and resume as it
  // executes. Stack frames for this script also have a generator object.
  IsAsync = 1 << 14,
  IsGenerator = 1 << 15,

  // This function's body serves as the `var` environment for a non-strict
  // direct eval. This matters because it's the only way bindings can be
  // dynamically added to a local environment, possibly shadowing other
  // variables.
  FunHasExtensibleScope = 1 << 16,

  // This function has an internal .this binding and we need to emit
  // JSOp::FunctionThis in the prologue to initialize it. This binding may be
  // used directly for "this", or indirectly (such as class constructors).
  FunctionHasThisBinding = 1 << 17,

  // This function is a class method that must uses an internal [[HomeObject]]
  // slot. This slot is initialized when the class definition is executed in the
  // enclosing function.
  NeedsHomeObject = 1 << 18,

  // This function is a constructor for a derived class. This is a class that
  // uses the `extends` syntax.
  IsDerivedClassConstructor = 1 << 19,

  // This function is synthesized by the Parser. This is used for field
  // initializer lambdas and missing constructors for classes. These functions
  // have unusual source coordinates and may be hidden from things like
  // Reflect.parse.
  IsSyntheticFunction = 1 << 20,

  // This function is a class constructor that has MemberInitializer data
  // associated with it.
  UseMemberInitializers = 1 << 21,

  // This function has a rest (`...`) parameter.
  HasRest = 1 << 22,

  // This function needs a call object or named lambda environment to be created
  // in order to execute the function. This is done in the Stack or JIT frame
  // setup code _before_ the bytecode prologue starts.
  NeedsFunctionEnvironmentObjects = 1 << 23,

  // An extra VarScope is used as the body scope instead of the normal
  // FunctionScope. This is needed when parameter expressions are used AND the
  // function has var bindings or a sloppy-direct-eval. For example,
  //    `function(x = eval("")) { var y; }`
  FunctionHasExtraBodyVarScope = 1 << 24,

  // This function must define the implicit `arguments` binding on the function
  // scope. If there are no free uses or an appropriate explicit binding exists,
  // then this flag is unset.
  //
  // Note: Parameter expressions will not see an explicit `var arguments;`
  // binding in the body and an implicit binding on the function-scope must
  // still be used in that case.
  ShouldDeclareArguments = 1 << 25,

  // This function has a local (implicit or explicit) `arguments` binding. This
  // binding is initialized by the JSOp::Arguments bytecode.
  //
  // Technically, every function has a binding named `arguments`. Internally,
  // this binding is only added when `arguments` is mentioned by the function
  // body.
  //
  // Examples:
  //   ```
  //    // Explicit definition
  //    function f() { var arguments; return arguments; }
  //
  //    // Implicit use
  //    function f() { return arguments; }
  //
  //    // Implicit use in arrow function
  //    function f() { return () => arguments; }
  //
  //    // Implicit use in parameter expression
  //    function f(a = arguments) { return a; }
  //   ```
  NeedsArgsObj = 1 << 26,

  // This function must use the "mapped" form of an arguments object. This flag
  // is set independently of whether we actually use an `arguments` binding. The
  // conditions are specified in the ECMAScript spec.
  HasMappedArgsObj = 1 << 27,

  // Large self-hosted methods that should be inlined anyway by the JIT for
  // performance reasons can be marked with this flag.
  IsInlinableLargeFunction = 1 << 28,
};

enum class MutableScriptFlagsEnum : uint32_t {
  // Number of times the |warmUpCount| was forcibly discarded. The counter is
  // reset when a script is successfully jit-compiled.
  WarmupResets_MASK = 0xFF,

  // If treatAsRunOnce, whether script has executed.
  HasRunOnce = 1 << 8,

  // Script has been reused for a clone.
  HasBeenCloned = 1 << 9,

  // Script has an entry in Realm::scriptCountsMap.
  HasScriptCounts = 1 << 10,

  // Script has an entry in Realm::debugScriptMap.
  HasDebugScript = 1 << 11,

  // (1 << 12) is unused.
  // (1 << 13) is unused.

  // Script supports relazification where it releases bytecode and gcthings to
  // save memory. This process is opt-in since various complexities may disallow
  // this for some scripts.
  // NOTE: Must check for isRelazifiable() before setting this flag.
  AllowRelazify = 1 << 14,

  // Set if the script has opted into spew.
  SpewEnabled = 1 << 15,

  // Set if we care about a script's final warmup count.
  NeedsFinalWarmUpCount = 1 << 16,

  //
  // IonMonkey compilation hints.
  //

  // Whether Baseline or Ion compilation has been disabled for this script.
  // IonDisabled is equivalent to |jitScript->canIonCompile() == false| but
  // JitScript can be discarded on GC and we don't want this to affect
  // observable behavior (see ArgumentsGetterImpl comment).
  BaselineDisabled = 1 << 17,
  IonDisabled = 1 << 18,

  // This script should not be inlined into others. This happens after inlining
  // has failed.
  Uninlineable = 1 << 19,

  // (1 << 20) is unused.

  // *****************************************************************
  // The flags below are set when we bail out and invalidate a script.
  // When we recompile, we will be more conservative.
  // *****************************************************************

  // A hoisted bounds check bailed out.
  FailedBoundsCheck = 1 << 21,

  // An instruction hoisted by LICM bailed out.
  HadLICMInvalidation = 1 << 22,

  // An instruction hoisted by InstructionReordering bailed out.
  HadReorderingBailout = 1 << 23,

  // An instruction inserted or truncated by Range Analysis bailed out.
  HadEagerTruncationBailout = 1 << 24,

  // A lexical check bailed out.
  FailedLexicalCheck = 1 << 25,

  // A guard inserted by phi specialization bailed out.
  HadSpeculativePhiBailout = 1 << 26,

  // An unbox folded with a load bailed out.
  HadUnboxFoldingBailout = 1 << 27,
};

}  // namespace js

#endif /* vm_StencilEnums_h */
