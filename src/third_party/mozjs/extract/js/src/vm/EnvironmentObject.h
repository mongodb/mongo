/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_EnvironmentObject_h
#define vm_EnvironmentObject_h

#include "builtin/ModuleObject.h"
#include "frontend/NameAnalysisTypes.h"
#include "gc/Barrier.h"
#include "gc/WeakMap.h"
#include "js/GCHashTable.h"
#include "vm/ArgumentsObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/ProxyObject.h"
#include "vm/Scope.h"

namespace js {

class ModuleObject;
typedef Handle<ModuleObject*> HandleModuleObject;

/*
 * Return a shape representing the static scope containing the variable
 * accessed by the ALIASEDVAR op at 'pc'.
 */
extern Shape*
EnvironmentCoordinateToEnvironmentShape(JSScript* script, jsbytecode* pc);

/* Return the name being accessed by the given ALIASEDVAR op. */
extern PropertyName*
EnvironmentCoordinateName(EnvironmentCoordinateNameCache& cache, JSScript* script, jsbytecode* pc);

/* Return the function script accessed by the given ALIASEDVAR op, or nullptr. */
extern JSScript*
EnvironmentCoordinateFunctionScript(JSScript* script, jsbytecode* pc);


/*** Environment objects *****************************************************/


/*** Environment objects *****************************************************/

/*
 * About environments
 * ------------------
 *
 * (See also: ecma262 rev c7952de (19 Aug 2016) 8.1 "Lexical Environments".)
 *
 * Scoping in ES is specified in terms of "Environment Records". There's a
 * global Environment Record per realm, and a new Environment Record is created
 * whenever control enters a function, block, or other scope.
 *
 * A "Lexical Environment" is a list of nested Environment Records, innermost
 * first: everything that's in scope. Throughout SpiderMonkey, "environment"
 * means a Lexical Environment.
 *
 * N.B.: "Scope" means something different: a static scope, the compile-time
 * analogue of an environment. See Scope.h.
 *
 * How SpiderMonkey represents environments
 * ----------------------------------------
 *
 * Some environments are stored as JSObjects. Several kinds of objects
 * represent environments:
 *
 *   JSObject
 *    |
 *    +--NativeObject
 *    |   |
 *    |   +--EnvironmentObject             Engine-internal environment
 *    |   |   |
 *    |   |   +--CallObject                    Environment of entire function
 *    |   |   |
 *    |   |   +--ModuleEnvironmentObject       Module top-level environment
 *    |   |   |
 *    |   |   +--LexicalEnvironmentObject      Lexical (block) environment
 *    |   |   |   |
 *    |   |   |   +--NamedLambdaObject             Environment for `(function f(){...})`
 *    |   |   |                                        containing only a binding for `f`
 *    |   |   +--VarEnvironmentObject          See VarScope in Scope.h.
 *    |   |   |
 *    |   |   +--WithEnvironmentObject         Presents object properties as bindings
 *    |   |   |
 *    |   |   +--NonSyntacticVariablesObject   See "Non-syntactic environments" below
 *    |   |
 *    |   +--GlobalObject                  The global environment
 *    |
 *    +--ProxyObject
 *        |
 *        +--DebugEnvironmentProxy         Environment for debugger eval-in-frame
 *
 * EnvironmentObjects are technically real JSObjects but only belong on the
 * environment chain (that is, fp->environmentChain() or fun->environment()).
 * They are never exposed to scripts.
 *
 * Note that reserved slots in any base classes shown above are fixed for all
 * derived classes. So e.g. EnvironmentObject::enclosingEnvironment() can
 * simply access a fixed slot without further dynamic type information.
 *
 * When the current environment is represented by an object, the stack frame
 * has a pointer to that object (see AbstractFramePtr::environmentChain()).
 * However, that isn't always the case. Where possible, we store binding values
 * in JS stack slots. For block and function scopes where all bindings can be
 * stored in stack slots, nothing is allocated in the heap; there is no
 * environment object.
 *
 * Full information about the environment chain is always recoverable:
 * EnvironmentIter can do it, and we construct a fake environment for debugger
 * eval-in-frame (see "Debug environment objects" below).
 *
 * Syntactic Environments
 * ----------------------
 *
 * Environments may be syntactic, i.e., corresponding to source text, or
 * non-syntactic, i.e., specially created by embedding. The distinction is
 * necessary to maintain invariants about the environment chain: non-syntactic
 * environments may not occur in arbitrary positions in the chain.
 *
 * CallObject, ModuleEnvironmentObject, and LexicalEnvironmentObject always
 * represent syntactic environments. (CallObject is considered syntactic even
 * when it's used as the scope of strict eval code.) WithEnvironmentObject is
 * syntactic when it's used to represent the scope of a `with` block.
 *
 *
 * Non-syntactic Environments
 * --------------------------
 *
 * A non-syntactic environment is one that was not created due to JS source
 * code. On the scope chain, a single NonSyntactic GlobalScope maps to 0+
 * non-syntactic environment objects. This is contrasted with syntactic
 * environments, where each scope corresponds to 0 or 1 environment object.
 *
 * There are 3 kinds of dynamic environment objects:
 *
 * 1. WithEnvironmentObject
 *
 *    When the embedding compiles or executes a script, it has the option to
 *    pass in a vector of objects to be used as the initial env chain, ordered
 *    from outermost env to innermost env. Each of those objects is wrapped by
 *    a WithEnvironmentObject.
 *
 *    The innermost object passed in by the embedding becomes a qualified
 *    variables object that captures 'var' bindings. That is, it wraps the
 *    holder object of 'var' bindings.
 *
 *    Does not hold 'let' or 'const' bindings.
 *
 * 2. NonSyntacticVariablesObject
 *
 *    When the embedding wants qualified 'var' bindings and unqualified
 *    bareword assignments to go on a different object than the global
 *    object. While any object can be made into a qualified variables object,
 *    only the GlobalObject and NonSyntacticVariablesObject are considered
 *    unqualified variables objects.
 *
 *    Unlike WithEnvironmentObjects that delegate to the object they wrap,
 *    this object is itself the holder of 'var' bindings.
 *
 *    Does not hold 'let' or 'const' bindings.
 *
 * 3. LexicalEnvironmentObject
 *
 *    Each non-syntactic object used as a qualified variables object needs to
 *    enclose a non-syntactic LexicalEnvironmentObject to hold 'let' and
 *    'const' bindings. There is a bijection per compartment between the
 *    non-syntactic variables objects and their non-syntactic
 *    LexicalEnvironmentObjects.
 *
 *    Does not hold 'var' bindings.
 *
 * The embedding (Gecko) uses non-syntactic envs for various things, some of
 * which are detailed below. All env chain listings below are, from top to
 * bottom, outermost to innermost.
 *
 * A. Component loading
 *
 * Components may be loaded in a shared global mode where most JSMs share a
 * single global in order to save on memory and avoid CCWs. To support this, a
 * NonSyntacticVariablesObject is used for each JSM to provide a basic form of
 * isolation. They have the following env chain:
 *
 *   BackstagePass global
 *       |
 *   LexicalEnvironmentObject[this=global]
 *       |
 *   NonSyntacticVariablesObject
 *       |
 *   LexicalEnvironmentObject[this=nsvo]
 *
 * B.1 Subscript loading
 *
 * Subscripts may be loaded into a target object and it's associated global.
 * They have the following env chain:
 *
 *   Target object's global
 *       |
 *   LexicalEnvironmentObject[this=global]
 *       |
 *   WithEnvironmentObject wrapping target
 *       |
 *   LexicalEnvironmentObject[this=target]
 *
 * B.2 Subscript loading (Shared-global JSM)
 *
 * The target object of a subscript load may be in a JSM with a shared global,
 * in which case we will also have the NonSyntacticVariablesObject on the
 * chain.
 *
 *   Target object's global
 *       |
 *   LexicalEnvironmentObject[this=global]
 *       |
 *   NonSyntacticVariablesObject
 *       |
 *   LexicalEnvironmentObject[this=nsvo]
 *       |
 *   WithEnvironmentObject wrapping target
 *       |
 *   LexicalEnvironmentObject[this=target]
 *
 * D. Frame scripts
 *
 * XUL frame scripts are always loaded with a NonSyntacticVariablesObject as a
 * "polluting global". This is done exclusively in
 * js::ExecuteInGlobalAndReturnScope.
 *
 *   Loader global
 *       |
 *   LexicalEnvironmentObject[this=global]
 *       |
 *   NonSyntacticVariablesObject
 *       |
 *   LexicalEnvironmentObject[this=global]
 *
 * D. XBL and DOM event handlers
 *
 * XBL methods are compiled as functions with XUL elements on the env chain,
 * and DOM event handlers are compiled as functions with HTML elements on the
 * env chain. For a chain of elements e0,e1,...:
 *
 *      ...
 *       |
 *   WithEnvironmentObject wrapping e1
 *       |
 *   WithEnvironmentObject wrapping e0
 *       |
 *   LexicalEnvironmentObject
 *
 */

class EnvironmentObject : public NativeObject
{
  protected:
    // The enclosing environment. Either another EnvironmentObject, a
    // GlobalObject, or a non-syntactic environment object.
    static const uint32_t ENCLOSING_ENV_SLOT = 0;

    inline void setAliasedBinding(JSContext* cx, uint32_t slot, PropertyName* name,
                                  const Value& v);

    void setEnclosingEnvironment(JSObject* enclosing) {
        setReservedSlot(ENCLOSING_ENV_SLOT, ObjectOrNullValue(enclosing));
    }

  public:
    // Since every env chain terminates with a global object, whether
    // GlobalObject or a non-syntactic one, and since those objects do not
    // derive EnvironmentObject (they have completely different layouts), the
    // enclosing environment of an EnvironmentObject is necessarily non-null.
    JSObject& enclosingEnvironment() const {
        return getReservedSlot(ENCLOSING_ENV_SLOT).toObject();
    }

    void initEnclosingEnvironment(JSObject* enclosing) {
        initReservedSlot(ENCLOSING_ENV_SLOT, ObjectOrNullValue(enclosing));
    }

    // Get or set a name contained in this environment.
    const Value& aliasedBinding(EnvironmentCoordinate ec) {
        return getSlot(ec.slot());
    }

    const Value& aliasedBinding(const BindingIter& bi) {
        MOZ_ASSERT(bi.location().kind() == BindingLocation::Kind::Environment);
        return getSlot(bi.location().slot());
    }

    inline void setAliasedBinding(JSContext* cx, EnvironmentCoordinate ec, PropertyName* name,
                                  const Value& v);

    inline void setAliasedBinding(JSContext* cx, const BindingIter& bi, const Value& v);

    // For JITs.
    static size_t offsetOfEnclosingEnvironment() {
        return getFixedSlotOffset(ENCLOSING_ENV_SLOT);
    }

    static uint32_t enclosingEnvironmentSlot() {
        return ENCLOSING_ENV_SLOT;
    }
};

class CallObject : public EnvironmentObject
{
  protected:
    static const uint32_t CALLEE_SLOT = 1;

    static CallObject* create(JSContext* cx, HandleScript script, HandleFunction callee,
                              HandleObject enclosing);

  public:
    static const uint32_t RESERVED_SLOTS = 2;
    static const Class class_;

    /* These functions are internal and are exposed only for JITs. */

    /*
     * Construct a bare-bones call object given a shape and a non-singleton
     * group.  The call object must be further initialized to be usable.
     */
    static CallObject* create(JSContext* cx, HandleShape shape, HandleObjectGroup group);

    /*
     * Construct a bare-bones call object given a shape and make it into
     * a singleton.  The call object must be initialized to be usable.
     */
    static CallObject* createSingleton(JSContext* cx, HandleShape shape);

    static CallObject* createTemplateObject(JSContext* cx, HandleScript script,
                                            HandleObject enclosing, gc::InitialHeap heap);

    static CallObject* create(JSContext* cx, HandleFunction callee, HandleObject enclosing);
    static CallObject* create(JSContext* cx, AbstractFramePtr frame);

    static CallObject* createHollowForDebug(JSContext* cx, HandleFunction callee);

    /*
     * When an aliased formal (var accessed by nested closures) is also
     * aliased by the arguments object, it must of course exist in one
     * canonical location and that location is always the CallObject. For this
     * to work, the ArgumentsObject stores special MagicValue in its array for
     * forwarded-to-CallObject variables. This MagicValue's payload is the
     * slot of the CallObject to access.
     */
    const Value& aliasedFormalFromArguments(const Value& argsValue) {
        return getSlot(ArgumentsObject::SlotFromMagicScopeSlotValue(argsValue));
    }
    inline void setAliasedFormalFromArguments(JSContext* cx, const Value& argsValue, jsid id,
                                              const Value& v);

    JSFunction& callee() const {
        return getReservedSlot(CALLEE_SLOT).toObject().as<JSFunction>();
    }

    /* For jit access. */
    static size_t offsetOfCallee() {
        return getFixedSlotOffset(CALLEE_SLOT);
    }

    static size_t calleeSlot() {
        return CALLEE_SLOT;
    }
};

class VarEnvironmentObject : public EnvironmentObject
{
    static const uint32_t SCOPE_SLOT = 1;

    static VarEnvironmentObject* create(JSContext* cx, HandleShape shape, HandleObject enclosing,
                                        gc::InitialHeap heap);

    void initScope(Scope* scope) {
        initReservedSlot(SCOPE_SLOT, PrivateGCThingValue(scope));
    }

  public:
    static const uint32_t RESERVED_SLOTS = 2;
    static const Class class_;

    static VarEnvironmentObject* create(JSContext* cx, HandleScope scope, AbstractFramePtr frame);
    static VarEnvironmentObject* createHollowForDebug(JSContext* cx, Handle<VarScope*> scope);

    Scope& scope() const {
        Value v = getReservedSlot(SCOPE_SLOT);
        MOZ_ASSERT(v.isPrivateGCThing());
        Scope& s = *static_cast<Scope*>(v.toGCThing());
        MOZ_ASSERT(s.is<VarScope>() || s.is<EvalScope>());
        return s;
    }

    bool isForEval() const {
        return scope().is<EvalScope>();
    }
};

class ModuleEnvironmentObject : public EnvironmentObject
{
    static const uint32_t MODULE_SLOT = 1;

    static const ObjectOps objectOps_;
    static const ClassOps classOps_;

  public:
    static const Class class_;

    static const uint32_t RESERVED_SLOTS = 2;

    static ModuleEnvironmentObject* create(JSContext* cx, HandleModuleObject module);
    ModuleObject& module();
    IndirectBindingMap& importBindings();

    bool createImportBinding(JSContext* cx, HandleAtom importName, HandleModuleObject module,
                             HandleAtom exportName);

    bool hasImportBinding(HandlePropertyName name);

    bool lookupImport(jsid name, ModuleEnvironmentObject** envOut, Shape** shapeOut);

    void fixEnclosingEnvironmentAfterCompartmentMerge(GlobalObject& global);

  private:
    static bool lookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                               MutableHandleObject objp, MutableHandle<PropertyResult> propp);
    static bool hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);
    static bool getProperty(JSContext* cx, HandleObject obj, HandleValue receiver, HandleId id,
                            MutableHandleValue vp);
    static bool setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                            HandleValue receiver, JS::ObjectOpResult& result);
    static bool getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                         MutableHandle<PropertyDescriptor> desc);
    static bool deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                               ObjectOpResult& result);
    static bool newEnumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                             bool enumerableOnly);
};

typedef Rooted<ModuleEnvironmentObject*> RootedModuleEnvironmentObject;
typedef Handle<ModuleEnvironmentObject*> HandleModuleEnvironmentObject;
typedef MutableHandle<ModuleEnvironmentObject*> MutableHandleModuleEnvironmentObject;

class WasmInstanceEnvironmentObject : public EnvironmentObject
{
    // Currently WasmInstanceScopes do not use their scopes in a
    // meaningful way. However, it is an invariant of DebugEnvironments that
    // environments kept in those maps have live scopes, thus this strong
    // reference.
    static const uint32_t SCOPE_SLOT = 1;

  public:
    static const Class class_;

    static const uint32_t RESERVED_SLOTS = 2;

    static WasmInstanceEnvironmentObject* createHollowForDebug(JSContext* cx,
                                                               Handle<WasmInstanceScope*> scope);
    WasmInstanceScope& scope() const {
        Value v = getReservedSlot(SCOPE_SLOT);
        MOZ_ASSERT(v.isPrivateGCThing());
        return *static_cast<WasmInstanceScope*>(v.toGCThing());
    }
};

class WasmFunctionCallObject : public EnvironmentObject
{
    // Currently WasmFunctionCallObjects do not use their scopes in a
    // meaningful way. However, it is an invariant of DebugEnvironments that
    // environments kept in those maps have live scopes, thus this strong
    // reference.
    static const uint32_t SCOPE_SLOT = 1;

  public:
    static const Class class_;

    static const uint32_t RESERVED_SLOTS = 2;

    static WasmFunctionCallObject* createHollowForDebug(JSContext* cx, HandleObject enclosing,
                                                        Handle<WasmFunctionScope*> scope);
    WasmFunctionScope& scope() const {
        Value v = getReservedSlot(SCOPE_SLOT);
        MOZ_ASSERT(v.isPrivateGCThing());
        return *static_cast<WasmFunctionScope*>(v.toGCThing());
    }
};

class LexicalEnvironmentObject : public EnvironmentObject
{
    // Global and non-syntactic lexical environments need to store a 'this'
    // value and all other lexical environments have a fixed shape and store a
    // backpointer to the LexicalScope.
    //
    // Since the two sets are disjoint, we only use one slot to save space.
    static const unsigned THIS_VALUE_OR_SCOPE_SLOT = 1;

  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const Class class_;

  private:
    static LexicalEnvironmentObject* createTemplateObject(JSContext* cx, HandleShape shape,
                                                          HandleObject enclosing,
                                                          gc::InitialHeap heap);

    void initThisValue(JSObject* obj) {
        MOZ_ASSERT(isGlobal() || !isSyntactic());
        initReservedSlot(THIS_VALUE_OR_SCOPE_SLOT, GetThisValue(obj));
    }

    void initScopeUnchecked(LexicalScope* scope) {
        initReservedSlot(THIS_VALUE_OR_SCOPE_SLOT, PrivateGCThingValue(scope));
    }

    void initScope(LexicalScope* scope) {
        MOZ_ASSERT(!isGlobal());
        MOZ_ASSERT(isSyntactic());
        initScopeUnchecked(scope);
    }

  public:
    static LexicalEnvironmentObject* create(JSContext* cx, Handle<LexicalScope*> scope,
                                            HandleObject enclosing, gc::InitialHeap heap);
    static LexicalEnvironmentObject* create(JSContext* cx, Handle<LexicalScope*> scope,
                                            AbstractFramePtr frame);
    static LexicalEnvironmentObject* createGlobal(JSContext* cx, Handle<GlobalObject*> global);
    static LexicalEnvironmentObject* createNonSyntactic(JSContext* cx, HandleObject enclosing,
                                                        HandleObject thisv);
    static LexicalEnvironmentObject* createHollowForDebug(JSContext* cx,
                                                          Handle<LexicalScope*> scope);

    // Create a new LexicalEnvironmentObject with the same enclosing env and
    // variable values as this.
    static LexicalEnvironmentObject* clone(JSContext* cx, Handle<LexicalEnvironmentObject*> env);

    // Create a new LexicalEnvironmentObject with the same enclosing env as
    // this, with all variables uninitialized.
    static LexicalEnvironmentObject* recreate(JSContext* cx, Handle<LexicalEnvironmentObject*> env);

    // For non-extensible lexical environments, the LexicalScope that created
    // this environment. Otherwise asserts.
    LexicalScope& scope() const {
        Value v = getReservedSlot(THIS_VALUE_OR_SCOPE_SLOT);
        MOZ_ASSERT(!isExtensible() && v.isPrivateGCThing());
        return *static_cast<LexicalScope*>(v.toGCThing());
    }

    // Is this the global lexical scope?
    bool isGlobal() const {
        return enclosingEnvironment().is<GlobalObject>();
    }

    GlobalObject& global() const {
        return enclosingEnvironment().as<GlobalObject>();
    }

    // Global and non-syntactic lexical scopes are extensible. All other
    // lexical scopes are not.
    bool isExtensible() const;

    // Is this a syntactic (i.e. corresponds to a source text) lexical
    // environment?
    bool isSyntactic() const {
        return !isExtensible() || isGlobal();
    }

    // For extensible lexical environments, the 'this' value for its
    // scope. Otherwise asserts.
    Value thisValue() const;
};

class NamedLambdaObject : public LexicalEnvironmentObject
{
    static NamedLambdaObject* create(JSContext* cx, HandleFunction callee,
                                     HandleFunction replacement,
                                     HandleObject enclosing, gc::InitialHeap heap);

  public:
    static NamedLambdaObject* createTemplateObject(JSContext* cx, HandleFunction callee,
                                                   gc::InitialHeap heap);

    static NamedLambdaObject* create(JSContext* cx, AbstractFramePtr frame);
    static NamedLambdaObject* create(JSContext* cx, AbstractFramePtr frame,
                                     HandleFunction replacement);

    // For JITs.
    static size_t lambdaSlot();
};

// A non-syntactic dynamic scope object that captures non-lexical
// bindings. That is, a scope object that captures both qualified var
// assignments and unqualified bareword assignments. Its parent is always the
// global lexical environment.
//
// This is used in ExecuteInGlobalAndReturnScope and sits in front of the
// global scope to store 'var' bindings, and to store fresh properties created
// by assignments to undeclared variables that otherwise would have gone on
// the global object.
class NonSyntacticVariablesObject : public EnvironmentObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const Class class_;

    static NonSyntacticVariablesObject* create(JSContext* cx);
};

// With environment objects on the run-time environment chain.
class WithEnvironmentObject : public EnvironmentObject
{
    static const unsigned OBJECT_SLOT = 1;
    static const unsigned THIS_SLOT = 2;
    static const unsigned SCOPE_SLOT = 3;

  public:
    static const unsigned RESERVED_SLOTS = 4;
    static const Class class_;

    static WithEnvironmentObject* create(JSContext* cx, HandleObject object, HandleObject enclosing,
                                         Handle<WithScope*> scope);
    static WithEnvironmentObject* createNonSyntactic(JSContext* cx, HandleObject object,
                                                     HandleObject enclosing);

    /* Return the 'o' in 'with (o)'. */
    JSObject& object() const;

    /* Return object for GetThisValue. */
    JSObject* withThis() const;

    /*
     * Return whether this object is a syntactic with object.  If not, this is
     * a With object we inserted between the outermost syntactic scope and the
     * global object to wrap the environment chain someone explicitly passed
     * via JSAPI to CompileFunction or script evaluation.
     */
    bool isSyntactic() const;

    // For syntactic with environment objects, the with scope.
    WithScope& scope() const;

    static inline size_t objectSlot() {
        return OBJECT_SLOT;
    }

    static inline size_t thisSlot() {
        return THIS_SLOT;
    }
};

// Internal scope object used by JSOP_BINDNAME upon encountering an
// uninitialized lexical slot or an assignment to a 'const' binding.
//
// ES6 lexical bindings cannot be accessed in any way (throwing
// ReferenceErrors) until initialized. Normally, NAME operations
// unconditionally check for uninitialized lexical slots. When getting or
// looking up names, this can be done without slowing down normal operations
// on the return value. When setting names, however, we do not want to pollute
// all set-property paths with uninitialized lexical checks. For setting names
// (i.e. JSOP_SETNAME), we emit an accompanying, preceding JSOP_BINDNAME which
// finds the right scope on which to set the name. Moreover, when the name on
// the scope is an uninitialized lexical, we cannot throw eagerly, as the spec
// demands that the error be thrown after evaluating the RHS of
// assignments. Instead, this sentinel scope object is pushed on the stack.
// Attempting to access anything on this scope throws the appropriate
// ReferenceError.
//
// ES6 'const' bindings induce a runtime error when assigned to outside
// of initialization, regardless of strictness.
class RuntimeLexicalErrorObject : public EnvironmentObject
{
    static const unsigned ERROR_SLOT = 1;

  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const Class class_;

    static RuntimeLexicalErrorObject* create(JSContext* cx, HandleObject enclosing,
                                             unsigned errorNumber);

    unsigned errorNumber() {
        return getReservedSlot(ERROR_SLOT).toInt32();
    }
};


/*****************************************************************************/

// A environment iterator describes the active environments starting from an
// environment, scope pair. This pair may be derived from the current point of
// execution in a frame. If derived in such a fashion, the EnvironmentIter
// tracks whether the current scope is within the extent of this initial
// frame.  Here, "frame" means a single activation of: a function, eval, or
// global code.
class MOZ_RAII EnvironmentIter
{
    Rooted<ScopeIter> si_;
    RootedObject env_;
    AbstractFramePtr frame_;

    void incrementScopeIter();
    void settle();

    // No value semantics.
    EnvironmentIter(const EnvironmentIter& ei) = delete;

  public:
    // Constructing from a copy of an existing EnvironmentIter.
    EnvironmentIter(JSContext* cx, const EnvironmentIter& ei
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    // Constructing from an environment, scope pair. All environments
    // considered not to be withinInitialFrame, since no frame is given.
    EnvironmentIter(JSContext* cx, JSObject* env, Scope* scope
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    // Constructing from a frame. Places the EnvironmentIter on the innermost
    // environment at pc.
    EnvironmentIter(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    // Constructing from an environment, scope and frame. The frame is given
    // to initialize to proper enclosing environment/scope.
    EnvironmentIter(JSContext* cx, JSObject* env, Scope* scope, AbstractFramePtr frame
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM);


    bool done() const {
        return si_.done();
    }

    explicit operator bool() const {
        return !done();
    }

    void operator++(int) {
        if (hasAnyEnvironmentObject())
            env_ = &env_->as<EnvironmentObject>().enclosingEnvironment();
        incrementScopeIter();
        settle();
    }

    EnvironmentIter& operator++() {
        operator++(1);
        return *this;
    }

    // If done():
    JSObject& enclosingEnvironment() const;

    // If !done():
    bool hasNonSyntacticEnvironmentObject() const;

    bool hasSyntacticEnvironment() const {
        return si_.hasSyntacticEnvironment();
    }

    bool hasAnyEnvironmentObject() const {
        return hasNonSyntacticEnvironmentObject() || hasSyntacticEnvironment();
    }

    EnvironmentObject& environment() const {
        MOZ_ASSERT(hasAnyEnvironmentObject());
        return env_->as<EnvironmentObject>();
    }

    Scope& scope() const {
        return *si_.scope();
    }

    Scope* maybeScope() const {
        if (si_)
            return si_.scope();
        return nullptr;
    }

    JSFunction& callee() const {
        return env_->as<CallObject>().callee();
    }

    bool withinInitialFrame() const {
        return !!frame_;
    }

    AbstractFramePtr initialFrame() const {
        MOZ_ASSERT(withinInitialFrame());
        return frame_;
    }

    AbstractFramePtr maybeInitialFrame() const {
        return frame_;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

// The key in MissingEnvironmentMap. For live frames, maps live frames to
// their synthesized environments. For completely optimized-out environments,
// maps the Scope to their synthesized environments. The env we synthesize for
// Scopes are read-only, and we never use their parent links, so they don't
// need to be distinct.
//
// That is, completely optimized out environments can't be distinguished by
// frame. Note that even if the frame corresponding to the Scope is live on
// the stack, it is unsound to synthesize an environment from that live
// frame. In other words, the provenance of the environment chain is from
// allocated closures (i.e., allocation sites) and is irrecoverable from
// simple stack inspection (i.e., call sites).
class MissingEnvironmentKey
{
    friend class LiveEnvironmentVal;

    AbstractFramePtr frame_;
    Scope* scope_;

  public:
    explicit MissingEnvironmentKey(const EnvironmentIter& ei)
      : frame_(ei.maybeInitialFrame()),
        scope_(ei.maybeScope())
    { }

    MissingEnvironmentKey(AbstractFramePtr frame, Scope* scope)
      : frame_(frame),
        scope_(scope)
    { }

    AbstractFramePtr frame() const { return frame_; }
    Scope* scope() const { return scope_; }

    void updateScope(Scope* scope) { scope_ = scope; }
    void updateFrame(AbstractFramePtr frame) { frame_ = frame; }

    // For use as hash policy.
    typedef MissingEnvironmentKey Lookup;
    static HashNumber hash(MissingEnvironmentKey sk);
    static bool match(MissingEnvironmentKey sk1, MissingEnvironmentKey sk2);
    bool operator!=(const MissingEnvironmentKey& other) const {
        return frame_ != other.frame_ || scope_ != other.scope_;
    }
    static void rekey(MissingEnvironmentKey& k, const MissingEnvironmentKey& newKey) {
        k = newKey;
    }
};

// The value in LiveEnvironmentMap, mapped from by live environment objects.
class LiveEnvironmentVal
{
    friend class DebugEnvironments;
    friend class MissingEnvironmentKey;

    AbstractFramePtr frame_;
    HeapPtr<Scope*> scope_;

    static void staticAsserts();

  public:
    explicit LiveEnvironmentVal(const EnvironmentIter& ei)
      : frame_(ei.initialFrame()),
        scope_(ei.maybeScope())
    { }

    AbstractFramePtr frame() const { return frame_; }
    Scope* scope() const { return scope_; }

    void updateFrame(AbstractFramePtr frame) { frame_ = frame; }

    bool needsSweep();
};


/*****************************************************************************/

/*
 * Debug environment objects
 *
 * The debugger effectively turns every opcode into a potential direct eval.
 * Naively, this would require creating a EnvironmentObject for every
 * call/block scope and using JSOP_GETALIASEDVAR for every access. To optimize
 * this, the engine assumes there is no debugger and optimizes scope access
 * and creation accordingly. When the debugger wants to perform an unexpected
 * eval-in-frame (or other, similar environment-requiring operations),
 * fp->environmentChain is now incomplete.
 *
 * To resolve this, the debugger first calls GetDebugEnvironmentFor* to
 * synthesize a "debug env chain". A debug env chain is just a chain of
 * objects that fill in missing environments and protect the engine from
 * unexpected access. (The latter means that some debugger operations, like
 * redefining a lexical binding, can fail when a true eval would succeed.) To
 * do both of these things, GetDebugEnvironmentFor* creates a new proxy
 * DebugEnvironmentProxy to sit in front of every existing EnvironmentObject.
 *
 * GetDebugEnvironmentFor* ensures the invariant that the same
 * DebugEnvironmentProxy is always produced for the same underlying
 * environment (optimized or not!). This is maintained by some bookkeeping
 * information stored in DebugEnvironments.
 */

extern JSObject*
GetDebugEnvironmentForFunction(JSContext* cx, HandleFunction fun);

extern JSObject*
GetDebugEnvironmentForFrame(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc);

extern JSObject*
GetDebugEnvironmentForGlobalLexicalEnvironment(JSContext* cx);

/* Provides debugger access to a environment. */
class DebugEnvironmentProxy : public ProxyObject
{
    /*
     * The enclosing environment on the dynamic environment chain. This slot is analogous
     * to the ENCLOSING_ENV_SLOT of a EnvironmentObject.
     */
    static const unsigned ENCLOSING_SLOT = 0;

    /*
     * NullValue or a dense array holding the unaliased variables of a function
     * frame that has been popped.
     */
    static const unsigned SNAPSHOT_SLOT = 1;

  public:
    static DebugEnvironmentProxy* create(JSContext* cx, EnvironmentObject& env,
                                         HandleObject enclosing);

    // NOTE: The environment may be a debug hollow with invalid
    // enclosingEnvironment. Always use the enclosingEnvironment accessor on
    // the DebugEnvironmentProxy in order to walk the environment chain.
    EnvironmentObject& environment() const;
    JSObject& enclosingEnvironment() const;

    /* May only be called for proxies to function call objects. */
    ArrayObject* maybeSnapshot() const;
    void initSnapshot(ArrayObject& snapshot);

    // Currently, the 'declarative' environments are function, module, and
    // lexical environments.
    bool isForDeclarative() const;

    // Get a property by 'id', but returns sentinel values instead of throwing
    // on exceptional cases.
    static bool getMaybeSentinelValue(JSContext* cx, Handle<DebugEnvironmentProxy*> env,
                                      HandleId id, MutableHandleValue vp);

    // Returns true iff this is a function environment with its own this-binding
    // (all functions except arrow functions).
    bool isFunctionEnvironmentWithThis();

    // Does this debug environment not have a real counterpart or was never
    // live (and thus does not have a synthesized EnvironmentObject or a
    // snapshot)?
    bool isOptimizedOut() const;
};

/* Maintains per-compartment debug environment bookkeeping information. */
class DebugEnvironments
{
    Zone* zone_;

    /* The map from (non-debug) environments to debug environments. */
    ObjectWeakMap proxiedEnvs;

    /*
     * The map from live frames which have optimized-away environments to the
     * corresponding debug environments.
     */
    typedef HashMap<MissingEnvironmentKey,
                    ReadBarrieredDebugEnvironmentProxy,
                    MissingEnvironmentKey,
                    ZoneAllocPolicy> MissingEnvironmentMap;
    MissingEnvironmentMap missingEnvs;

    /*
     * The map from environment objects of live frames to the live frame. This
     * map updated lazily whenever the debugger needs the information. In
     * between two lazy updates, liveEnvs becomes incomplete (but not invalid,
     * onPop* removes environments as they are popped). Thus, two consecutive
     * debugger lazy updates of liveEnvs need only fill in the new
     * environments.
     */
    typedef GCHashMap<ReadBarriered<JSObject*>,
                      LiveEnvironmentVal,
                      MovableCellHasher<ReadBarriered<JSObject*>>,
                      ZoneAllocPolicy> LiveEnvironmentMap;
    LiveEnvironmentMap liveEnvs;

  public:
    DebugEnvironments(JSContext* cx, Zone* zone);
    ~DebugEnvironments();

    Zone* zone() const { return zone_; }

  private:
    bool init();

    static DebugEnvironments* ensureCompartmentData(JSContext* cx);

    template <typename Environment, typename Scope>
    static void onPopGeneric(JSContext* cx, const EnvironmentIter& ei);

  public:
    void trace(JSTracer* trc);
    void sweep();
    void finish();
#ifdef JS_GC_ZEAL
    void checkHashTablesAfterMovingGC();
#endif

    // If a live frame has a synthesized entry in missingEnvs, make sure it's not
    // collected.
    void traceLiveFrame(JSTracer* trc, AbstractFramePtr frame);

    static DebugEnvironmentProxy* hasDebugEnvironment(JSContext* cx, EnvironmentObject& env);
    static bool addDebugEnvironment(JSContext* cx, Handle<EnvironmentObject*> env,
                                    Handle<DebugEnvironmentProxy*> debugEnv);

    static DebugEnvironmentProxy* hasDebugEnvironment(JSContext* cx, const EnvironmentIter& ei);
    static bool addDebugEnvironment(JSContext* cx, const EnvironmentIter& ei,
                                    Handle<DebugEnvironmentProxy*> debugEnv);

    static bool updateLiveEnvironments(JSContext* cx);
    static LiveEnvironmentVal* hasLiveEnvironment(EnvironmentObject& env);
    static void unsetPrevUpToDateUntil(JSContext* cx, AbstractFramePtr frame);

    // When a frame bails out from Ion to Baseline, there might be missing
    // envs keyed on, and live envs containing, the old
    // RematerializedFrame. Forward those values to the new BaselineFrame.
    static void forwardLiveFrame(JSContext* cx, AbstractFramePtr from, AbstractFramePtr to);

    // When an environment is popped, we store a snapshot of its bindings that
    // live on the frame.
    //
    // This is done during frame unwinding, which cannot handle errors
    // gracefully. Errors result in no snapshot being set on the
    // DebugEnvironmentProxy.
    static void takeFrameSnapshot(JSContext* cx, Handle<DebugEnvironmentProxy*> debugEnv,
                                  AbstractFramePtr frame);

    // In debug-mode, these must be called whenever exiting a scope that might
    // have stack-allocated locals.
    static void onPopCall(JSContext* cx, AbstractFramePtr frame);
    static void onPopVar(JSContext* cx, const EnvironmentIter& ei);
    static void onPopVar(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc);
    static void onPopLexical(JSContext* cx, const EnvironmentIter& ei);
    static void onPopLexical(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc);
    static void onPopWith(AbstractFramePtr frame);
    static void onCompartmentUnsetIsDebuggee(JSCompartment* c);
};

}  /* namespace js */

template <>
inline bool
JSObject::is<js::EnvironmentObject>() const
{
    return is<js::CallObject>() ||
           is<js::VarEnvironmentObject>() ||
           is<js::ModuleEnvironmentObject>() ||
           is<js::WasmInstanceEnvironmentObject>() ||
           is<js::WasmFunctionCallObject>() ||
           is<js::LexicalEnvironmentObject>() ||
           is<js::WithEnvironmentObject>() ||
           is<js::NonSyntacticVariablesObject>() ||
           is<js::RuntimeLexicalErrorObject>();
}

template<>
bool
JSObject::is<js::DebugEnvironmentProxy>() const;

namespace js {

inline bool
IsSyntacticEnvironment(JSObject* env)
{
    if (!env->is<EnvironmentObject>())
        return false;

    if (env->is<WithEnvironmentObject>())
        return env->as<WithEnvironmentObject>().isSyntactic();

    if (env->is<LexicalEnvironmentObject>())
        return env->as<LexicalEnvironmentObject>().isSyntactic();

    if (env->is<NonSyntacticVariablesObject>())
        return false;

    return true;
}

inline bool
IsExtensibleLexicalEnvironment(JSObject* env)
{
    return env->is<LexicalEnvironmentObject>() &&
           env->as<LexicalEnvironmentObject>().isExtensible();
}

inline bool
IsGlobalLexicalEnvironment(JSObject* env)
{
    return env->is<LexicalEnvironmentObject>() &&
           env->as<LexicalEnvironmentObject>().isGlobal();
}

inline bool
IsNSVOLexicalEnvironment(JSObject* env)
{
    return env->is<LexicalEnvironmentObject>() &&
           env->as<LexicalEnvironmentObject>().enclosingEnvironment()
                                              .is<NonSyntacticVariablesObject>();
}

inline JSObject*
MaybeUnwrapWithEnvironment(JSObject* env)
{
    if (env->is<WithEnvironmentObject>())
        return &env->as<WithEnvironmentObject>().object();
    return env;
}

template <typename SpecificEnvironment>
inline bool
IsFrameInitialEnvironment(AbstractFramePtr frame, SpecificEnvironment& env)
{
    // A frame's initial environment is the innermost environment
    // corresponding to the scope chain from frame.script()->bodyScope() to
    // frame.script()->outermostScope(). This environment must be on the chain
    // for the frame to be considered initialized. That is, it must be on the
    // chain for the environment chain to fully match the scope chain at the
    // start of execution in the frame.
    //
    // This logic must be in sync with the HAS_INITIAL_ENV logic in
    // InitFromBailout.

    // A function frame's CallObject, if present, is always the initial
    // environment.
    if (mozilla::IsSame<SpecificEnvironment, CallObject>::value)
        return true;

    // For an eval frame, the VarEnvironmentObject, if present, is always the
    // initial environment.
    if (mozilla::IsSame<SpecificEnvironment, VarEnvironmentObject>::value &&
        frame.isEvalFrame())
    {
        return true;
    }

    // For named lambda frames without CallObjects (i.e., no binding in the
    // body of the function was closed over), the LexicalEnvironmentObject
    // corresponding to the named lambda scope is the initial environment.
    if (mozilla::IsSame<SpecificEnvironment, NamedLambdaObject>::value &&
        frame.isFunctionFrame() &&
        frame.callee()->needsNamedLambdaEnvironment() &&
        !frame.callee()->needsCallObject())
    {
        LexicalScope* namedLambdaScope = frame.script()->maybeNamedLambdaScope();
        return &env.template as<LexicalEnvironmentObject>().scope() == namedLambdaScope;
    }

    return false;
}

extern bool
CreateObjectsForEnvironmentChain(JSContext* cx, AutoObjectVector& chain,
                                 HandleObject terminatingEnv,
                                 MutableHandleObject envObj);

ModuleEnvironmentObject* GetModuleEnvironmentForScript(JSScript* script);

MOZ_MUST_USE bool
GetThisValueForDebuggerMaybeOptimizedOut(JSContext* cx, AbstractFramePtr frame,
                                         jsbytecode* pc, MutableHandleValue res);

MOZ_MUST_USE bool
CheckVarNameConflict(JSContext* cx, Handle<LexicalEnvironmentObject*> lexicalEnv,
                     HandlePropertyName name);

MOZ_MUST_USE bool
CheckCanDeclareGlobalBinding(JSContext* cx, Handle<GlobalObject*> global,
                             HandlePropertyName name, bool isFunction);

MOZ_MUST_USE bool
CheckLexicalNameConflict(JSContext* cx, Handle<LexicalEnvironmentObject*> lexicalEnv,
                         HandleObject varObj, HandlePropertyName name);

MOZ_MUST_USE bool
CheckGlobalDeclarationConflicts(JSContext* cx, HandleScript script,
                                Handle<LexicalEnvironmentObject*> lexicalEnv,
                                HandleObject varObj);

MOZ_MUST_USE bool
CheckEvalDeclarationConflicts(JSContext* cx, HandleScript script, HandleObject envChain,
                              HandleObject varObj);

MOZ_MUST_USE bool
InitFunctionEnvironmentObjects(JSContext* cx, AbstractFramePtr frame);

MOZ_MUST_USE bool
PushVarEnvironmentObject(JSContext* cx, HandleScope scope, AbstractFramePtr frame);

MOZ_MUST_USE bool
GetFrameEnvironmentAndScope(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc,
                            MutableHandleObject env, MutableHandleScope scope);

#ifdef DEBUG
bool
AnalyzeEntrainedVariables(JSContext* cx, HandleScript script);
#endif

} // namespace js

namespace JS {

template <>
struct DeletePolicy<js::DebugEnvironments> : public js::GCManagedDeletePolicy<js::DebugEnvironments>
{};

} // namespace JS

#endif /* vm_EnvironmentObject_h */
