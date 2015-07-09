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

#include "gc/Barrier.h"
#include "vm/ArgumentsObject.h"
#include "vm/ProxyObject.h"

namespace js {

namespace frontend { struct Definition; }

class StaticWithObject;
class StaticEvalObject;

/*****************************************************************************/

/*
 * All function scripts have an "enclosing static scope" that refers to the
 * innermost enclosing let or function in the program text. This allows full
 * reconstruction of the lexical scope for debugging or compiling efficient
 * access to variables in enclosing scopes. The static scope is represented at
 * runtime by a tree of compiler-created objects representing each scope:
 *  - a StaticBlockObject is created for 'let' and 'catch' scopes
 *  - a JSFunction+JSScript+Bindings trio is created for function scopes
 * (These objects are primarily used to clone objects scopes for the
 * dynamic scope chain.)
 *
 * There is an additional scope for named lambdas. E.g., in:
 *
 *   (function f() { var x; function g() { } })
 *
 * g's innermost enclosing scope will first be the function scope containing
 * 'x', enclosed by a scope containing only the name 'f'. (This separate scope
 * is necessary due to the fact that declarations in the function scope shadow
 * (dynamically, in the case of 'eval') the lambda name.)
 */
template <AllowGC allowGC>
class StaticScopeIter
{
    typename MaybeRooted<JSObject*, allowGC>::RootType obj;
    bool onNamedLambda;

  public:
    StaticScopeIter(ExclusiveContext* cx, JSObject* obj)
      : obj(cx, obj), onNamedLambda(false)
    {
        static_assert(allowGC == CanGC,
                      "the context-accepting constructor should only be used "
                      "in CanGC code");
        MOZ_ASSERT_IF(obj,
                      obj->is<StaticBlockObject>() ||
                      obj->is<StaticWithObject>() ||
                      obj->is<StaticEvalObject>() ||
                      obj->is<JSFunction>());
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
        MOZ_ASSERT_IF(obj,
                      obj->is<StaticBlockObject>() ||
                      obj->is<StaticWithObject>() ||
                      obj->is<StaticEvalObject>() ||
                      obj->is<JSFunction>());
    }

    explicit StaticScopeIter(const StaticScopeIter<NoGC>& ssi)
      : obj((ExclusiveContext*) nullptr, ssi.obj), onNamedLambda(ssi.onNamedLambda)
    {
        static_assert(allowGC == NoGC,
                      "the constructor not taking a context should only be "
                      "used in NoGC code");
    }

    bool done() const;
    void operator++(int);

    /* Return whether this static scope will be on the dynamic scope chain. */
    bool hasDynamicScopeObject() const;
    Shape* scopeShape() const;

    enum Type { Function, Block, With, NamedLambda, Eval };
    Type type() const;

    StaticBlockObject& block() const;
    StaticWithObject& staticWith() const;
    StaticEvalObject& eval() const;
    JSScript* funScript() const;
    JSFunction& fun() const;
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
 *     \
 *   ScopeObject                   Engine-internal scope
 *   \   \   \   \
 *    \   \   \  StaticEvalObject  Placeholder so eval scopes may be iterated through
 *     \   \   \
 *      \   \  DeclEnvObject       Holds name of recursive/heavyweight named lambda
 *       \   \
 *        \  CallObject            Scope of entire function or strict eval
 *         \
 *   NestedScopeObject             Scope created for a statement
 *     \   \  \
 *      \   \ StaticWithObject     Template for "with" object in static scope chain
 *       \   \
 *        \  DynamicWithObject     Run-time "with" object on scope chain
 *         \
 *   BlockObject                   Shared interface of cloned/static block objects
 *     \   \
 *      \  ClonedBlockObject       let, switch, catch, for
 *       \
 *       StaticBlockObject         See NB
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

class CallObject : public ScopeObject
{
    static const uint32_t CALLEE_SLOT = 1;

    static CallObject*
    create(JSContext* cx, HandleScript script, HandleObject enclosing, HandleFunction callee);

    inline void initRemainingSlotsToUninitializedLexicals(uint32_t begin);
    inline void initAliasedLexicalsToThrowOnTouch(JSScript* script);

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
        return getFixedSlot(CALLEE_SLOT).toObject().as<JSFunction>();
    }

    /* Get/set the aliased variable referred to by 'bi'. */
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

    /* For jit access. */
    static size_t offsetOfCallee() {
        return getFixedSlotOffset(CALLEE_SLOT);
    }

    static size_t calleeSlot() {
        return CALLEE_SLOT;
    }
};

class DeclEnvObject : public ScopeObject
{
    // Pre-allocated slot for the named lambda.
    static const uint32_t LAMBDA_SLOT = 1;

  public:
    static const uint32_t RESERVED_SLOTS = 2;
    static const gc::AllocKind FINALIZE_KIND = gc::FINALIZE_OBJECT2_BACKGROUND;

    static const Class class_;

    static DeclEnvObject*
    createTemplateObject(JSContext* cx, HandleFunction fun, gc::InitialHeap heap);

    static DeclEnvObject* create(JSContext* cx, HandleObject enclosing, HandleFunction callee);

    static inline size_t lambdaSlot() {
        return LAMBDA_SLOT;
    }
};

// Static eval scope template objects on the static scope. Created at the
// time of compiling the eval script, and set as its static enclosing scope.
class StaticEvalObject : public ScopeObject
{
    static const uint32_t STRICT_SLOT = 1;

  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const gc::AllocKind FINALIZE_KIND = gc::FINALIZE_OBJECT2_BACKGROUND;

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

    // Indirect evals terminate in the global at run time, and has no static
    // enclosing scope.
    bool isDirect() const {
        return getReservedSlot(SCOPE_CHAIN_SLOT).isObject();
    }
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

    void initEnclosingNestedScope(JSObject* obj) {
        MOZ_ASSERT(getReservedSlot(SCOPE_CHAIN_SLOT).isUndefined());
        setReservedSlot(SCOPE_CHAIN_SLOT, ObjectOrNullValue(obj));
    }

    /*
     * The parser uses 'enclosingNestedScope' as the prev-link in the
     * pc->staticScope stack. Note: in the case of hoisting, this prev-link will
     * not ultimately be the same as enclosingNestedScope;
     * initEnclosingNestedScope must be called separately in the
     * emitter. 'reset' is just for asserting stackiness.
     */
    void initEnclosingNestedScopeFromParser(NestedScopeObject* prev) {
        setReservedSlot(SCOPE_CHAIN_SLOT, ObjectOrNullValue(prev));
    }

    void resetEnclosingNestedScopeFromParser() {
        setReservedSlot(SCOPE_CHAIN_SLOT, UndefinedValue());
    }
};

// With scope template objects on the static scope chain.
class StaticWithObject : public NestedScopeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const gc::AllocKind FINALIZE_KIND = gc::FINALIZE_OBJECT2_BACKGROUND;

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
    static const gc::AllocKind FINALIZE_KIND = gc::FINALIZE_OBJECT4_BACKGROUND;

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

    /* Return object for the 'this' class hook. */
    JSObject& withThis() const {
        return getReservedSlot(THIS_SLOT).toObject();
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
  protected:
    static const unsigned DEPTH_SLOT = 1;

  public:
    static const unsigned RESERVED_SLOTS = 2;
    static const gc::AllocKind FINALIZE_KIND = gc::FINALIZE_OBJECT4_BACKGROUND;

    static const Class class_;

    /* Return the abstract stack depth right before entering this nested scope. */
    uint32_t stackDepth() const {
        return getReservedSlot(DEPTH_SLOT).toPrivateUint32();
    }

    /* Return the number of variables associated with this block. */
    uint32_t numVariables() const {
        // TODO: propertyCount() is O(n), use O(1) lastProperty()->slot() instead
        return propertyCount();
    }

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

    // Return the slot corresponding to local variable 'local', where 'local' is
    // in the range [localOffset(), localOffset() + numVariables()).  The result is
    // in the range [RESERVED_SLOTS, RESERVED_SLOTS + numVariables()).
    uint32_t localIndexToSlot(uint32_t local) {
        MOZ_ASSERT(local >= localOffset());
        local -= localOffset();
        MOZ_ASSERT(local < numVariables());
        return RESERVED_SLOTS + local;
    }

    /*
     * A let binding is aliased if accessed lexically by nested functions or
     * dynamically through dynamic name lookup (eval, with, function::, etc).
     */
    bool isAliased(unsigned i) {
        return slotValue(i).isTrue();
    }

    /*
     * A static block object is cloned (when entering the block) iff some
     * variable of the block isAliased.
     */
    bool needsClone() {
        return !getFixedSlot(RESERVED_SLOTS).isFalse();
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
    static ClonedBlockObject* create(JSContext* cx, Handle<StaticBlockObject*> block,
                                     HandleObject enclosing);

  public:
    static ClonedBlockObject* create(JSContext* cx, Handle<StaticBlockObject*> block,
                                     AbstractFramePtr frame);

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

    /* Copy in all the unaliased formals and locals. */
    void copyUnaliasedValues(AbstractFramePtr frame);
};

// Internal scope object used by JSOP_BINDNAME upon encountering an
// uninitialized lexical slot.
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
class UninitializedLexicalObject : public ScopeObject
{
  public:
    static const unsigned RESERVED_SLOTS = 1;
    static const gc::AllocKind FINALIZE_KIND = gc::FINALIZE_OBJECT2_BACKGROUND;

    static const Class class_;

    static UninitializedLexicalObject* create(JSContext* cx, HandleObject enclosing);
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
class ScopeIter
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
    enum Type { Call, Block, With, Eval };
    Type type() const;

    inline bool hasScopeObject() const;
    inline bool canHaveScopeObject() const;
    ScopeObject& scope() const;

    JSObject* maybeStaticScope() const;
    StaticBlockObject& staticBlock() const { return ssi_.block(); }
    StaticWithObject& staticWith() const { return ssi_.staticWith(); }
    StaticEvalObject& staticEval() const { return ssi_.eval(); }
    JSFunction& fun() const { return ssi_.fun(); }

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
// That is, completely optimized out scopes have can't be distinguished by
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

    void sweep();
    static void staticAsserts();

  public:
    explicit LiveScopeVal(const ScopeIter& si)
      : frame_(si.initialFrame()),
        staticScope_(si.maybeStaticScope())
    { }

    AbstractFramePtr frame() const { return frame_; }
    JSObject* staticScope() const { return staticScope_; }

    void updateFrame(AbstractFramePtr frame) { frame_ = frame; }
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
 * To resolve this, the debugger first calls GetDebugScopeFor(Function|Frame)
 * to synthesize a "debug scope chain". A debug scope chain is just a chain of
 * objects that fill in missing scopes and protect the engine from unexpected
 * access. (The latter means that some debugger operations, like redefining a
 * lexical binding, can fail when a true eval would succeed.) To do both of
 * these things, GetDebugScopeFor* creates a new proxy DebugScopeObject to sit
 * in front of every existing ScopeObject.
 *
 * GetDebugScopeFor* ensures the invariant that the same DebugScopeObject is
 * always produced for the same underlying scope (optimized or not!). This is
 * maintained by some bookkeeping information stored in DebugScopes.
 */

extern JSObject*
GetDebugScopeForFunction(JSContext* cx, HandleFunction fun);

extern JSObject*
GetDebugScopeForFrame(JSContext* cx, AbstractFramePtr frame, jsbytecode* pc);

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

    // Does this debug scope not have a dynamic counterpart or was never live
    // (and thus does not have a synthesized ScopeObject or a snapshot)?
    bool isOptimizedOut() const;
};

/* Maintains per-compartment debug scope bookkeeping information. */
class DebugScopes
{
    /* The map from (non-debug) scopes to debug scopes. */
    typedef WeakMap<PreBarrieredObject, RelocatablePtrObject> ObjectWeakMap;
    ObjectWeakMap proxiedScopes;
    static MOZ_ALWAYS_INLINE void proxiedScopesPostWriteBarrier(JSRuntime* rt, ObjectWeakMap* map,
                                                               const PreBarrieredObject& key);

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
    typedef HashMap<ScopeObject*,
                    LiveScopeVal,
                    DefaultHasher<ScopeObject*>,
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
JSObject::is<js::ScopeObject>() const
{
    return is<js::CallObject>() ||
           is<js::DeclEnvObject>() ||
           is<js::NestedScopeObject>() ||
           is<js::UninitializedLexicalObject>();
}

template<>
inline bool
JSObject::is<js::DebugScopeObject>() const
{
    extern bool js_IsDebugScopeSlow(js::ProxyObject* proxy);

    // Note: don't use is<ProxyObject>() here -- it also matches subclasses!
    return hasClass(&js::ProxyObject::class_) &&
           js_IsDebugScopeSlow(&const_cast<JSObject*>(this)->as<js::ProxyObject>());
}

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

inline JSObject*
JSObject::enclosingScope()
{
    return is<js::ScopeObject>()
           ? &as<js::ScopeObject>().enclosingScope()
           : is<js::DebugScopeObject>()
           ? &as<js::DebugScopeObject>().enclosingScope()
           : getParent();
}

namespace js {

inline const Value&
ScopeObject::aliasedVar(ScopeCoordinate sc)
{
    MOZ_ASSERT(is<CallObject>() || is<ClonedBlockObject>());
    return getSlot(sc.slot());
}

inline NestedScopeObject*
NestedScopeObject::enclosingNestedScope() const
{
    JSObject* obj = getReservedSlot(SCOPE_CHAIN_SLOT).toObjectOrNull();
    return obj && obj->is<NestedScopeObject>() ? &obj->as<NestedScopeObject>() : nullptr;
}

inline bool
ScopeIter::done() const
{
    return ssi_.done();
}

inline bool
ScopeIter::hasScopeObject() const
{
    return ssi_.hasDynamicScopeObject();
}

inline bool
ScopeIter::canHaveScopeObject() const
{
    // Non-strict eval scopes cannot have dynamic scope objects.
    return !ssi_.done() && (type() != Eval || staticEval().isStrict());
}

inline JSObject&
ScopeIter::enclosingScope() const
{
    // As an engine invariant (maintained internally and asserted by Execute),
    // ScopeObjects and non-ScopeObjects cannot be interleaved on the scope
    // chain; every scope chain must start with zero or more ScopeObjects and
    // terminate with one or more non-ScopeObjects (viz., GlobalObject).
    MOZ_ASSERT(done());
    MOZ_ASSERT(!scope_->is<ScopeObject>());
    return *scope_;
}

#ifdef DEBUG
bool
AnalyzeEntrainedVariables(JSContext* cx, HandleScript script);
#endif

} // namespace js

#endif /* vm_ScopeObject_h */
