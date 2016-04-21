/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ScopeObject_h
#define vm_ScopeObject_h

#include "jscntxt.h"
#include "jsobj.h"
#include "jsweakmap.h"

#include "builtin/ModuleObject.h"
#include "gc/Barrier.h"
#include "js/GCHashTable.h"
#include "vm/ArgumentsObject.h"
#include "vm/ProxyObject.h"

namespace js {

namespace frontend {
struct Definition;
class FunctionBox;
class ModuleBox;
}

class StaticWithObject;
class StaticEvalObject;
class StaticNonSyntacticScopeObjects;

class ModuleObject;
typedef Handle<ModuleObject*> HandleModuleObject;

/*****************************************************************************/

/*
 * The static scope chain is the canonical truth for lexical scope contour of
 * a program. The dynamic scope chain is derived from the static scope chain:
 * it is the chain of scopes whose static scopes have a runtime
 * representation, for example, due to aliased bindings.
 *
 * Static scopes roughly correspond to a scope in the program text. They are
 * divided into scopes that have direct correspondence to program text (i.e.,
 * syntactic) and ones used internally for scope walking (i.e., non-syntactic).
 *
 * The following are syntactic static scopes:
 *
 * StaticBlockObject
 *   Scope for non-function body blocks. e.g., |{ let x; }|
 *
 * JSFunction
 *   Scope for function bodies. e.g., |function f() { var x; let y; }|
 *
 * ModuleObject
 *   Scope for moddules.
 *
 * StaticWithObject
 *   Scope for |with|. e.g., |with ({}) { ... }|
 *
 * StaticEvalObject
 *   Scope for |eval|. e.g., |eval(...)|
 *
 * The following are non-syntactic static scopes:
 *
 * StaticNonSyntacticScopeObjects
 *   Signals presence of "polluting" scope objects. Used by Gecko.
 *
 * There is an additional scope for named lambdas without a static scope
 * object. E.g., in:
 *
 *   (function f() { var x; function g() { } })
 *
 * All static scope objects are ScopeObjects with the exception of JSFunction
 * and ModuleObject, which keeps their enclosing scope link on
 * |JSScript::enclosingStaticScope()|.
 */
template <AllowGC allowGC>
class StaticScopeIter
{
    typename MaybeRooted<JSObject*, allowGC>::RootType obj;
    bool onNamedLambda;

    static bool IsStaticScope(JSObject* obj) {
        return obj->is<StaticBlockObject>() ||
               obj->is<StaticWithObject>() ||
               obj->is<StaticEvalObject>() ||
               obj->is<StaticNonSyntacticScopeObjects>() ||
               obj->is<JSFunction>() ||
               obj->is<ModuleObject>();
    }

  public:
    StaticScopeIter(ExclusiveContext* cx, JSObject* obj)
      : obj(cx, obj), onNamedLambda(false)
    {
        static_assert(allowGC == CanGC,
                      "the context-accepting constructor should only be used "
                      "in CanGC code");
        MOZ_ASSERT_IF(obj, IsStaticScope(obj));
    }

    StaticScopeIter(ExclusiveContext* cx, const StaticScopeIter<CanGC>& ssi)
      : obj(cx, ssi.obj), onNamedLambda(ssi.onNamedLambda)
    {
        JS_STATIC_ASSERT(allowGC == CanGC);
    }

    explicit StaticScopeIter(JSObject* obj)
      : obj((ExclusiveContext*) nullptr, obj), onNamedLambda(false)
    {
        static_assert(allowGC == NoGC,
                      "the constructor not taking a context should only be "
                      "used in NoGC code");
        MOZ_ASSERT_IF(obj, IsStaticScope(obj));
    }

    explicit StaticScopeIter(const StaticScopeIter<NoGC>& ssi)
      : obj((ExclusiveContext*) nullptr, ssi.obj), onNamedLambda(ssi.onNamedLambda)
    {
        static_assert(allowGC == NoGC,
                      "the constructor not taking a context should only be "
                      "used in NoGC code");
    }

    bool done() const { return !obj; }
    void operator++(int);

    JSObject* staticScope() const { MOZ_ASSERT(!done()); return obj; }

    // Return whether this static scope will have a syntactic scope (i.e. a
    // ScopeObject that isn't a non-syntactic With or
    // NonSyntacticVariablesObject) on the dynamic scope chain.
    bool hasSyntacticDynamicScopeObject() const;
    Shape* scopeShape() const;

    enum Type { Module, Function, Block, With, NamedLambda, Eval, NonSyntactic };
    Type type() const;

    StaticBlockObject& block() const;
    StaticWithObject& staticWith() const;
    StaticEvalObject& eval() const;
    StaticNonSyntacticScopeObjects& nonSyntactic() const;
    JSScript* funScript() const;
    JSFunction& fun() const;
    frontend::FunctionBox* maybeFunctionBox() const;
    JSScript* moduleScript() const;
    ModuleObject& module() const;
};

/*****************************************************************************/

/*
 * A "scope coordinate" describes how to get from head of the scope chain to a
 * given lexically-enclosing variable. A scope coordinate has two dimensions:
 *  - hops: the number of scope objects on the scope chain to skip
 *  - slot: the slot on the scope object holding the variable's value
 */
class ScopeCoordinate
{
    uint32_t hops_;
    uint32_t slot_;

    /*
     * Technically, hops_/slot_ are SCOPECOORD_(HOPS|SLOT)_BITS wide.  Since
     * ScopeCoordinate is a temporary value, don't bother with a bitfield as
     * this only adds overhead.
     */
    static_assert(SCOPECOORD_HOPS_BITS <= 32, "We have enough bits below");
    static_assert(SCOPECOORD_SLOT_BITS <= 32, "We have enough bits below");

  public:
    explicit inline ScopeCoordinate(jsbytecode* pc)
      : hops_(GET_SCOPECOORD_HOPS(pc)), slot_(GET_SCOPECOORD_SLOT(pc + SCOPECOORD_HOPS_LEN))
    {
        MOZ_ASSERT(JOF_OPTYPE(JSOp(*pc)) == JOF_SCOPECOORD);
    }

    inline ScopeCoordinate() {}

    void setHops(uint32_t hops) { MOZ_ASSERT(hops < SCOPECOORD_HOPS_LIMIT); hops_ = hops; }
    void setSlot(uint32_t slot) { MOZ_ASSERT(slot < SCOPECOORD_SLOT_LIMIT); slot_ = slot; }

    uint32_t hops() const { MOZ_ASSERT(hops_ < SCOPECOORD_HOPS_LIMIT); return hops_; }
    uint32_t slot() const { MOZ_ASSERT(slot_ < SCOPECOORD_SLOT_LIMIT); return slot_; }

    bool operator==(const ScopeCoordinate& rhs) const {
        return hops() == rhs.hops() && slot() == rhs.slot();
    }
};

/*
 * Return a shape representing the static scope containing the variable
 * accessed by the ALIASEDVAR op at 'pc'.
 */
extern Shape*
ScopeCoordinateToStaticScopeShape(JSScript* script, jsbytecode* pc);

/* Return the name being accessed by the given ALIASEDVAR op. */
extern PropertyName*
ScopeCoordinateName(ScopeCoordinateNameCache& cache, JSScript* script, jsbytecode* pc);

/* Return the function script accessed by the given ALIASEDVAR op, or nullptr. */
extern JSScript*
ScopeCoordinateFunctionScript(JSScript* script, jsbytecode* pc);

/*****************************************************************************/

/*
 * Scope objects
 *
 * Scope objects are technically real JSObjects but only belong on the scope
 * chain (that is, fp->scopeChain() or fun->environment()). The hierarchy of
 * scope objects is:
 *
 *   JSObject                      Generic object
 *     |
 *   ScopeObject---+---+           Engine-internal scope
 *     |   |   |   |   |
 *     |   |   |   |  StaticNonSyntacticScopeObjects  See "Non-syntactic scope objects"
 *     |   |   |   |
 *     |   |   |  StaticEvalObject  Placeholder so eval scopes may be iterated through
 *     |   |   |
 *     |   |  DeclEnvObject         Holds name of recursive/needsCallObject named lambda
 *     |   |
 *     |  LexicalScopeBase          Shared base for function and modules scopes
 *     |   |   |
 *     |   |  CallObject            Scope of entire function or strict eval
 *     |   |
 *     |  ModuleEnvironmentObject   Module top-level scope on run-time scope chain
 *     |
 *   NestedScopeObject              Statement scopes; don't cross script boundaries
 *     |   |   |
 *     |   |  StaticWithObject      Template for "with" object in static scope chain
 *     |   |
 *     |  DynamicWithObject         Run-time "with" object on scope chain
 *     |
 *   BlockObject                    Shared interface of cloned/static block objects
 *     |   |
 *     |  ClonedBlockObject         let, switch, catch, for
 *     |
 *   StaticBlockObject              See NB
 *
 * This hierarchy represents more than just the interface hierarchy: reserved
 * slots in base classes are fixed for all derived classes. Thus, for example,
 * ScopeObject::enclosingScope() can simply access a fixed slot without further
 * dynamic type information.
 *
 * NB: Static block objects are a special case: these objects are created at
 * compile time to hold the shape/binding information from which block objects
 * are cloned at runtime. These objects should never escape into the wild and
 * support a restricted set of ScopeObject operations.
 *
 * See also "Debug scope objects" below.
 */

class ScopeObject : public NativeObject
{
  protected:
    static const uint32_t SCOPE_CHAIN_SLOT = 0;

  public:
    /*
     * Since every scope chain terminates with a global object and GlobalObject
     * does not derive ScopeObject (it has a completely different layout), the
     * enclosing scope of a ScopeObject is necessarily non-null.
     */
    inline JSObject& enclosingScope() const {
        return getFixedSlot(SCOPE_CHAIN_SLOT).toObject();
    }

    void setEnclosingScope(HandleObject obj);

    /*
     * Get or set an aliased variable contained in this scope. Unaliased
     * variables should instead access the stack frame. Aliased variable access
     * is primarily made through JOF_SCOPECOORD ops which is why these members
     * take a ScopeCoordinate instead of just the slot index.
     */
    inline const Value& aliasedVar(ScopeCoordinate sc);

    inline void setAliasedVar(JSContext* cx, ScopeCoordinate sc, PropertyName* name, const Value& v);

    /* For jit access. */
    static size_t offsetOfEnclosingScope() {
        return getFixedSlotOffset(SCOPE_CHAIN_SLOT);
    }

    static size_t enclosingScopeSlot() {
        return SCOPE_CHAIN_SLOT;
    }
};

class LexicalScopeBase : public ScopeObject
{
  protected:
    inline void initRemainingSlotsToUninitializedLexicals(uint32_t begin);
    inline void initAliasedLexicalsToThrowOnTouch(JSScript* script);

  public:
    /* Get/set the aliased variable referred to by 'fi'. */
    const Value& aliasedVar(AliasedFormalIter fi) {
        return getSlot(fi.scopeSlot());
    }
    inline void setAliasedVar(JSContext* cx, AliasedFormalIter fi, PropertyName* name,
                              const Value& v);

    /*
     * When an aliased var (var accessed by nested closures) is also aliased by
     * the arguments object, it must of course exist in one canonical location
     * and that location is always the CallObject. For this to work, the
     * ArgumentsObject stores special MagicValue in its array for forwarded-to-
     * CallObject variables. This MagicValue's payload is the slot of the
     * CallObject to access.
     */
    const Value& aliasedVarFromArguments(const Value& argsValue) {
        return getSlot(ArgumentsObject::SlotFromMagicScopeSlotValue(argsValue));
    }
    inline void setAliasedVarFromArguments(JSContext* cx, const Value& argsValue, jsid id,
                                           const Value& v);
};

class CallObject : public LexicalScopeBase
{
  protected:
    static const uint32_t CALLEE_SLOT = 1;

    static CallObject*
    create(JSContext* cx, HandleScript script, HandleObject enclosing, HandleFunction callee);

  public:
    static const Class class_;

    /* These functions are internal and are exposed only for JITs. */

    /*
     * Construct a bare-bones call object given a shape and a non-singleton
     * group.  The call object must be further initialized to be usable.
     */
    static CallObject*
    create(JSContext* cx, HandleShape shape, HandleObjectGroup group, uint32_t lexicalBegin);

    /*
     * Construct a bare-bones call object given a shape and make it into
     * a singleton.  The call object must be initialized to be usable.
     */
    static CallObject*
    createSingleton(JSContext* cx, HandleShape shape, uint32_t lexicalBegin);

    static CallObject*
    createTemplateObject(JSContext* cx, HandleScript script, gc::InitialHeap heap);

    static const uint32_t RESERVED_SLOTS = 2;

    static CallObject* createForFunction(JSContext* cx, HandleObject enclosing, HandleFunction callee);

    static CallObject* createForFunction(JSContext* cx, AbstractFramePtr frame);
    static CallObject* createForStrictEval(JSContext* cx, AbstractFramePtr frame);
    static CallObject* createHollowForDebug(JSContext* cx, HandleFunction callee);

    /* True if this is for a strict mode eval frame. */
    bool isForEval() const {
        if (is<ModuleEnvironmentObject>())
            return false;
        MOZ_ASSERT(getFixedSlot(CALLEE_SLOT).isObjectOrNull());
        MOZ_ASSERT_IF(getFixedSlot(CALLEE_SLOT).isObject(),
                      getFixedSlot(CALLEE_SLOT).toObject().is<JSFunction>());
        return getFixedSlot(CALLEE_SLOT).isNull();
    }

    /*
     * Returns the function for which this CallObject was created. (This may
     * only be called if !isForEval.)
     */
    JSFunction& callee() const {
        MOZ_ASSERT(!is<ModuleEnvironmentObject>());
        return getFixedSlot(CALLEE_SLOT).toObject().as<JSFunction>();
    }

    /* For jit access. */
    static size_t offsetOfCallee() {
        return getFixedSlotOffset(CALLEE_SLOT);
    }

    static size_t calleeSlot() {
        return CALLEE_SLOT;
    }
};

class ModuleEnvironmentObject : public LexicalScopeBase
{
    static const uint32_t MODULE_SLOT = 1;

  public:
    static const Class class_;

    static const uint32_t RESERVED_SLOTS = 2;

    static ModuleEnvironmentObject* create(ExclusiveContext* cx, HandleModuleObject module);
    ModuleObject& module();
    IndirectBindingMap& importBindings();

    bool createImportBinding(JSContext* cx, HandleAtom importName, HandleModuleObject module,
                             HandleAtom exportName);

    bool hasImportBinding(HandlePropertyName name);

    bool lookupImport(jsid name, ModuleEnvironmentObject** envOut, Shape** shapeOut);

  private:
    static bool lookupProperty(JSContext* cx, HandleObject obj, HandleId id,
                               MutableHandleObject objp, MutableHandleShape propp);
    static bool hasProperty(JSContext* cx, HandleObject obj, HandleId id, bool* foundp);
    static bool getProperty(JSContext* cx, HandleObject obj, HandleValue receiver, HandleId id,
                            MutableHandleValue vp);
    static bool setProperty(JSContext* cx, HandleObject obj, HandleId id, HandleValue v,
                            HandleValue receiver, JS::ObjectOpResult& result);
    static bool getOwnPropertyDescriptor(JSContext* cx, HandleObject obj, HandleId id,
                                         MutableHandle<JSPropertyDescriptor> desc);
    static bool deleteProperty(JSContext* cx, HandleObject obj, HandleId id,
                               ObjectOpResult& result);
    static bool enumerate(JSContext* cx, HandleObject obj, AutoIdVector& properties,
                          bool enumerableOnly);
};

typedef Rooted<ModuleEnvironmentObject*> RootedModuleEnvironmentObject;
typedef Handle<ModuleEnvironmentObject*> HandleModuleEnvironmentObject;
typedef MutableHandle<ModuleEnvironmentObject*> MutableHandleModuleEnvironmentObject;

class DeclEnvObject : public ScopeObject
{
    // Pre-allocated slot for the named lambda.
    static const uint32_t LAMBDA_SLOT = 1;

  public:
    static const uint32_t RESERVED_SLOTS = 2;
    static const Class class_;

    static DeclEnvObject*
    createTemplateObject(JSContext* cx, HandleFunction fun, NewObjectKind newKind);

    static DeclEnvObject* create(JSContext* cx, HandleObject enclosing, HandleFunction callee);

    static inline size_t lambdaSlot() {
        return LAMBDA_SLOT;
    }
};

// Static eval scope placeholder objects on the static scope chain. Created at
// the time of compiling the eval script, and set as its static enclosing
// scope.
class StaticEvalObject : public ScopeObject
{
    static const uint32_t STRICT_SLOT = 1;

  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const Class class_;

    static StaticEvalObject* create(JSContext* cx, HandleObject enclosing);

    JSObject* enclosingScopeForStaticScopeIter() {
        return getReservedSlot(SCOPE_CHAIN_SLOT).toObjectOrNull();
    }

    void setStrict() {
        setReservedSlot(STRICT_SLOT, BooleanValue(true));
    }

    bool isStrict() const {
        return getReservedSlot(STRICT_SLOT).isTrue();
    }

    inline bool isNonGlobal() const;
};

/*
 * Non-syntactic scope objects
 *
 * A non-syntactic scope is one that was not created due to source code. On
 * the static scope chain, a single StaticNonSyntacticScopeObjects maps to 0+
 * non-syntactic dynamic scope objects. This is contrasted with syntactic
 * scopes, where each syntactic static scope corresponds to 0 or 1 dynamic
 * scope objects.
 *
 * There are 3 kinds of dynamic non-syntactic scopes:
 *
 * 1. DynamicWithObject
 *
 *    When the embedding compiles or executes a script, it has the option to
 *    pass in a vector of objects to be used as the initial scope chain. Each
 *    of those objects is wrapped by a DynamicWithObject.
 *
 *    The innermost scope passed in by the embedding becomes a qualified
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
 *    Unlike DynamicWithObjects, this object is itself the holder of 'var'
 *    bindings.
 *
 *    Does not hold 'let' or 'const' bindings.
 *
 * 3. ClonedBlockObject
 *
 *    Each non-syntactic object used as a qualified variables object needs to
 *    enclose a non-syntactic ClonedBlockObject to hold 'let' and 'const'
 *    bindings. There is a bijection per compartment between the non-syntactic
 *    variables objects and their non-syntactic ClonedBlockObjects.
 *
 *    Does not hold 'var' bindings.
 *
 * The embedding (Gecko) uses non-syntactic scopes for various things, some of
 * which are detailed below. All scope chain listings below are, from top to
 * bottom, outermost to innermost.
 *
 * A. Component loading
 *
 * Components may be loaded in "reuse loader global" mode, where to save on
 * memory, all JSMs and JS-implemented XPCOM modules are loaded into a single
 * global. Each individual JSMs are compiled as functions with their own
 * FakeBackstagePass. They have the following dynamic scope chain:
 *
 *   BackstagePass global
 *       |
 *   Global lexical scope
 *       |
 *   DynamicWithObject wrapping FakeBackstagePass
 *       |
 *   Non-syntactic lexical scope
 *
 * B. Subscript loading
 *
 * Subscripts may be loaded into a target object. They have the following
 * dynamic scope chain:
 *
 *   Loader global
 *       |
 *   Global lexical scope
 *       |
 *   DynamicWithObject wrapping target
 *       |
 *   ClonedBlockObject
 *
 * C. Frame scripts
 *
 * XUL frame scripts are always loaded with a NonSyntacticVariablesObject as a
 * "polluting global". This is done exclusively in
 * js::ExecuteInGlobalAndReturnScope.
 *
 *   Loader global
 *       |
 *   Global lexical scope
 *       |
 *   NonSyntacticVariablesObject
 *       |
 *   ClonedBlockObject
 *
 * D. XBL
 *
 * XBL methods are compiled as functions with XUL elements on the scope chain.
 * For a chain of elements e0,...,eN:
 *
 *      ...
 *       |
 *   DynamicWithObject wrapping eN
 *       |
 *      ...
 *       |
 *   DynamicWithObject wrapping e0
 *       |
 *   ClonedBlockObject
 *
 */
class StaticNonSyntacticScopeObjects : public ScopeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const Class class_;

    static StaticNonSyntacticScopeObjects* create(JSContext* cx, HandleObject enclosing);

    JSObject* enclosingScopeForStaticScopeIter() {
        return getReservedSlot(SCOPE_CHAIN_SLOT).toObjectOrNull();
    }
};

// A non-syntactic dynamic scope object that captures non-lexical
// bindings. That is, a scope object that captures both qualified var
// assignments and unqualified bareword assignments. Its parent is always the
// global lexical scope.
//
// This is used in ExecuteInGlobalAndReturnScope and sits in front of the
// global scope to capture 'var' and bareword asignments.
class NonSyntacticVariablesObject : public ScopeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const Class class_;

    static NonSyntacticVariablesObject* create(JSContext* cx,
                                               Handle<ClonedBlockObject*> globalLexical);
};

class NestedScopeObject : public ScopeObject
{
  public:
    /*
     * A refinement of enclosingScope that returns nullptr if the enclosing
     * scope is not a NestedScopeObject.
     */
    inline NestedScopeObject* enclosingNestedScope() const;

    // Return true if this object is a compile-time scope template.
    inline bool isStatic() { return !getProto(); }

    // Return the static scope corresponding to this scope chain object.
    inline NestedScopeObject* staticScope() {
        MOZ_ASSERT(!isStatic());
        return &getProto()->as<NestedScopeObject>();
    }

    // At compile-time it's possible for the scope chain to be null.
    JSObject* enclosingScopeForStaticScopeIter() {
        return getReservedSlot(SCOPE_CHAIN_SLOT).toObjectOrNull();
    }

    void initEnclosingScope(JSObject* obj) {
        MOZ_ASSERT(getReservedSlot(SCOPE_CHAIN_SLOT).isUndefined());
        setReservedSlot(SCOPE_CHAIN_SLOT, ObjectOrNullValue(obj));
    }

    /*
     * Note: in the case of hoisting, this prev-link will not ultimately be
     * the same as enclosingNestedScope; initEnclosingNestedScope must be
     * called separately in the emitter. 'reset' is just for asserting
     * stackiness.
     */
    void initEnclosingScopeFromParser(JSObject* prev) {
        setReservedSlot(SCOPE_CHAIN_SLOT, ObjectOrNullValue(prev));
    }

    void resetEnclosingScopeFromParser() {
        setReservedSlot(SCOPE_CHAIN_SLOT, UndefinedValue());
    }
};

// With scope template objects on the static scope chain.
class StaticWithObject : public NestedScopeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const Class class_;

    static StaticWithObject* create(ExclusiveContext* cx);
};

// With scope objects on the run-time scope chain.
class DynamicWithObject : public NestedScopeObject
{
    static const unsigned OBJECT_SLOT = 1;
    static const unsigned THIS_SLOT = 2;
    static const unsigned KIND_SLOT = 3;

  public:
    static const unsigned RESERVED_SLOTS = 4;
    static const Class class_;

    enum WithKind {
        SyntacticWith,
        NonSyntacticWith
    };

    static DynamicWithObject*
    create(JSContext* cx, HandleObject object, HandleObject enclosing, HandleObject staticWith,
           WithKind kind = SyntacticWith);

    StaticWithObject& staticWith() const {
        return getProto()->as<StaticWithObject>();
    }

    /* Return the 'o' in 'with (o)'. */
    JSObject& object() const {
        return getReservedSlot(OBJECT_SLOT).toObject();
    }

    /* Return object for GetThisValue. */
    JSObject* withThis() const {
        return &getReservedSlot(THIS_SLOT).toObject();
    }

    /*
     * Return whether this object is a syntactic with object.  If not, this is a
     * With object we inserted between the outermost syntactic scope and the
     * global object to wrap the scope chain someone explicitly passed via JSAPI
     * to CompileFunction or script evaluation.
     */
    bool isSyntactic() const {
        return getReservedSlot(KIND_SLOT).toInt32() == SyntacticWith;
    }

    static inline size_t objectSlot() {
        return OBJECT_SLOT;
    }

    static inline size_t thisSlot() {
        return THIS_SLOT;
    }
};

class BlockObject : public NestedScopeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const Class class_;

    /* Return the number of variables associated with this block. */
    uint32_t numVariables() const {
        // TODO: propertyCount() is O(n), use O(1) lastProperty()->slot() instead
        return propertyCount();
    }

    // Global lexical scopes are extensible. Non-global lexicals scopes are
    // not.
    bool isExtensible() const;

  protected:
    /* Blocks contain an object slot for each slot i: 0 <= i < slotCount. */
    const Value& slotValue(unsigned i) {
        return getSlotRef(RESERVED_SLOTS + i);
    }

    void setSlotValue(unsigned i, const Value& v) {
        setSlot(RESERVED_SLOTS + i, v);
    }
};

class StaticBlockObject : public BlockObject
{
    static const unsigned LOCAL_OFFSET_SLOT = 1;

  public:
    static StaticBlockObject* create(ExclusiveContext* cx);

    /* See StaticScopeIter comment. */
    JSObject* enclosingStaticScope() const {
        return getFixedSlot(SCOPE_CHAIN_SLOT).toObjectOrNull();
    }

    /*
     * Return the index (in the range [0, numVariables()) corresponding to the
     * given shape of a block object.
     */
    uint32_t shapeToIndex(const Shape& shape) {
        uint32_t slot = shape.slot();
        MOZ_ASSERT(slot - RESERVED_SLOTS < numVariables());
        return slot - RESERVED_SLOTS;
    }

    /*
     * A refinement of enclosingStaticScope that returns nullptr if the enclosing
     * static scope is a JSFunction.
     */
    inline StaticBlockObject* enclosingBlock() const;

    uint32_t localOffset() {
        return getReservedSlot(LOCAL_OFFSET_SLOT).toPrivateUint32();
    }

    // Return the local corresponding to the 'var'th binding where 'var' is in the
    // range [0, numVariables()).
    uint32_t blockIndexToLocalIndex(uint32_t index) {
        MOZ_ASSERT(index < numVariables());
        return getReservedSlot(LOCAL_OFFSET_SLOT).toPrivateUint32() + index;
    }

    // Return the slot corresponding to block index 'index', where 'index' is
    // in the range [0, numVariables()).  The result is in the range
    // [RESERVED_SLOTS, RESERVED_SLOTS + numVariables()).
    uint32_t blockIndexToSlot(uint32_t index) {
        MOZ_ASSERT(index < numVariables());
        return RESERVED_SLOTS + index;
    }

    // Return the slot corresponding to local variable 'local', where 'local' is
    // in the range [localOffset(), localOffset() + numVariables()).  The result is
    // in the range [RESERVED_SLOTS, RESERVED_SLOTS + numVariables()).
    uint32_t localIndexToSlot(uint32_t local) {
        MOZ_ASSERT(local >= localOffset());
        return blockIndexToSlot(local - localOffset());
    }

    /*
     * A let binding is aliased if accessed lexically by nested functions or
     * dynamically through dynamic name lookup (eval, with, function::, etc).
     */
    bool isAliased(unsigned i) {
        return slotValue(i).isTrue();
    }

    // Look up if the block has an aliased binding named |name|.
    Shape* lookupAliasedName(PropertyName* name);

    /*
     * A static block object is cloned (when entering the block) iff some
     * variable of the block isAliased.
     */
    bool needsClone() {
        return numVariables() > 0 && !getSlot(RESERVED_SLOTS).isFalse();
    }

    // Is this the static global lexical scope?
    bool isGlobal() const {
        return !enclosingStaticScope();
    }

    bool isSyntactic() const {
        return !isExtensible() || isGlobal();
    }

    /* Frontend-only functions ***********************************************/

    /* Initialization functions for above fields. */
    void setAliased(unsigned i, bool aliased) {
        MOZ_ASSERT_IF(i > 0, slotValue(i-1).isBoolean());
        setSlotValue(i, BooleanValue(aliased));
        if (aliased && !needsClone()) {
            setSlotValue(0, MagicValue(JS_BLOCK_NEEDS_CLONE));
            MOZ_ASSERT(needsClone());
        }
    }

    void setLocalOffset(uint32_t offset) {
        MOZ_ASSERT(getReservedSlot(LOCAL_OFFSET_SLOT).isUndefined());
        initReservedSlot(LOCAL_OFFSET_SLOT, PrivateUint32Value(offset));
    }

    /*
     * Frontend compilation temporarily uses the object's slots to link
     * a let var to its associated Definition parse node.
     */
    void setDefinitionParseNode(unsigned i, frontend::Definition* def) {
        MOZ_ASSERT(slotValue(i).isUndefined());
        setSlotValue(i, PrivateValue(def));
    }

    frontend::Definition* definitionParseNode(unsigned i) {
        Value v = slotValue(i);
        return reinterpret_cast<frontend::Definition*>(v.toPrivate());
    }

    // Called by BytecodeEmitter to mark regular block scopes as
    // non-extensible. By contrast, the global lexical scope is extensible.
    bool makeNonExtensible(ExclusiveContext* cx);

    /*
     * While ScopeCoordinate can generally reference up to 2^24 slots, block objects have an
     * additional limitation that all slot indices must be storable as uint16_t short-ids in the
     * associated Shape. If we could remove the block dependencies on shape->shortid, we could
     * remove INDEX_LIMIT.
     */
    static const unsigned LOCAL_INDEX_LIMIT = JS_BIT(16);

    static Shape* addVar(ExclusiveContext* cx, Handle<StaticBlockObject*> block, HandleId id,
                         bool constant, unsigned index, bool* redeclared);
};

class ClonedBlockObject : public BlockObject
{
    static const unsigned THIS_VALUE_SLOT = 1;

    static ClonedBlockObject* create(JSContext* cx, Handle<StaticBlockObject*> block,
                                     HandleObject enclosing);

  public:
    static ClonedBlockObject* create(JSContext* cx, Handle<StaticBlockObject*> block,
                                     AbstractFramePtr frame);

    static ClonedBlockObject* createGlobal(JSContext* cx, Handle<GlobalObject*> global);

    static ClonedBlockObject* createNonSyntactic(JSContext* cx, HandleObject enclosingStatic,
                                                 HandleObject enclosingScope);

    static ClonedBlockObject* createHollowForDebug(JSContext* cx,
                                                   Handle<StaticBlockObject*> block);

    /* The static block from which this block was cloned. */
    StaticBlockObject& staticBlock() const {
        return getProto()->as<StaticBlockObject>();
    }

    /* Assuming 'put' has been called, return the value of the ith let var. */
    const Value& var(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) {
        MOZ_ASSERT_IF(checkAliasing, staticBlock().isAliased(i));
        return slotValue(i);
    }

    void setVar(unsigned i, const Value& v, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) {
        MOZ_ASSERT_IF(checkAliasing, staticBlock().isAliased(i));
        setSlotValue(i, v);
    }

    // Is this the global lexical scope?
    bool isGlobal() const {
        MOZ_ASSERT_IF(staticBlock().isGlobal(), enclosingScope().is<GlobalObject>());
        return enclosingScope().is<GlobalObject>();
    }

    GlobalObject& global() const {
        MOZ_ASSERT(isGlobal());
        return enclosingScope().as<GlobalObject>();
    }

    bool isSyntactic() const {
        return !isExtensible() || isGlobal();
    }

    /* Copy in all the unaliased formals and locals. */
    void copyUnaliasedValues(AbstractFramePtr frame);

    /*
     * Create a new ClonedBlockObject with the same enclosing scope and
     * variable values as this.
     */
    static ClonedBlockObject* clone(JSContext* cx, Handle<ClonedBlockObject*> block);

    Value thisValue() const;
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
class RuntimeLexicalErrorObject : public ScopeObject
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

template<XDRMode mode>
bool
XDRStaticBlockObject(XDRState<mode>* xdr, HandleObject enclosingScope,
                     MutableHandle<StaticBlockObject*> objp);

template<XDRMode mode>
bool
XDRStaticWithObject(XDRState<mode>* xdr, HandleObject enclosingScope,
                    MutableHandle<StaticWithObject*> objp);

extern JSObject*
CloneNestedScopeObject(JSContext* cx, HandleObject enclosingScope, Handle<NestedScopeObject*> src);

/*****************************************************************************/

// A scope iterator describes the active scopes starting from a dynamic scope,
// static scope pair. This pair may be derived from the current point of
// execution in a frame. If derived in such a fashion, the ScopeIter tracks
// whether the current scope is within the extent of this initial frame.
// Here, "frame" means a single activation of: a function, eval, or global
// code.
class MOZ_RAII ScopeIter
{
    StaticScopeIter<CanGC> ssi_;
    RootedObject scope_;
    AbstractFramePtr frame_;

    void incrementStaticScopeIter();
    void settle();

    // No value semantics.
    ScopeIter(const ScopeIter& si) = delete;

  public:
    // Constructing from a copy of an existing ScopeIter.
    ScopeIter(JSContext* cx, const ScopeIter& si
              MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    // Constructing from a dynamic scope, static scope pair. All scopes are
    // considered not to be withinInitialFrame, since no frame is given.
    ScopeIter(JSContext* cx, JSObject* scope, JSObject* staticScope
              MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    // Constructing from a frame. Places the ScopeIter on the innermost scope
    // at pc.
    ScopeIter(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc
              MOZ_GUARD_OBJECT_NOTIFIER_PARAM);

    inline bool done() const;
    ScopeIter& operator++();

    // If done():
    inline JSObject& enclosingScope() const;

    // If !done():
    enum Type { Module, Call, Block, With, Eval, NonSyntactic };
    Type type() const;

    inline bool hasNonSyntacticScopeObject() const;
    inline bool hasSyntacticScopeObject() const;
    inline bool hasAnyScopeObject() const;
    inline bool canHaveSyntacticScopeObject() const;
    ScopeObject& scope() const;

    JSObject* maybeStaticScope() const;
    StaticBlockObject& staticBlock() const { return ssi_.block(); }
    StaticWithObject& staticWith() const { return ssi_.staticWith(); }
    StaticEvalObject& staticEval() const { return ssi_.eval(); }
    StaticNonSyntacticScopeObjects& staticNonSyntactic() const { return ssi_.nonSyntactic(); }
    JSFunction& fun() const { return ssi_.fun(); }
    ModuleObject& module() const { return ssi_.module(); }

    bool withinInitialFrame() const { return !!frame_; }
    AbstractFramePtr initialFrame() const { MOZ_ASSERT(withinInitialFrame()); return frame_; }
    AbstractFramePtr maybeInitialFrame() const { return frame_; }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

// The key in MissingScopeMap. For live frames, maps live frames to their
// synthesized scopes. For completely optimized-out scopes, maps the static
// scope objects to their synthesized scopes. The scopes we synthesize for
// static scope objects are read-only, and we never use their parent links, so
// they don't need to be distinct.
//
// That is, completely optimized out scopes can't be distinguished by
// frame. Note that even if the frame corresponding to the static scope is
// live on the stack, it is unsound to synthesize a scope from that live
// frame. In other words, the provenance of the scope chain is from allocated
// closures (i.e., allocation sites) and is irrecoverable from simple stack
// inspection (i.e., call sites).
class MissingScopeKey
{
    friend class LiveScopeVal;

    AbstractFramePtr frame_;
    JSObject* staticScope_;

  public:
    explicit MissingScopeKey(const ScopeIter& si)
      : frame_(si.maybeInitialFrame()),
        staticScope_(si.maybeStaticScope())
    { }

    AbstractFramePtr frame() const { return frame_; }
    JSObject* staticScope() const { return staticScope_; }

    void updateStaticScope(JSObject* obj) { staticScope_ = obj; }
    void updateFrame(AbstractFramePtr frame) { frame_ = frame; }

    // For use as hash policy.
    typedef MissingScopeKey Lookup;
    static HashNumber hash(MissingScopeKey sk);
    static bool match(MissingScopeKey sk1, MissingScopeKey sk2);
    bool operator!=(const MissingScopeKey& other) const {
        return frame_ != other.frame_ || staticScope_ != other.staticScope_;
    }
    static void rekey(MissingScopeKey& k, const MissingScopeKey& newKey) {
        k = newKey;
    }
};

// The value in LiveScopeMap, mapped from by live scope objects.
class LiveScopeVal
{
    friend class DebugScopes;
    friend class MissingScopeKey;

    AbstractFramePtr frame_;
    RelocatablePtrObject staticScope_;

    static void staticAsserts();

  public:
    explicit LiveScopeVal(const ScopeIter& si)
      : frame_(si.initialFrame()),
        staticScope_(si.maybeStaticScope())
    { }

    AbstractFramePtr frame() const { return frame_; }
    JSObject* staticScope() const { return staticScope_; }

    void updateFrame(AbstractFramePtr frame) { frame_ = frame; }

    bool needsSweep();
};

/*****************************************************************************/

/*
 * Debug scope objects
 *
 * The debugger effectively turns every opcode into a potential direct eval.
 * Naively, this would require creating a ScopeObject for every call/block
 * scope and using JSOP_GETALIASEDVAR for every access. To optimize this, the
 * engine assumes there is no debugger and optimizes scope access and creation
 * accordingly. When the debugger wants to perform an unexpected eval-in-frame
 * (or other, similar dynamic-scope-requiring operations), fp->scopeChain is
 * now incomplete: it may not contain all, or any, of the ScopeObjects to
 * represent the current scope.
 *
 * To resolve this, the debugger first calls GetDebugScopeFor* to synthesize a
 * "debug scope chain". A debug scope chain is just a chain of objects that
 * fill in missing scopes and protect the engine from unexpected access. (The
 * latter means that some debugger operations, like redefining a lexical
 * binding, can fail when a true eval would succeed.) To do both of these
 * things, GetDebugScopeFor* creates a new proxy DebugScopeObject to sit in
 * front of every existing ScopeObject.
 *
 * GetDebugScopeFor* ensures the invariant that the same DebugScopeObject is
 * always produced for the same underlying scope (optimized or not!). This is
 * maintained by some bookkeeping information stored in DebugScopes.
 */

extern JSObject*
GetDebugScopeForFunction(JSContext* cx, HandleFunction fun);

extern JSObject*
GetDebugScopeForFrame(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc);

extern JSObject*
GetDebugScopeForGlobalLexicalScope(JSContext* cx);

/* Provides debugger access to a scope. */
class DebugScopeObject : public ProxyObject
{
    /*
     * The enclosing scope on the dynamic scope chain. This slot is analogous
     * to the SCOPE_CHAIN_SLOT of a ScopeObject.
     */
    static const unsigned ENCLOSING_EXTRA = 0;

    /*
     * NullValue or a dense array holding the unaliased variables of a function
     * frame that has been popped.
     */
    static const unsigned SNAPSHOT_EXTRA = 1;

  public:
    static DebugScopeObject* create(JSContext* cx, ScopeObject& scope, HandleObject enclosing);

    ScopeObject& scope() const;
    JSObject& enclosingScope() const;

    /* May only be called for proxies to function call objects. */
    ArrayObject* maybeSnapshot() const;
    void initSnapshot(ArrayObject& snapshot);

    /* Currently, the 'declarative' scopes are Call and Block. */
    bool isForDeclarative() const;

    // Get a property by 'id', but returns sentinel values instead of throwing
    // on exceptional cases.
    bool getMaybeSentinelValue(JSContext* cx, HandleId id, MutableHandleValue vp);

    // Returns true iff this is a function scope with its own this-binding
    // (all functions except arrow functions and generator expression lambdas).
    bool isFunctionScopeWithThis();

    // Does this debug scope not have a dynamic counterpart or was never live
    // (and thus does not have a synthesized ScopeObject or a snapshot)?
    bool isOptimizedOut() const;
};

/* Maintains per-compartment debug scope bookkeeping information. */
class DebugScopes
{
    /* The map from (non-debug) scopes to debug scopes. */
    ObjectWeakMap proxiedScopes;

    /*
     * The map from live frames which have optimized-away scopes to the
     * corresponding debug scopes.
     */
    typedef HashMap<MissingScopeKey,
                    ReadBarrieredDebugScopeObject,
                    MissingScopeKey,
                    RuntimeAllocPolicy> MissingScopeMap;
    MissingScopeMap missingScopes;

    /*
     * The map from scope objects of live frames to the live frame. This map
     * updated lazily whenever the debugger needs the information. In between
     * two lazy updates, liveScopes becomes incomplete (but not invalid, onPop*
     * removes scopes as they are popped). Thus, two consecutive debugger lazy
     * updates of liveScopes need only fill in the new scopes.
     */
    typedef GCHashMap<ReadBarriered<ScopeObject*>,
                      LiveScopeVal,
                      MovableCellHasher<ReadBarriered<ScopeObject*>>,
                      RuntimeAllocPolicy> LiveScopeMap;
    LiveScopeMap liveScopes;
    static MOZ_ALWAYS_INLINE void liveScopesPostWriteBarrier(JSRuntime* rt, LiveScopeMap* map,
                                                             ScopeObject* key);

  public:
    explicit DebugScopes(JSContext* c);
    ~DebugScopes();

  private:
    bool init();

    static DebugScopes* ensureCompartmentData(JSContext* cx);

  public:
    void mark(JSTracer* trc);
    void sweep(JSRuntime* rt);
#ifdef JS_GC_ZEAL
    void checkHashTablesAfterMovingGC(JSRuntime* rt);
#endif

    static DebugScopeObject* hasDebugScope(JSContext* cx, ScopeObject& scope);
    static bool addDebugScope(JSContext* cx, ScopeObject& scope, DebugScopeObject& debugScope);

    static DebugScopeObject* hasDebugScope(JSContext* cx, const ScopeIter& si);
    static bool addDebugScope(JSContext* cx, const ScopeIter& si, DebugScopeObject& debugScope);

    static bool updateLiveScopes(JSContext* cx);
    static LiveScopeVal* hasLiveScope(ScopeObject& scope);
    static void unsetPrevUpToDateUntil(JSContext* cx, AbstractFramePtr frame);

    // When a frame bails out from Ion to Baseline, there might be missing
    // scopes keyed on, and live scopes containing, the old
    // RematerializedFrame. Forward those values to the new BaselineFrame.
    static void forwardLiveFrame(JSContext* cx, AbstractFramePtr from, AbstractFramePtr to);

    // In debug-mode, these must be called whenever exiting a scope that might
    // have stack-allocated locals.
    static void onPopCall(AbstractFramePtr frame, JSContext* cx);
    static void onPopBlock(JSContext* cx, const ScopeIter& si);
    static void onPopBlock(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc);
    static void onPopWith(AbstractFramePtr frame);
    static void onPopStrictEvalScope(AbstractFramePtr frame);
    static void onCompartmentUnsetIsDebuggee(JSCompartment* c);
};

}  /* namespace js */

template<>
inline bool
JSObject::is<js::NestedScopeObject>() const
{
    return is<js::BlockObject>() ||
           is<js::StaticWithObject>() ||
           is<js::DynamicWithObject>();
}

template<>
inline bool
JSObject::is<js::LexicalScopeBase>() const
{
    return is<js::CallObject>() ||
           is<js::ModuleEnvironmentObject>();
}

template<>
inline bool
JSObject::is<js::ScopeObject>() const
{
    return is<js::LexicalScopeBase>() ||
           is<js::DeclEnvObject>() ||
           is<js::NestedScopeObject>() ||
           is<js::RuntimeLexicalErrorObject>() ||
           is<js::NonSyntacticVariablesObject>();
}

template<>
bool
JSObject::is<js::DebugScopeObject>() const;

template<>
inline bool
JSObject::is<js::ClonedBlockObject>() const
{
    return is<js::BlockObject>() && !!getProto();
}

template<>
inline bool
JSObject::is<js::StaticBlockObject>() const
{
    return is<js::BlockObject>() && !getProto();
}

namespace js {

inline bool
IsSyntacticScope(JSObject* scope)
{
    if (!scope->is<ScopeObject>())
        return false;

    if (scope->is<DynamicWithObject>())
        return scope->as<DynamicWithObject>().isSyntactic();

    if (scope->is<ClonedBlockObject>())
        return scope->as<ClonedBlockObject>().isSyntactic();

    if (scope->is<NonSyntacticVariablesObject>())
        return false;

    return true;
}

inline bool
IsExtensibleLexicalScope(JSObject* scope)
{
    return scope->is<ClonedBlockObject>() && scope->as<ClonedBlockObject>().isExtensible();
}

inline bool
IsGlobalLexicalScope(JSObject* scope)
{
    return scope->is<ClonedBlockObject>() && scope->as<ClonedBlockObject>().isGlobal();
}

inline bool
IsStaticGlobalLexicalScope(JSObject* scope)
{
    return scope->is<StaticBlockObject>() && scope->as<StaticBlockObject>().isGlobal();
}

inline const Value&
ScopeObject::aliasedVar(ScopeCoordinate sc)
{
    MOZ_ASSERT(is<LexicalScopeBase>() || is<ClonedBlockObject>());
    return getSlot(sc.slot());
}

inline NestedScopeObject*
NestedScopeObject::enclosingNestedScope() const
{
    JSObject* obj = getReservedSlot(SCOPE_CHAIN_SLOT).toObjectOrNull();
    return obj && obj->is<NestedScopeObject>() ? &obj->as<NestedScopeObject>() : nullptr;
}

inline bool
StaticEvalObject::isNonGlobal() const
{
    if (isStrict())
        return true;
    return !IsStaticGlobalLexicalScope(&getReservedSlot(SCOPE_CHAIN_SLOT).toObject());
}

inline bool
ScopeIter::done() const
{
    return ssi_.done();
}

inline bool
ScopeIter::hasSyntacticScopeObject() const
{
    return ssi_.hasSyntacticDynamicScopeObject();
}

inline bool
ScopeIter::hasNonSyntacticScopeObject() const
{
    // The case we're worrying about here is a NonSyntactic static scope which
    // has 0+ corresponding non-syntactic DynamicWithObject scopes, a
    // NonSyntacticVariablesObject, or a non-syntactic ClonedBlockObject.
    if (ssi_.type() == StaticScopeIter<CanGC>::NonSyntactic) {
        MOZ_ASSERT_IF(scope_->is<DynamicWithObject>(),
                      !scope_->as<DynamicWithObject>().isSyntactic());
        return scope_->is<ScopeObject>() && !IsSyntacticScope(scope_);
    }
    return false;
}

inline bool
ScopeIter::hasAnyScopeObject() const
{
    return hasSyntacticScopeObject() || hasNonSyntacticScopeObject();
}

inline bool
ScopeIter::canHaveSyntacticScopeObject() const
{
    if (ssi_.done())
        return false;

    switch (type()) {
      case Module:
      case Call:
      case Block:
      case With:
        return true;

      case Eval:
        // Only strict eval scopes can have dynamic scope objects.
        return staticEval().isStrict();

      case NonSyntactic:
        return false;
    }

    // Silence warnings.
    return false;
}

inline JSObject&
ScopeIter::enclosingScope() const
{
    // As an engine invariant (maintained internally and asserted by Execute),
    // ScopeObjects and non-ScopeObjects cannot be interleaved on the scope
    // chain; every scope chain must start with zero or more ScopeObjects and
    // terminate with one or more non-ScopeObjects (viz., GlobalObject).
    MOZ_ASSERT(done());
    MOZ_ASSERT(!IsSyntacticScope(scope_));
    return *scope_;
}

extern bool
CreateScopeObjectsForScopeChain(JSContext* cx, AutoObjectVector& scopeChain,
                                HandleObject dynamicTerminatingScope,
                                MutableHandleObject dynamicScopeObj);

bool HasNonSyntacticStaticScopeChain(JSObject* staticScope);
uint32_t StaticScopeChainLength(JSObject* staticScope);

ModuleEnvironmentObject* GetModuleEnvironmentForScript(JSScript* script);

bool GetThisValueForDebuggerMaybeOptimizedOut(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc,
                                              MutableHandleValue res);

bool CheckVarNameConflict(JSContext* cx, Handle<ClonedBlockObject*> lexicalScope,
                          HandlePropertyName name);

bool CheckLexicalNameConflict(JSContext* cx, Handle<ClonedBlockObject*> lexicalScope,
                              HandleObject varObj, HandlePropertyName name);

bool CheckGlobalDeclarationConflicts(JSContext* cx, HandleScript script,
                                     Handle<ClonedBlockObject*> lexicalScope,
                                     HandleObject varObj);

bool CheckEvalDeclarationConflicts(JSContext* cx, HandleScript script,
                                   HandleObject scopeChain, HandleObject varObj);

#ifdef DEBUG
void DumpStaticScopeChain(JSScript* script);
void DumpStaticScopeChain(JSObject* staticScope);
bool
AnalyzeEntrainedVariables(JSContext* cx, HandleScript script);
#endif

} // namespace js

#endif /* vm_ScopeObject_h */
