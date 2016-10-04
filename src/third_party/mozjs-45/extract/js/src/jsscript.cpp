/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS script operations.
 */

#include "jsscriptinlines.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Vector.h"

#include <algorithm>
#include <string.h>

#include "jsapi.h"
#include "jsatom.h"
#include "jscntxt.h"
#include "jsfun.h"
#include "jsgc.h"
#include "jsobj.h"
#include "jsopcode.h"
#include "jsprf.h"
#include "jstypes.h"
#include "jsutil.h"
#include "jswrapper.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/SharedContext.h"
#include "gc/Marking.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/IonCode.h"
#include "js/MemoryMetrics.h"
#include "js/Utility.h"
#include "vm/ArgumentsObject.h"
#include "vm/Compression.h"
#include "vm/Debugger.h"
#include "vm/Opcodes.h"
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/Xdr.h"

#include "jsfuninlines.h"
#include "jsobjinlines.h"

#include "vm/ScopeObject-inl.h"
#include "vm/Stack-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::frontend;

using mozilla::PodCopy;
using mozilla::PodZero;
using mozilla::RotateLeft;

static BindingIter
GetBinding(HandleScript script, HandlePropertyName name)
{
    BindingIter bi(script);
    while (bi->name() != name)
        bi++;
    return bi;
}

/* static */ BindingIter
Bindings::argumentsBinding(ExclusiveContext* cx, HandleScript script)
{
    return GetBinding(script, cx->names().arguments);
}

/* static */ BindingIter
Bindings::thisBinding(ExclusiveContext* cx, HandleScript script)
{
    return GetBinding(script, cx->names().dotThis);
}

bool
Bindings::initWithTemporaryStorage(ExclusiveContext* cx, MutableHandle<Bindings> self,
                                   uint32_t numArgs, uint32_t numVars,
                                   uint32_t numBodyLevelLexicals, uint32_t numBlockScoped,
                                   uint32_t numUnaliasedVars, uint32_t numUnaliasedBodyLevelLexicals,
                                   const Binding* bindingArray, bool isModule /* = false */)
{
    MOZ_ASSERT(!self.callObjShape());
    MOZ_ASSERT(self.bindingArrayUsingTemporaryStorage());
    MOZ_ASSERT(!self.bindingArray());
    MOZ_ASSERT(!(uintptr_t(bindingArray) & TEMPORARY_STORAGE_BIT));
    MOZ_ASSERT(numArgs <= ARGC_LIMIT);
    MOZ_ASSERT(numVars <= LOCALNO_LIMIT);
    MOZ_ASSERT(numBlockScoped <= LOCALNO_LIMIT);
    MOZ_ASSERT(numBodyLevelLexicals <= LOCALNO_LIMIT);
    mozilla::DebugOnly<uint64_t> totalSlots = uint64_t(numVars) +
                                              uint64_t(numBodyLevelLexicals) +
                                              uint64_t(numBlockScoped);
    MOZ_ASSERT(totalSlots <= LOCALNO_LIMIT);
    MOZ_ASSERT(UINT32_MAX - numArgs >= totalSlots);

    MOZ_ASSERT(numUnaliasedVars <= numVars);
    MOZ_ASSERT(numUnaliasedBodyLevelLexicals <= numBodyLevelLexicals);

    self.setBindingArray(bindingArray, TEMPORARY_STORAGE_BIT);
    self.setNumArgs(numArgs);
    self.setNumVars(numVars);
    self.setNumBodyLevelLexicals(numBodyLevelLexicals);
    self.setNumBlockScoped(numBlockScoped);
    self.setNumUnaliasedVars(numUnaliasedVars);
    self.setNumUnaliasedBodyLevelLexicals(numUnaliasedBodyLevelLexicals);

    // Get the initial shape to use when creating CallObjects for this script.
    // After creation, a CallObject's shape may change completely (via direct eval() or
    // other operations that mutate the lexical scope). However, since the
    // lexical bindings added to the initial shape are permanent and the
    // allocKind/nfixed of a CallObject cannot change, one may assume that the
    // slot location (whether in the fixed or dynamic slots) of a variable is
    // the same as in the initial shape. (This is assumed by the interpreter and
    // JITs when interpreting/compiling aliasedvar ops.)

    // Since unaliased variables are, by definition, only accessed by local
    // operations and never through the scope chain, only give shapes to
    // aliased variables. While the debugger may observe any scope object at
    // any time, such accesses are mediated by DebugScopeProxy (see
    // DebugScopeProxy::handleUnaliasedAccess).
    uint32_t nslots = CallObject::RESERVED_SLOTS;
    uint32_t aliasedBodyLevelLexicalBegin = UINT16_MAX;
    for (BindingIter bi(self); bi; bi++) {
        if (bi->aliased()) {
            // Per ES6, lexical bindings cannot be accessed until
            // initialized. Remember the first aliased slot that is a
            // body-level lexical, so that they may be initialized to sentinel
            // magic values.
            if (numBodyLevelLexicals > 0 &&
                nslots < aliasedBodyLevelLexicalBegin &&
                bi.isBodyLevelLexical() &&
                bi.localIndex() >= numVars)
            {
                aliasedBodyLevelLexicalBegin = nslots;
            }

            nslots++;
        }
    }
    self.setAliasedBodyLevelLexicalBegin(aliasedBodyLevelLexicalBegin);

    // Put as many of nslots inline into the object header as possible.
    uint32_t nfixed = gc::GetGCKindSlots(gc::GetGCObjectKind(nslots));

    // Start with the empty shape and then append one shape per aliased binding.
    const Class* cls = isModule ? &ModuleEnvironmentObject::class_ : &CallObject::class_;
    uint32_t baseShapeFlags = BaseShape::QUALIFIED_VAROBJ | BaseShape::DELEGATE;
    if (isModule)
        baseShapeFlags |= BaseShape::NOT_EXTENSIBLE; // Module code is always strict.
    RootedShape shape(cx,
        EmptyShape::getInitialShape(cx, cls, TaggedProto(nullptr), nfixed, baseShapeFlags));
    if (!shape)
        return false;

#ifdef DEBUG
    HashSet<PropertyName*> added(cx);
    if (!added.init()) {
        ReportOutOfMemory(cx);
        return false;
    }
#endif

    uint32_t slot = CallObject::RESERVED_SLOTS;
    for (BindingIter bi(self); bi; bi++) {
        MOZ_ASSERT_IF(isModule, bi->aliased());
        if (!bi->aliased())
            continue;

#ifdef DEBUG
        // The caller ensures no duplicate aliased names.
        MOZ_ASSERT(!added.has(bi->name()));
        if (!added.put(bi->name())) {
            ReportOutOfMemory(cx);
            return false;
        }
#endif

        StackBaseShape stackBase(cx, cls, baseShapeFlags);
        UnownedBaseShape* base = BaseShape::getUnowned(cx, stackBase);
        if (!base)
            return false;

        unsigned attrs = JSPROP_PERMANENT |
                         JSPROP_ENUMERATE |
                         (bi->kind() == Binding::CONSTANT ? JSPROP_READONLY : 0);
        Rooted<StackShape> child(cx, StackShape(base, NameToId(bi->name()), slot, attrs, 0));

        shape = cx->compartment()->propertyTree.getChild(cx, shape, child);
        if (!shape)
            return false;

        MOZ_ASSERT(slot < nslots);
        slot++;
    }
    MOZ_ASSERT(slot == nslots);

    MOZ_ASSERT(!shape->inDictionary());
    self.setCallObjShape(shape);
    return true;
}

bool
Bindings::initTrivial(ExclusiveContext* cx)
{
    Shape* shape = EmptyShape::getInitialShape(cx, &CallObject::class_, TaggedProto(nullptr),
                                               CallObject::RESERVED_SLOTS,
                                               BaseShape::QUALIFIED_VAROBJ | BaseShape::DELEGATE);
    if (!shape)
        return false;
    callObjShape_.init(shape);
    return true;
}

uint8_t*
Bindings::switchToScriptStorage(Binding* newBindingArray)
{
    MOZ_ASSERT(bindingArrayUsingTemporaryStorage());
    MOZ_ASSERT(!(uintptr_t(newBindingArray) & TEMPORARY_STORAGE_BIT));

    if (count() > 0)
        PodCopy(newBindingArray, bindingArray(), count());
    bindingArrayAndFlag_ = uintptr_t(newBindingArray);
    return reinterpret_cast<uint8_t*>(newBindingArray + count());
}

/* static */ bool
Bindings::clone(JSContext* cx, MutableHandle<Bindings> self,
                uint8_t* dstScriptData, HandleScript srcScript)
{
    /* The clone has the same bindingArray_ offset as 'src'. */
    Handle<Bindings> src = Handle<Bindings>::fromMarkedLocation(&srcScript->bindings);
    ptrdiff_t off = (uint8_t*)src.bindingArray() - srcScript->data;
    MOZ_ASSERT(off >= 0);
    MOZ_ASSERT(size_t(off) <= srcScript->dataSize());
    Binding* dstPackedBindings = (Binding*)(dstScriptData + off);

    /*
     * Since atoms are shareable throughout the runtime, we can simply copy
     * the source's bindingArray directly.
     */
    if (!initWithTemporaryStorage(cx, self, src.numArgs(), src.numVars(),
                                  src.numBodyLevelLexicals(),
                                  src.numBlockScoped(),
                                  src.numUnaliasedVars(),
                                  src.numUnaliasedBodyLevelLexicals(),
                                  src.bindingArray()))
    {
        return false;
    }

    self.switchToScriptStorage(dstPackedBindings);
    return true;
}

template<XDRMode mode>
static bool
XDRScriptBindings(XDRState<mode>* xdr, LifoAllocScope& las, uint16_t numArgs, uint32_t numVars,
                  uint16_t numBodyLevelLexicals, uint16_t numBlockScoped,
                  uint32_t numUnaliasedVars, uint16_t numUnaliasedBodyLevelLexicals,
                  HandleScript script)
{
    JSContext* cx = xdr->cx();

    if (mode == XDR_ENCODE) {
        for (BindingIter bi(script); bi; bi++) {
            RootedAtom atom(cx, bi->name());
            if (!XDRAtom(xdr, &atom))
                return false;
        }

        for (BindingIter bi(script); bi; bi++) {
            uint8_t u8 = (uint8_t(bi->kind()) << 1) | uint8_t(bi->aliased());
            if (!xdr->codeUint8(&u8))
                return false;
        }
    } else {
        uint32_t nameCount = numArgs + numVars + numBodyLevelLexicals;

        AutoValueVector atoms(cx);
        if (!atoms.resize(nameCount))
            return false;
        for (uint32_t i = 0; i < nameCount; i++) {
            RootedAtom atom(cx);
            if (!XDRAtom(xdr, &atom))
                return false;
            atoms[i].setString(atom);
        }

        Binding* bindingArray = las.alloc().newArrayUninitialized<Binding>(nameCount);
        if (!bindingArray)
            return false;
        for (uint32_t i = 0; i < nameCount; i++) {
            uint8_t u8;
            if (!xdr->codeUint8(&u8))
                return false;

            PropertyName* name = atoms[i].toString()->asAtom().asPropertyName();
            Binding::Kind kind = Binding::Kind(u8 >> 1);
            bool aliased = bool(u8 & 1);

            bindingArray[i] = Binding(name, kind, aliased);
        }

        Rooted<Bindings> bindings(cx, script->bindings);
        if (!Bindings::initWithTemporaryStorage(cx, &bindings, numArgs, numVars,
                                                numBodyLevelLexicals, numBlockScoped,
                                                numUnaliasedVars, numUnaliasedBodyLevelLexicals,
                                                bindingArray))
        {
            return false;
        }
        script->bindings = bindings;
    }

    return true;
}

bool
Bindings::bindingIsAliased(uint32_t bindingIndex)
{
    MOZ_ASSERT(bindingIndex < count());
    return bindingArray()[bindingIndex].aliased();
}

void
Binding::trace(JSTracer* trc)
{
    PropertyName* name = this->name();
    TraceManuallyBarrieredEdge(trc, &name, "binding");
}

void
Bindings::trace(JSTracer* trc)
{
    if (callObjShape_)
        TraceEdge(trc, &callObjShape_, "callObjShape");

    /*
     * As the comment in Bindings explains, bindingsArray may point into freed
     * storage when bindingArrayUsingTemporaryStorage so we don't mark it.
     * Note: during compilation, atoms are already kept alive by gcKeepAtoms.
     */
    if (bindingArrayUsingTemporaryStorage())
        return;

    for (Binding& b : *this)
        b.trace(trc);
}

template<XDRMode mode>
bool
js::XDRScriptConst(XDRState<mode>* xdr, MutableHandleValue vp)
{
    JSContext* cx = xdr->cx();

    /*
     * A script constant can be an arbitrary primitive value as they are used
     * to implement JSOP_LOOKUPSWITCH. But they cannot be objects, see
     * bug 407186.
     */
    enum ConstTag {
        SCRIPT_INT     = 0,
        SCRIPT_DOUBLE  = 1,
        SCRIPT_ATOM    = 2,
        SCRIPT_TRUE    = 3,
        SCRIPT_FALSE   = 4,
        SCRIPT_NULL    = 5,
        SCRIPT_OBJECT  = 6,
        SCRIPT_VOID    = 7,
        SCRIPT_HOLE    = 8
    };

    uint32_t tag;
    if (mode == XDR_ENCODE) {
        if (vp.isInt32()) {
            tag = SCRIPT_INT;
        } else if (vp.isDouble()) {
            tag = SCRIPT_DOUBLE;
        } else if (vp.isString()) {
            tag = SCRIPT_ATOM;
        } else if (vp.isTrue()) {
            tag = SCRIPT_TRUE;
        } else if (vp.isFalse()) {
            tag = SCRIPT_FALSE;
        } else if (vp.isNull()) {
            tag = SCRIPT_NULL;
        } else if (vp.isObject()) {
            tag = SCRIPT_OBJECT;
        } else if (vp.isMagic(JS_ELEMENTS_HOLE)) {
            tag = SCRIPT_HOLE;
        } else {
            MOZ_ASSERT(vp.isUndefined());
            tag = SCRIPT_VOID;
        }
    }

    if (!xdr->codeUint32(&tag))
        return false;

    switch (tag) {
      case SCRIPT_INT: {
        uint32_t i;
        if (mode == XDR_ENCODE)
            i = uint32_t(vp.toInt32());
        if (!xdr->codeUint32(&i))
            return false;
        if (mode == XDR_DECODE)
            vp.set(Int32Value(int32_t(i)));
        break;
      }
      case SCRIPT_DOUBLE: {
        double d;
        if (mode == XDR_ENCODE)
            d = vp.toDouble();
        if (!xdr->codeDouble(&d))
            return false;
        if (mode == XDR_DECODE)
            vp.set(DoubleValue(d));
        break;
      }
      case SCRIPT_ATOM: {
        RootedAtom atom(cx);
        if (mode == XDR_ENCODE)
            atom = &vp.toString()->asAtom();
        if (!XDRAtom(xdr, &atom))
            return false;
        if (mode == XDR_DECODE)
            vp.set(StringValue(atom));
        break;
      }
      case SCRIPT_TRUE:
        if (mode == XDR_DECODE)
            vp.set(BooleanValue(true));
        break;
      case SCRIPT_FALSE:
        if (mode == XDR_DECODE)
            vp.set(BooleanValue(false));
        break;
      case SCRIPT_NULL:
        if (mode == XDR_DECODE)
            vp.set(NullValue());
        break;
      case SCRIPT_OBJECT: {
        RootedObject obj(cx);
        if (mode == XDR_ENCODE)
            obj = &vp.toObject();

        if (!XDRObjectLiteral(xdr, &obj))
            return false;

        if (mode == XDR_DECODE)
            vp.setObject(*obj);
        break;
      }
      case SCRIPT_VOID:
        if (mode == XDR_DECODE)
            vp.set(UndefinedValue());
        break;
      case SCRIPT_HOLE:
        if (mode == XDR_DECODE)
            vp.setMagic(JS_ELEMENTS_HOLE);
        break;
    }
    return true;
}

template bool
js::XDRScriptConst(XDRState<XDR_ENCODE>*, MutableHandleValue);

template bool
js::XDRScriptConst(XDRState<XDR_DECODE>*, MutableHandleValue);

// Code LazyScript's free variables.
template<XDRMode mode>
static bool
XDRLazyFreeVariables(XDRState<mode>* xdr, MutableHandle<LazyScript*> lazy)
{
    JSContext* cx = xdr->cx();
    RootedAtom atom(cx);
    uint8_t isHoistedUse;
    LazyScript::FreeVariable* freeVariables = lazy->freeVariables();
    size_t numFreeVariables = lazy->numFreeVariables();
    for (size_t i = 0; i < numFreeVariables; i++) {
        if (mode == XDR_ENCODE) {
            atom = freeVariables[i].atom();
            isHoistedUse = freeVariables[i].isHoistedUse();
        }

        if (!XDRAtom(xdr, &atom))
            return false;
        if (!xdr->codeUint8(&isHoistedUse))
            return false;

        if (mode == XDR_DECODE) {
            freeVariables[i] = LazyScript::FreeVariable(atom);
            if (isHoistedUse)
                freeVariables[i].setIsHoistedUse();
        }
    }

    return true;
}

// Code the missing part needed to re-create a LazyScript from a JSScript.
template<XDRMode mode>
static bool
XDRRelazificationInfo(XDRState<mode>* xdr, HandleFunction fun, HandleScript script,
                      HandleObject enclosingScope, MutableHandle<LazyScript*> lazy)
{
    MOZ_ASSERT_IF(mode == XDR_ENCODE, script->isRelazifiable() && script->maybeLazyScript());
    MOZ_ASSERT_IF(mode == XDR_ENCODE, !lazy->numInnerFunctions());

    JSContext* cx = xdr->cx();

    uint64_t packedFields;
    {
        uint32_t begin = script->sourceStart();
        uint32_t end = script->sourceEnd();
        uint32_t lineno = script->lineno();
        uint32_t column = script->column();

        if (mode == XDR_ENCODE) {
            packedFields = lazy->packedFields();
            MOZ_ASSERT(begin == lazy->begin());
            MOZ_ASSERT(end == lazy->end());
            MOZ_ASSERT(lineno == lazy->lineno());
            MOZ_ASSERT(column == lazy->column());
            // We can assert we have no inner functions because we don't
            // relazify scripts with inner functions.  See
            // JSFunction::createScriptForLazilyInterpretedFunction.
            MOZ_ASSERT(lazy->numInnerFunctions() == 0);
        }

        if (!xdr->codeUint64(&packedFields))
            return false;

        if (mode == XDR_DECODE) {
            lazy.set(LazyScript::Create(cx, fun, script, enclosingScope, script,
                                        packedFields, begin, end, lineno, column));

            // As opposed to XDRLazyScript, we need to restore the runtime bits
            // of the script, as we are trying to match the fact this function
            // has already been parsed and that it would need to be re-lazified.
            lazy->initRuntimeFields(packedFields);
        }
    }

    // Code free variables.
    if (!XDRLazyFreeVariables(xdr, lazy))
        return false;

    // No need to do anything with inner functions, since we asserted we don't
    // have any.

    return true;
}

static inline uint32_t
FindScopeObjectIndex(JSScript* script, NestedScopeObject& scope)
{
    ObjectArray* objects = script->objects();
    HeapPtrObject* vector = objects->vector;
    unsigned length = objects->length;
    for (unsigned i = 0; i < length; ++i) {
        if (vector[i] == &scope)
            return i;
    }

    MOZ_CRASH("Scope not found");
}

static bool
SaveSharedScriptData(ExclusiveContext*, Handle<JSScript*>, SharedScriptData*, uint32_t);

enum XDRClassKind {
    CK_BlockObject = 0,
    CK_WithObject  = 1,
    CK_JSFunction  = 2,
    CK_JSObject    = 3
};

template<XDRMode mode>
bool
js::XDRScript(XDRState<mode>* xdr, HandleObject enclosingScopeArg, HandleScript enclosingScript,
              HandleFunction fun, MutableHandleScript scriptp)
{
    /* NB: Keep this in sync with CopyScript. */

    MOZ_ASSERT(enclosingScopeArg);

    enum ScriptBits {
        NoScriptRval,
        SavedCallerFun,
        Strict,
        ContainsDynamicNameAccess,
        FunHasExtensibleScope,
        FunNeedsDeclEnvObject,
        FunHasAnyAliasedFormal,
        ArgumentsHasVarBinding,
        NeedsArgsObj,
        HasMappedArgsObj,
        FunctionHasThisBinding,
        IsGeneratorExp,
        IsLegacyGenerator,
        IsStarGenerator,
        OwnSource,
        ExplicitUseStrict,
        SelfHosted,
        HasSingleton,
        TreatAsRunOnce,
        HasLazyScript,
        HasNonSyntacticScope,
        HasInnerFunctions,
        NeedsHomeObject,
        IsDerivedClassConstructor,
    };

    uint32_t length, lineno, column, nslots;
    uint32_t natoms, nsrcnotes, i;
    uint32_t nconsts, nobjects, nregexps, ntrynotes, nblockscopes, nyieldoffsets;
    uint32_t prologueLength, version;
    uint32_t funLength = 0;
    uint32_t nTypeSets = 0;
    uint32_t scriptBits = 0;

    JSContext* cx = xdr->cx();
    RootedScript script(cx);
    RootedObject enclosingScope(cx, enclosingScopeArg);
    natoms = nsrcnotes = 0;
    nconsts = nobjects = nregexps = ntrynotes = nblockscopes = nyieldoffsets = 0;

    /* XDR arguments and vars. */
    uint16_t nargs = 0;
    uint16_t nblocklocals = 0;
    uint16_t nbodylevellexicals = 0;
    uint32_t nvars = 0;
    uint32_t nunaliasedvars = 0;
    uint16_t nunaliasedbodylevellexicals = 0;
    if (mode == XDR_ENCODE) {
        script = scriptp.get();
        MOZ_ASSERT_IF(enclosingScript, enclosingScript->compartment() == script->compartment());
        MOZ_ASSERT(script->functionNonDelazifying() == fun);

        if (!fun && script->treatAsRunOnce()) {
            // This is a toplevel or eval script that's runOnce.  We want to
            // make sure that we're not XDR-saving an object we emitted for
            // JSOP_OBJECT that then got modified.  So throw if we're not
            // cloning in JSOP_OBJECT or if we ever didn't clone in it in the
            // past.
            const JS::CompartmentOptions& opts = JS::CompartmentOptionsRef(cx);
            if (!opts.cloneSingletons() || !opts.getSingletonsAsTemplates()) {
                JS_ReportError(cx,
                               "Can't serialize a run-once non-function script "
                               "when we're not doing singleton cloning");
                return false;
            }
        }

        nargs = script->bindings.numArgs();
        nblocklocals = script->bindings.numBlockScoped();
        nbodylevellexicals = script->bindings.numBodyLevelLexicals();
        nvars = script->bindings.numVars();
        nunaliasedvars = script->bindings.numUnaliasedVars();
        nunaliasedbodylevellexicals = script->bindings.numUnaliasedBodyLevelLexicals();
    }
    if (!xdr->codeUint16(&nargs))
        return false;
    if (!xdr->codeUint16(&nblocklocals))
        return false;
    if (!xdr->codeUint16(&nbodylevellexicals))
        return false;
    if (!xdr->codeUint32(&nvars))
        return false;
    if (!xdr->codeUint32(&nunaliasedvars))
        return false;
    if (!xdr->codeUint16(&nunaliasedbodylevellexicals))
        return false;

    if (mode == XDR_ENCODE)
        length = script->length();
    if (!xdr->codeUint32(&length))
        return false;

    if (mode == XDR_ENCODE) {
        prologueLength = script->mainOffset();
        MOZ_ASSERT(script->getVersion() != JSVERSION_UNKNOWN);
        version = script->getVersion();
        lineno = script->lineno();
        column = script->column();
        nslots = script->nslots();
        natoms = script->natoms();

        nsrcnotes = script->numNotes();

        if (script->hasConsts())
            nconsts = script->consts()->length;
        if (script->hasObjects())
            nobjects = script->objects()->length;
        if (script->hasRegexps())
            nregexps = script->regexps()->length;
        if (script->hasTrynotes())
            ntrynotes = script->trynotes()->length;
        if (script->hasBlockScopes())
            nblockscopes = script->blockScopes()->length;
        if (script->hasYieldOffsets())
            nyieldoffsets = script->yieldOffsets().length();

        nTypeSets = script->nTypeSets();
        funLength = script->funLength();

        if (script->noScriptRval())
            scriptBits |= (1 << NoScriptRval);
        if (script->savedCallerFun())
            scriptBits |= (1 << SavedCallerFun);
        if (script->strict())
            scriptBits |= (1 << Strict);
        if (script->explicitUseStrict())
            scriptBits |= (1 << ExplicitUseStrict);
        if (script->selfHosted())
            scriptBits |= (1 << SelfHosted);
        if (script->bindingsAccessedDynamically())
            scriptBits |= (1 << ContainsDynamicNameAccess);
        if (script->funHasExtensibleScope())
            scriptBits |= (1 << FunHasExtensibleScope);
        if (script->funNeedsDeclEnvObject())
            scriptBits |= (1 << FunNeedsDeclEnvObject);
        if (script->funHasAnyAliasedFormal())
            scriptBits |= (1 << FunHasAnyAliasedFormal);
        if (script->argumentsHasVarBinding())
            scriptBits |= (1 << ArgumentsHasVarBinding);
        if (script->analyzedArgsUsage() && script->needsArgsObj())
            scriptBits |= (1 << NeedsArgsObj);
        if (script->hasMappedArgsObj())
            scriptBits |= (1 << HasMappedArgsObj);
        if (script->functionHasThisBinding())
            scriptBits |= (1 << FunctionHasThisBinding);
        if (!enclosingScript || enclosingScript->scriptSource() != script->scriptSource())
            scriptBits |= (1 << OwnSource);
        if (script->isGeneratorExp())
            scriptBits |= (1 << IsGeneratorExp);
        if (script->isLegacyGenerator())
            scriptBits |= (1 << IsLegacyGenerator);
        if (script->isStarGenerator())
            scriptBits |= (1 << IsStarGenerator);
        if (script->hasSingletons())
            scriptBits |= (1 << HasSingleton);
        if (script->treatAsRunOnce())
            scriptBits |= (1 << TreatAsRunOnce);
        if (script->isRelazifiable())
            scriptBits |= (1 << HasLazyScript);
        if (script->hasNonSyntacticScope())
            scriptBits |= (1 << HasNonSyntacticScope);
        if (script->hasInnerFunctions())
            scriptBits |= (1 << HasInnerFunctions);
        if (script->needsHomeObject())
            scriptBits |= (1 << NeedsHomeObject);
        if (script->isDerivedClassConstructor())
            scriptBits |= (1 << IsDerivedClassConstructor);
    }

    if (!xdr->codeUint32(&prologueLength))
        return false;
    if (!xdr->codeUint32(&version))
        return false;

    // To fuse allocations, we need lengths of all embedded arrays early.
    if (!xdr->codeUint32(&natoms))
        return false;
    if (!xdr->codeUint32(&nsrcnotes))
        return false;
    if (!xdr->codeUint32(&nconsts))
        return false;
    if (!xdr->codeUint32(&nobjects))
        return false;
    if (!xdr->codeUint32(&nregexps))
        return false;
    if (!xdr->codeUint32(&ntrynotes))
        return false;
    if (!xdr->codeUint32(&nblockscopes))
        return false;
    if (!xdr->codeUint32(&nyieldoffsets))
        return false;
    if (!xdr->codeUint32(&nTypeSets))
        return false;
    if (!xdr->codeUint32(&funLength))
        return false;
    if (!xdr->codeUint32(&scriptBits))
        return false;

    if (mode == XDR_DECODE) {
        JSVersion version_ = JSVersion(version);
        MOZ_ASSERT((version_ & VersionFlags::MASK) == unsigned(version_));

        CompileOptions options(cx);
        options.setVersion(version_)
               .setNoScriptRval(!!(scriptBits & (1 << NoScriptRval)))
               .setSelfHostingMode(!!(scriptBits & (1 << SelfHosted)));
        RootedScriptSource sourceObject(cx);
        if (scriptBits & (1 << OwnSource)) {
            ScriptSource* ss = cx->new_<ScriptSource>();
            if (!ss)
                return false;
            ScriptSourceHolder ssHolder(ss);

            /*
             * We use this CompileOptions only to initialize the
             * ScriptSourceObject. Most CompileOptions fields aren't used by
             * ScriptSourceObject, and those that are (element; elementAttributeName)
             * aren't preserved by XDR. So this can be simple.
             */
            CompileOptions options(cx);
            ss->initFromOptions(cx, options);
            sourceObject = ScriptSourceObject::create(cx, ss);
            if (!sourceObject ||
                !ScriptSourceObject::initFromOptions(cx, sourceObject, options))
                return false;
        } else {
            MOZ_ASSERT(enclosingScript);
            // When decoding, all the scripts and the script source object
            // are in the same compartment, so the script's source object
            // should never be a cross-compartment wrapper.
            MOZ_ASSERT(enclosingScript->sourceObject()->is<ScriptSourceObject>());
            sourceObject = &enclosingScript->sourceObject()->as<ScriptSourceObject>();
        }

        // If the outermost script has a non-syntactic scope, reflect that on
        // the static scope chain.
        if (scriptBits & (1 << HasNonSyntacticScope) &&
            IsStaticGlobalLexicalScope(enclosingScope))
        {
            enclosingScope = StaticNonSyntacticScopeObjects::create(cx, enclosingScope);
            if (!enclosingScope)
                return false;
        }

        script = JSScript::Create(cx, enclosingScope, !!(scriptBits & (1 << SavedCallerFun)),
                                  options, sourceObject, 0, 0);
        if (!script)
            return false;

        // Set the script in its function now so that inner scripts to be
        // decoded may iterate the static scope chain.
        if (fun) {
            fun->initScript(script);
            script->setFunction(fun);
        }
    }

    /* JSScript::partiallyInit assumes script->bindings is fully initialized. */
    LifoAllocScope las(&cx->tempLifoAlloc());
    if (!XDRScriptBindings(xdr, las, nargs, nvars, nbodylevellexicals, nblocklocals,
                           nunaliasedvars, nunaliasedbodylevellexicals, script))
        return false;

    if (mode == XDR_DECODE) {
        if (!JSScript::partiallyInit(cx, script, nconsts, nobjects, nregexps, ntrynotes,
                                     nblockscopes, nyieldoffsets, nTypeSets))
        {
            return false;
        }

        MOZ_ASSERT(!script->mainOffset());
        script->mainOffset_ = prologueLength;
        script->setLength(length);
        script->funLength_ = funLength;

        scriptp.set(script);

        if (scriptBits & (1 << Strict))
            script->strict_ = true;
        if (scriptBits & (1 << ExplicitUseStrict))
            script->explicitUseStrict_ = true;
        if (scriptBits & (1 << ContainsDynamicNameAccess))
            script->bindingsAccessedDynamically_ = true;
        if (scriptBits & (1 << FunHasExtensibleScope))
            script->funHasExtensibleScope_ = true;
        if (scriptBits & (1 << FunNeedsDeclEnvObject))
            script->funNeedsDeclEnvObject_ = true;
        if (scriptBits & (1 << FunHasAnyAliasedFormal))
            script->funHasAnyAliasedFormal_ = true;
        if (scriptBits & (1 << ArgumentsHasVarBinding))
            script->setArgumentsHasVarBinding();
        if (scriptBits & (1 << NeedsArgsObj))
            script->setNeedsArgsObj(true);
        if (scriptBits & (1 << HasMappedArgsObj))
            script->hasMappedArgsObj_ = true;
        if (scriptBits & (1 << FunctionHasThisBinding))
            script->functionHasThisBinding_ = true;
        if (scriptBits & (1 << IsGeneratorExp))
            script->isGeneratorExp_ = true;
        if (scriptBits & (1 << HasSingleton))
            script->hasSingletons_ = true;
        if (scriptBits & (1 << TreatAsRunOnce))
            script->treatAsRunOnce_ = true;
        if (scriptBits & (1 << HasNonSyntacticScope))
            script->hasNonSyntacticScope_ = true;
        if (scriptBits & (1 << HasInnerFunctions))
            script->hasInnerFunctions_ = true;
        if (scriptBits & (1 << NeedsHomeObject))
            script->needsHomeObject_ = true;
        if (scriptBits & (1 << IsDerivedClassConstructor))
            script->isDerivedClassConstructor_ = true;

        if (scriptBits & (1 << IsLegacyGenerator)) {
            MOZ_ASSERT(!(scriptBits & (1 << IsStarGenerator)));
            script->setGeneratorKind(LegacyGenerator);
        } else if (scriptBits & (1 << IsStarGenerator))
            script->setGeneratorKind(StarGenerator);
    }

    JS_STATIC_ASSERT(sizeof(jsbytecode) == 1);
    JS_STATIC_ASSERT(sizeof(jssrcnote) == 1);

    if (scriptBits & (1 << OwnSource)) {
        if (!script->scriptSource()->performXDR<mode>(xdr))
            return false;
    }
    if (!xdr->codeUint32(&script->sourceStart_))
        return false;
    if (!xdr->codeUint32(&script->sourceEnd_))
        return false;

    if (!xdr->codeUint32(&lineno) ||
        !xdr->codeUint32(&column) ||
        !xdr->codeUint32(&nslots))
    {
        return false;
    }

    if (mode == XDR_DECODE) {
        script->lineno_ = lineno;
        script->column_ = column;
        script->nslots_ = nslots;
    }

    jsbytecode* code = script->code();
    SharedScriptData* ssd;
    if (mode == XDR_DECODE) {
        ssd = SharedScriptData::new_(cx, length, nsrcnotes, natoms);
        if (!ssd)
            return false;
        code = ssd->data;
        if (natoms != 0) {
            script->natoms_ = natoms;
            script->atoms = ssd->atoms();
        }
    }

    if (!xdr->codeBytes(code, length) || !xdr->codeBytes(code + length, nsrcnotes)) {
        if (mode == XDR_DECODE)
            js_free(ssd);
        return false;
    }

    for (i = 0; i != natoms; ++i) {
        if (mode == XDR_DECODE) {
            RootedAtom tmp(cx);
            if (!XDRAtom(xdr, &tmp))
                return false;
            script->atoms[i].init(tmp);
        } else {
            RootedAtom tmp(cx, script->atoms[i]);
            if (!XDRAtom(xdr, &tmp))
                return false;
        }
    }

    if (mode == XDR_DECODE) {
        if (!SaveSharedScriptData(cx, script, ssd, nsrcnotes))
            return false;
    }

    if (nconsts) {
        HeapValue* vector = script->consts()->vector;
        RootedValue val(cx);
        for (i = 0; i != nconsts; ++i) {
            if (mode == XDR_ENCODE)
                val = vector[i];
            if (!XDRScriptConst(xdr, &val))
                return false;
            if (mode == XDR_DECODE)
                vector[i].init(val);
        }
    }

    /*
     * Here looping from 0-to-length to xdr objects is essential to ensure that
     * all references to enclosing blocks (via FindScopeObjectIndex below) happen
     * after the enclosing block has been XDR'd.
     */
    for (i = 0; i != nobjects; ++i) {
        HeapPtrObject* objp = &script->objects()->vector[i];
        XDRClassKind classk;

        if (mode == XDR_ENCODE) {
            JSObject* obj = *objp;
            if (obj->is<BlockObject>())
                classk = CK_BlockObject;
            else if (obj->is<StaticWithObject>())
                classk = CK_WithObject;
            else if (obj->is<JSFunction>())
                classk = CK_JSFunction;
            else if (obj->is<PlainObject>() || obj->is<ArrayObject>())
                classk = CK_JSObject;
            else
                MOZ_CRASH("Cannot encode this class of object.");
        }

        if (!xdr->codeEnum32(&classk))
            return false;

        switch (classk) {
          case CK_BlockObject:
          case CK_WithObject: {
            /* Code the nested block's enclosing scope. */
            uint32_t enclosingStaticScopeIndex = 0;
            if (mode == XDR_ENCODE) {
                NestedScopeObject& scope = (*objp)->as<NestedScopeObject>();
                if (NestedScopeObject* enclosing = scope.enclosingNestedScope()) {
                    if (IsStaticGlobalLexicalScope(enclosing))
                        enclosingStaticScopeIndex = UINT32_MAX;
                    else
                        enclosingStaticScopeIndex = FindScopeObjectIndex(script, *enclosing);
                } else {
                    enclosingStaticScopeIndex = UINT32_MAX;
                }
            }
            if (!xdr->codeUint32(&enclosingStaticScopeIndex))
                return false;
            Rooted<JSObject*> enclosingStaticScope(cx);
            if (mode == XDR_DECODE) {
                if (enclosingStaticScopeIndex != UINT32_MAX) {
                    MOZ_ASSERT(enclosingStaticScopeIndex < i);
                    enclosingStaticScope = script->objects()->vector[enclosingStaticScopeIndex];
                } else {
                    // This is not ternary because MSVC can't typecheck the
                    // ternary.
                    if (fun)
                        enclosingStaticScope = fun;
                    else
                        enclosingStaticScope = enclosingScope;
                }
            }

            if (classk == CK_BlockObject) {
                Rooted<StaticBlockObject*> tmp(cx, static_cast<StaticBlockObject*>(objp->get()));
                if (!XDRStaticBlockObject(xdr, enclosingStaticScope, &tmp))
                    return false;
                *objp = tmp;
            } else {
                Rooted<StaticWithObject*> tmp(cx, static_cast<StaticWithObject*>(objp->get()));
                if (!XDRStaticWithObject(xdr, enclosingStaticScope, &tmp))
                    return false;
                *objp = tmp;
            }
            break;
          }

          case CK_JSFunction: {
            /* Code the nested function's enclosing scope. */
            uint32_t funEnclosingScopeIndex = 0;
            RootedObject funEnclosingScope(cx);
            if (mode == XDR_ENCODE) {
                RootedFunction function(cx, &(*objp)->as<JSFunction>());

                if (function->isInterpretedLazy())
                    funEnclosingScope = function->lazyScript()->enclosingScope();
                else if (function->isInterpreted())
                    funEnclosingScope = function->nonLazyScript()->enclosingStaticScope();
                else {
                    MOZ_ASSERT(function->isAsmJSNative());
                    JS_ReportError(cx, "AsmJS modules are not yet supported in XDR serialization.");
                    return false;
                }

                StaticScopeIter<NoGC> ssi(funEnclosingScope);

                // Starting from a nested function, hitting a non-syntactic
                // scope on the static scope chain means that its enclosing
                // function has a non-syntactic scope. Nested functions
                // themselves never have non-syntactic scope chains.
                if (ssi.done() ||
                    ssi.type() == StaticScopeIter<NoGC>::NonSyntactic ||
                    ssi.type() == StaticScopeIter<NoGC>::Function)
                {
                    MOZ_ASSERT_IF(ssi.done() || ssi.type() != StaticScopeIter<NoGC>::Function, !fun);
                    funEnclosingScopeIndex = UINT32_MAX;
                } else if (ssi.type() == StaticScopeIter<NoGC>::Block) {
                    if (ssi.block().isGlobal()) {
                        funEnclosingScopeIndex = UINT32_MAX;
                    } else {
                        funEnclosingScopeIndex = FindScopeObjectIndex(script, ssi.block());
                        MOZ_ASSERT(funEnclosingScopeIndex < i);
                    }
                } else {
                    funEnclosingScopeIndex = FindScopeObjectIndex(script, ssi.staticWith());
                    MOZ_ASSERT(funEnclosingScopeIndex < i);
                }
            }

            if (!xdr->codeUint32(&funEnclosingScopeIndex))
                return false;

            if (mode == XDR_DECODE) {
                if (funEnclosingScopeIndex == UINT32_MAX) {
                    // This is not ternary because MSVC can't typecheck the
                    // ternary.
                    if (fun)
                        funEnclosingScope = fun;
                    else
                        funEnclosingScope = enclosingScope;
                } else {
                    MOZ_ASSERT(funEnclosingScopeIndex < i);
                    funEnclosingScope = script->objects()->vector[funEnclosingScopeIndex];
                }
            }

            // Code nested function and script.
            RootedFunction tmp(cx);
            if (mode == XDR_ENCODE)
                tmp = &(*objp)->as<JSFunction>();
            if (!XDRInterpretedFunction(xdr, funEnclosingScope, script, &tmp))
                return false;
            *objp = tmp;
            break;
          }

          case CK_JSObject: {
            /* Code object literal. */
            RootedObject tmp(cx, *objp);
            if (!XDRObjectLiteral(xdr, &tmp))
                return false;
            *objp = tmp;
            break;
          }

          default: {
            MOZ_ASSERT(false, "Unknown class kind.");
            return false;
          }
        }
    }

    for (i = 0; i != nregexps; ++i) {
        Rooted<RegExpObject*> regexp(cx);
        if (mode == XDR_ENCODE)
            regexp = &script->regexps()->vector[i]->as<RegExpObject>();
        if (!XDRScriptRegExpObject(xdr, &regexp))
            return false;
        if (mode == XDR_DECODE)
            script->regexps()->vector[i] = regexp;
    }

    if (ntrynotes != 0) {
        JSTryNote* tnfirst = script->trynotes()->vector;
        MOZ_ASSERT(script->trynotes()->length == ntrynotes);
        JSTryNote* tn = tnfirst + ntrynotes;
        do {
            --tn;
            if (!xdr->codeUint8(&tn->kind) ||
                !xdr->codeUint32(&tn->stackDepth) ||
                !xdr->codeUint32(&tn->start) ||
                !xdr->codeUint32(&tn->length)) {
                return false;
            }
        } while (tn != tnfirst);
    }

    for (i = 0; i < nblockscopes; ++i) {
        BlockScopeNote* note = &script->blockScopes()->vector[i];
        if (!xdr->codeUint32(&note->index) ||
            !xdr->codeUint32(&note->start) ||
            !xdr->codeUint32(&note->length) ||
            !xdr->codeUint32(&note->parent))
        {
            return false;
        }
    }

    for (i = 0; i < nyieldoffsets; ++i) {
        uint32_t* offset = &script->yieldOffsets()[i];
        if (!xdr->codeUint32(offset))
            return false;
    }

    if (scriptBits & (1 << HasLazyScript)) {
        Rooted<LazyScript*> lazy(cx);
        if (mode == XDR_ENCODE)
            lazy = script->maybeLazyScript();

        if (!XDRRelazificationInfo(xdr, fun, script, enclosingScope, &lazy))
            return false;

        if (mode == XDR_DECODE)
            script->setLazyScript(lazy);
    }

    if (mode == XDR_DECODE) {
        scriptp.set(script);

        /* see BytecodeEmitter::tellDebuggerAboutCompiledScript */
        if (!fun)
            Debugger::onNewScript(cx, script);
    }

    return true;
}

template bool
js::XDRScript(XDRState<XDR_ENCODE>*, HandleObject, HandleScript, HandleFunction,
              MutableHandleScript);

template bool
js::XDRScript(XDRState<XDR_DECODE>*, HandleObject, HandleScript, HandleFunction,
              MutableHandleScript);

template<XDRMode mode>
bool
js::XDRLazyScript(XDRState<mode>* xdr, HandleObject enclosingScope, HandleScript enclosingScript,
                  HandleFunction fun, MutableHandle<LazyScript*> lazy)
{
    JSContext* cx = xdr->cx();

    {
        uint32_t begin;
        uint32_t end;
        uint32_t lineno;
        uint32_t column;
        uint64_t packedFields;

        if (mode == XDR_ENCODE) {
            // Note: it's possible the LazyScript has a non-null script_ pointer
            // to a JSScript. We don't encode it: we can just delazify the
            // lazy script.

            MOZ_ASSERT(fun == lazy->functionNonDelazifying());

            begin = lazy->begin();
            end = lazy->end();
            lineno = lazy->lineno();
            column = lazy->column();
            packedFields = lazy->packedFields();
        }

        if (!xdr->codeUint32(&begin) || !xdr->codeUint32(&end) ||
            !xdr->codeUint32(&lineno) || !xdr->codeUint32(&column) ||
            !xdr->codeUint64(&packedFields))
        {
            return false;
        }

        if (mode == XDR_DECODE) {
            lazy.set(LazyScript::Create(cx, fun, nullptr, enclosingScope, enclosingScript,
                                        packedFields, begin, end, lineno, column));
            if (!lazy)
                return false;
            fun->initLazyScript(lazy);
        }
    }

    // Code free variables.
    if (!XDRLazyFreeVariables(xdr, lazy))
        return false;

    // Code inner functions.
    {
        RootedFunction func(cx);
        HeapPtrFunction* innerFunctions = lazy->innerFunctions();
        size_t numInnerFunctions = lazy->numInnerFunctions();
        for (size_t i = 0; i < numInnerFunctions; i++) {
            if (mode == XDR_ENCODE)
                func = innerFunctions[i];

            if (!XDRInterpretedFunction(xdr, fun, enclosingScript, &func))
                return false;

            if (mode == XDR_DECODE)
                innerFunctions[i] = func;
        }
    }

    return true;
}

template bool
js::XDRLazyScript(XDRState<XDR_ENCODE>*, HandleObject, HandleScript,
                  HandleFunction, MutableHandle<LazyScript*>);

template bool
js::XDRLazyScript(XDRState<XDR_DECODE>*, HandleObject, HandleScript,
                  HandleFunction, MutableHandle<LazyScript*>);

void
JSScript::setSourceObject(JSObject* object)
{
    MOZ_ASSERT(compartment() == object->compartment());
    sourceObject_ = object;
}

js::ScriptSourceObject&
JSScript::scriptSourceUnwrap() const {
    return UncheckedUnwrap(sourceObject())->as<ScriptSourceObject>();
}

js::ScriptSource*
JSScript::scriptSource() const {
    return scriptSourceUnwrap().source();
}

js::ScriptSource*
JSScript::maybeForwardedScriptSource() const {
    return UncheckedUnwrap(MaybeForwarded(sourceObject()))->as<ScriptSourceObject>().source();
}

bool
JSScript::initScriptCounts(JSContext* cx)
{
    MOZ_ASSERT(!hasScriptCounts());

    // Record all pc which are the first instruction of a basic block.
    mozilla::Vector<jsbytecode*, 16, SystemAllocPolicy> jumpTargets;
    jsbytecode* end = codeEnd();
    jsbytecode* mainEntry = main();
    for (jsbytecode* pc = code(); pc != end; pc = GetNextPc(pc)) {
        if (pc == mainEntry) {
            if (!jumpTargets.append(pc))
                return false;
        }

        bool jump = IsJumpOpcode(JSOp(*pc));
        if (jump) {
            jsbytecode* target = pc + GET_JUMP_OFFSET(pc);
            if (!jumpTargets.append(target))
                return false;

            if (BytecodeFallsThrough(JSOp(*pc))) {
                jsbytecode* fallthrough = GetNextPc(pc);
                if (!jumpTargets.append(fallthrough))
                    return false;
            }
        }

        if (JSOp(*pc) == JSOP_TABLESWITCH) {
            jsbytecode* pc2 = pc;
            int32_t len = GET_JUMP_OFFSET(pc2);

            // Default target.
            if (!jumpTargets.append(pc + len))
                return false;

            pc2 += JUMP_OFFSET_LEN;
            int32_t low = GET_JUMP_OFFSET(pc2);
            pc2 += JUMP_OFFSET_LEN;
            int32_t high = GET_JUMP_OFFSET(pc2);

            for (int i = 0; i < high-low+1; i++) {
                pc2 += JUMP_OFFSET_LEN;
                int32_t off = (int32_t) GET_JUMP_OFFSET(pc2);
                if (off) {
                    // Case (i + low)
                    if (!jumpTargets.append(pc + off))
                        return false;
                }
            }
        }
    }

    // Mark catch/finally blocks as being jump targets.
    if (hasTrynotes()) {
        JSTryNote* tn = trynotes()->vector;
        JSTryNote* tnlimit = tn + trynotes()->length;
        for (; tn < tnlimit; tn++) {
            jsbytecode* tryStart = mainEntry + tn->start;
            jsbytecode* tryPc = tryStart - 1;
            if (JSOp(*tryPc) != JSOP_TRY)
                continue;

            jsbytecode* tryTarget = tryStart + tn->length;
            if (!jumpTargets.append(tryTarget))
                return false;
        }
    }

    // Sort all pc, and remove duplicates.
    std::sort(jumpTargets.begin(), jumpTargets.end());
    auto last = std::unique(jumpTargets.begin(), jumpTargets.end());
    jumpTargets.erase(last, jumpTargets.end());

    // Initialize all PCCounts counters to 0.
    ScriptCounts::PCCountsVector base;
    if (!base.reserve(jumpTargets.length()))
        return false;

    for (size_t i = 0; i < jumpTargets.length(); i++)
        base.infallibleEmplaceBack(pcToOffset(jumpTargets[i]));

    // Create compartment's scriptCountsMap if necessary.
    ScriptCountsMap* map = compartment()->scriptCountsMap;
    if (!map) {
        map = cx->new_<ScriptCountsMap>();
        if (!map)
            return false;

        if (!map->init()) {
            js_delete(map);
            ReportOutOfMemory(cx);
            return false;
        }

        compartment()->scriptCountsMap = map;
    }

    // Register the current ScriptCount in the compartment's map.
    if (!map->putNew(this, Move(base)))
        return false;

    // safe to set this;  we can't fail after this point.
    hasScriptCounts_ = true;

    // Enable interrupts in any interpreter frames running on this script. This
    // is used to let the interpreter increment the PCCounts, if present.
    for (ActivationIterator iter(cx->runtime()); !iter.done(); ++iter) {
        if (iter->isInterpreter())
            iter->asInterpreter()->enableInterruptsIfRunning(this);
    }

    return true;
}

static inline ScriptCountsMap::Ptr GetScriptCountsMapEntry(JSScript* script)
{
    MOZ_ASSERT(script->hasScriptCounts());
    ScriptCountsMap* map = script->compartment()->scriptCountsMap;
    ScriptCountsMap::Ptr p = map->lookup(script);
    MOZ_ASSERT(p);
    return p;
}

ScriptCounts&
JSScript::getScriptCounts()
{
    ScriptCountsMap::Ptr p = GetScriptCountsMapEntry(this);
    return p->value();
}

js::PCCounts*
ScriptCounts::maybeGetPCCounts(size_t offset) {
    PCCounts searched = PCCounts(offset);
    PCCounts* elem = std::lower_bound(pcCounts_.begin(), pcCounts_.end(), searched);
    if (elem == pcCounts_.end() || elem->pcOffset() != offset)
        return nullptr;
    return elem;
}

const js::PCCounts*
ScriptCounts::maybeGetPCCounts(size_t offset) const {
    PCCounts searched = PCCounts(offset);
    const PCCounts* elem = std::lower_bound(pcCounts_.begin(), pcCounts_.end(), searched);
    if (elem == pcCounts_.end() || elem->pcOffset() != offset)
        return nullptr;
    return elem;
}

js::PCCounts*
ScriptCounts::getImmediatePrecedingPCCounts(size_t offset)
{
    PCCounts searched = PCCounts(offset);
    PCCounts* elem = std::lower_bound(pcCounts_.begin(), pcCounts_.end(), searched);
    if (elem == pcCounts_.end())
        return &pcCounts_.back();
    if (elem->pcOffset() == offset)
        return elem;
    if (elem != pcCounts_.begin())
        return elem - 1;
    return nullptr;
}

const js::PCCounts*
ScriptCounts::maybeGetThrowCounts(size_t offset) const {
    PCCounts searched = PCCounts(offset);
    const PCCounts* elem = std::lower_bound(throwCounts_.begin(), throwCounts_.end(), searched);
    if (elem == throwCounts_.end() || elem->pcOffset() != offset)
        return nullptr;
    return elem;
}

const js::PCCounts*
ScriptCounts::getImmediatePrecedingThrowCounts(size_t offset) const
{
    PCCounts searched = PCCounts(offset);
    const PCCounts* elem = std::lower_bound(throwCounts_.begin(), throwCounts_.end(), searched);
    if (elem == throwCounts_.end()) {
        if (throwCounts_.begin() == throwCounts_.end())
            return nullptr;
        return &throwCounts_.back();
    }
    if (elem->pcOffset() == offset)
        return elem;
    if (elem != throwCounts_.begin())
        return elem - 1;
    return nullptr;
}

js::PCCounts*
ScriptCounts::getThrowCounts(size_t offset) {
    PCCounts searched = PCCounts(offset);
    PCCounts* elem = std::lower_bound(throwCounts_.begin(), throwCounts_.end(), searched);
    if (elem == throwCounts_.end() || elem->pcOffset() != offset)
        elem = throwCounts_.insert(elem, searched);
    return elem;
}

void
JSScript::setIonScript(JSContext* maybecx, js::jit::IonScript* ionScript)
{
    MOZ_ASSERT_IF(ionScript != ION_DISABLED_SCRIPT, !baselineScript()->hasPendingIonBuilder());
    if (hasIonScript())
        js::jit::IonScript::writeBarrierPre(zone(), ion);
    ion = ionScript;
    MOZ_ASSERT_IF(hasIonScript(), hasBaselineScript());
    updateBaselineOrIonRaw(maybecx);
}

js::PCCounts*
JSScript::maybeGetPCCounts(jsbytecode* pc) {
    MOZ_ASSERT(containsPC(pc));
    return getScriptCounts().maybeGetPCCounts(pcToOffset(pc));
}

const js::PCCounts*
JSScript::maybeGetThrowCounts(jsbytecode* pc) {
    MOZ_ASSERT(containsPC(pc));
    return getScriptCounts().maybeGetThrowCounts(pcToOffset(pc));
}

js::PCCounts*
JSScript::getThrowCounts(jsbytecode* pc) {
    MOZ_ASSERT(containsPC(pc));
    return getScriptCounts().getThrowCounts(pcToOffset(pc));
}

uint64_t
JSScript::getHitCount(jsbytecode* pc)
{
    MOZ_ASSERT(containsPC(pc));
    if (pc < main())
        pc = main();

    ScriptCounts& sc = getScriptCounts();
    size_t targetOffset = pcToOffset(pc);
    const js::PCCounts* baseCount = sc.getImmediatePrecedingPCCounts(targetOffset);
    if (!baseCount)
        return 0;
    if (baseCount->pcOffset() == targetOffset)
        return baseCount->numExec();
    MOZ_ASSERT(baseCount->pcOffset() < targetOffset);
    uint64_t count = baseCount->numExec();
    do {
        const js::PCCounts* throwCount = sc.getImmediatePrecedingThrowCounts(targetOffset);
        if (!throwCount)
            return count;
        if (throwCount->pcOffset() <= baseCount->pcOffset())
            return count;
        count -= throwCount->numExec();
        targetOffset = throwCount->pcOffset() - 1;
    } while (true);
}

void
JSScript::incHitCount(jsbytecode* pc)
{
    MOZ_ASSERT(containsPC(pc));
    if (pc < main())
        pc = main();

    ScriptCounts& sc = getScriptCounts();
    js::PCCounts* baseCount = sc.getImmediatePrecedingPCCounts(pcToOffset(pc));
    if (!baseCount)
        return;
    baseCount->numExec()++;
}

void
JSScript::addIonCounts(jit::IonScriptCounts* ionCounts)
{
    ScriptCounts& sc = getScriptCounts();
    if (sc.ionCounts_)
        ionCounts->setPrevious(sc.ionCounts_);
    sc.ionCounts_ = ionCounts;
}

jit::IonScriptCounts*
JSScript::getIonCounts()
{
    return getScriptCounts().ionCounts_;
}

void
JSScript::takeOverScriptCountsMapEntry(ScriptCounts* entryValue)
{
#ifdef DEBUG
    ScriptCountsMap::Ptr p = GetScriptCountsMapEntry(this);
    MOZ_ASSERT(entryValue == &p->value());
#endif
    hasScriptCounts_ = false;
}

void
JSScript::releaseScriptCounts(ScriptCounts* counts)
{
    ScriptCountsMap::Ptr p = GetScriptCountsMapEntry(this);
    *counts = Move(p->value());
    compartment()->scriptCountsMap->remove(p);
    hasScriptCounts_ = false;
}

void
JSScript::destroyScriptCounts(FreeOp* fop)
{
    if (hasScriptCounts()) {
        ScriptCounts scriptCounts;
        releaseScriptCounts(&scriptCounts);
    }
}

void
ScriptSourceObject::trace(JSTracer* trc, JSObject* obj)
{
    ScriptSourceObject* sso = static_cast<ScriptSourceObject*>(obj);

    // Don't trip over the poison 'not yet initialized' values.
    if (!sso->getReservedSlot(INTRODUCTION_SCRIPT_SLOT).isMagic(JS_GENERIC_MAGIC)) {
        JSScript* script = sso->introductionScript();
        if (script) {
            TraceManuallyBarrieredEdge(trc, &script, "ScriptSourceObject introductionScript");
            sso->setReservedSlot(INTRODUCTION_SCRIPT_SLOT, PrivateValue(script));
        }
    }
}

void
ScriptSourceObject::finalize(FreeOp* fop, JSObject* obj)
{
    ScriptSourceObject* sso = &obj->as<ScriptSourceObject>();

    // If code coverage is enabled, record the filename associated with this
    // source object.
    if (fop->runtime()->lcovOutput.isEnabled())
        sso->compartment()->lcovOutput.collectSourceFile(sso->compartment(), sso);

    sso->source()->decref();
    sso->setReservedSlot(SOURCE_SLOT, PrivateValue(nullptr));
}

const Class ScriptSourceObject::class_ = {
    "ScriptSource",
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) |
    JSCLASS_IS_ANONYMOUS,
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* getProperty */
    nullptr, /* setProperty */
    nullptr, /* enumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    finalize,
    nullptr, /* call */
    nullptr, /* hasInstance */
    nullptr, /* construct */
    trace
};

ScriptSourceObject*
ScriptSourceObject::create(ExclusiveContext* cx, ScriptSource* source)
{
    RootedObject object(cx, NewObjectWithGivenProto(cx, &class_, nullptr));
    if (!object)
        return nullptr;
    RootedScriptSource sourceObject(cx, &object->as<ScriptSourceObject>());

    source->incref();    // The matching decref is in ScriptSourceObject::finalize.
    sourceObject->initReservedSlot(SOURCE_SLOT, PrivateValue(source));

    // The remaining slots should eventually be populated by a call to
    // initFromOptions. Poison them until that point.
    sourceObject->initReservedSlot(ELEMENT_SLOT, MagicValue(JS_GENERIC_MAGIC));
    sourceObject->initReservedSlot(ELEMENT_PROPERTY_SLOT, MagicValue(JS_GENERIC_MAGIC));
    sourceObject->initReservedSlot(INTRODUCTION_SCRIPT_SLOT, MagicValue(JS_GENERIC_MAGIC));

    return sourceObject;
}

/* static */ bool
ScriptSourceObject::initFromOptions(JSContext* cx, HandleScriptSource source,
                                    const ReadOnlyCompileOptions& options)
{
    assertSameCompartment(cx, source);
    MOZ_ASSERT(source->getReservedSlot(ELEMENT_SLOT).isMagic(JS_GENERIC_MAGIC));
    MOZ_ASSERT(source->getReservedSlot(ELEMENT_PROPERTY_SLOT).isMagic(JS_GENERIC_MAGIC));
    MOZ_ASSERT(source->getReservedSlot(INTRODUCTION_SCRIPT_SLOT).isMagic(JS_GENERIC_MAGIC));

    RootedValue element(cx, ObjectOrNullValue(options.element()));
    if (!cx->compartment()->wrap(cx, &element))
        return false;
    source->setReservedSlot(ELEMENT_SLOT, element);

    RootedValue elementAttributeName(cx);
    if (options.elementAttributeName())
        elementAttributeName = StringValue(options.elementAttributeName());
    else
        elementAttributeName = UndefinedValue();
    if (!cx->compartment()->wrap(cx, &elementAttributeName))
        return false;
    source->setReservedSlot(ELEMENT_PROPERTY_SLOT, elementAttributeName);

    // There is no equivalent of cross-compartment wrappers for scripts. If the
    // introduction script and ScriptSourceObject are in different compartments,
    // we would be creating a cross-compartment script reference, which is
    // forbidden. In that case, simply don't bother to retain the introduction
    // script.
    if (options.introductionScript() &&
        options.introductionScript()->compartment() == cx->compartment())
    {
        source->setReservedSlot(INTRODUCTION_SCRIPT_SLOT, PrivateValue(options.introductionScript()));
    } else {
        source->setReservedSlot(INTRODUCTION_SCRIPT_SLOT, UndefinedValue());
    }

    return true;
}

/* static */ bool
JSScript::loadSource(JSContext* cx, ScriptSource* ss, bool* worked)
{
    MOZ_ASSERT(!ss->hasSourceData());
    *worked = false;
    if (!cx->runtime()->sourceHook || !ss->sourceRetrievable())
        return true;
    char16_t* src = nullptr;
    size_t length;
    if (!cx->runtime()->sourceHook->load(cx, ss->filename(), &src, &length))
        return false;
    if (!src)
        return true;
    ss->setSource(src, length);
    *worked = true;
    return true;
}

JSFlatString*
JSScript::sourceData(JSContext* cx)
{
    MOZ_ASSERT(scriptSource()->hasSourceData());
    return scriptSource()->substring(cx, sourceStart(), sourceEnd());
}

UncompressedSourceCache::AutoHoldEntry::AutoHoldEntry()
  : cache_(nullptr), source_(nullptr), charsToFree_(nullptr)
{
}

void
UncompressedSourceCache::AutoHoldEntry::holdEntry(UncompressedSourceCache* cache, ScriptSource* source)
{
    // Initialise the holder for a specific cache and script source. This will
    // hold on to the cached source chars in the event that the cache is purged.
    MOZ_ASSERT(!cache_ && !source_ && !charsToFree_);
    cache_ = cache;
    source_ = source;
}

void
UncompressedSourceCache::AutoHoldEntry::deferDelete(const char16_t* chars)
{
    // Take ownership of source chars now the cache is being purged. Remove our
    // reference to the ScriptSource which might soon be destroyed.
    MOZ_ASSERT(cache_ && source_ && !charsToFree_);
    cache_ = nullptr;
    source_ = nullptr;
    charsToFree_ = chars;
}

UncompressedSourceCache::AutoHoldEntry::~AutoHoldEntry()
{
    // The holder is going out of scope. If it has taken ownership of cached
    // chars then delete them, otherwise unregister ourself with the cache.
    if (charsToFree_) {
        MOZ_ASSERT(!cache_ && !source_);
        js_free(const_cast<char16_t*>(charsToFree_));
    } else if (cache_) {
        MOZ_ASSERT(source_);
        cache_->releaseEntry(*this);
    }
}

void
UncompressedSourceCache::holdEntry(AutoHoldEntry& holder, ScriptSource* ss)
{
    MOZ_ASSERT(!holder_);
    holder.holdEntry(this, ss);
    holder_ = &holder;
}

void
UncompressedSourceCache::releaseEntry(AutoHoldEntry& holder)
{
    MOZ_ASSERT(holder_ == &holder);
    holder_ = nullptr;
}

const char16_t*
UncompressedSourceCache::lookup(ScriptSource* ss, AutoHoldEntry& holder)
{
    MOZ_ASSERT(!holder_);
    if (!map_)
        return nullptr;
    if (Map::Ptr p = map_->lookup(ss)) {
        holdEntry(holder, ss);
        return p->value();
    }
    return nullptr;
}

bool
UncompressedSourceCache::put(ScriptSource* ss, const char16_t* str, AutoHoldEntry& holder)
{
    MOZ_ASSERT(!holder_);

    if (!map_) {
        map_ = js_new<Map>();
        if (!map_)
            return false;

        if (!map_->init()) {
            js_delete(map_);
            map_ = nullptr;
            return false;
        }
    }

    if (!map_->put(ss, str))
        return false;

    holdEntry(holder, ss);
    return true;
}

void
UncompressedSourceCache::purge()
{
    if (!map_)
        return;

    for (Map::Range r = map_->all(); !r.empty(); r.popFront()) {
        const char16_t* chars = r.front().value();
        if (holder_ && r.front().key() == holder_->source()) {
            holder_->deferDelete(chars);
            holder_ = nullptr;
        } else {
            js_free(const_cast<char16_t*>(chars));
        }
    }

    js_delete(map_);
    map_ = nullptr;
}

size_t
UncompressedSourceCache::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf)
{
    size_t n = 0;
    if (map_ && !map_->empty()) {
        n += map_->sizeOfIncludingThis(mallocSizeOf);
        for (Map::Range r = map_->all(); !r.empty(); r.popFront()) {
            const char16_t* v = r.front().value();
            n += mallocSizeOf(v);
        }
    }
    return n;
}

const char16_t*
ScriptSource::chars(JSContext* cx, UncompressedSourceCache::AutoHoldEntry& holder)
{
    switch (dataType) {
      case DataUncompressed:
        return uncompressedChars();

      case DataCompressed: {
        if (const char16_t* decompressed = cx->runtime()->uncompressedSourceCache.lookup(this, holder))
            return decompressed;

        const size_t nbytes = sizeof(char16_t) * (length_ + 1);
        char16_t* decompressed = static_cast<char16_t*>(js_malloc(nbytes));
        if (!decompressed) {
            JS_ReportOutOfMemory(cx);
            return nullptr;
        }

        if (!DecompressString((const unsigned char*) compressedData(), compressedBytes(),
                              reinterpret_cast<unsigned char*>(decompressed), nbytes)) {
            JS_ReportOutOfMemory(cx);
            js_free(decompressed);
            return nullptr;
        }

        decompressed[length_] = 0;

        if (!cx->runtime()->uncompressedSourceCache.put(this, decompressed, holder)) {
            JS_ReportOutOfMemory(cx);
            js_free(decompressed);
            return nullptr;
        }

        return decompressed;
      }

      case DataParent:
        return parent()->chars(cx, holder);

      default:
        MOZ_CRASH();
    }
}

JSFlatString*
ScriptSource::substring(JSContext* cx, uint32_t start, uint32_t stop)
{
    MOZ_ASSERT(start <= stop);
    UncompressedSourceCache::AutoHoldEntry holder;
    const char16_t* chars = this->chars(cx, holder);
    if (!chars)
        return nullptr;
    return NewStringCopyN<CanGC>(cx, chars + start, stop - start);
}

JSFlatString*
ScriptSource::substringDontDeflate(JSContext* cx, uint32_t start, uint32_t stop)
{
    MOZ_ASSERT(start <= stop);
    UncompressedSourceCache::AutoHoldEntry holder;
    const char16_t* chars = this->chars(cx, holder);
    if (!chars)
        return nullptr;
    return NewStringCopyNDontDeflate<CanGC>(cx, chars + start, stop - start);
}

void
ScriptSource::setSource(const char16_t* chars, size_t length, bool ownsChars /* = true */)
{
    MOZ_ASSERT(dataType == DataMissing);

    dataType = DataUncompressed;
    data.uncompressed.chars = chars;
    data.uncompressed.ownsChars = ownsChars;

    length_ = length;
}

void
ScriptSource::setCompressedSource(JSRuntime* maybert, void* raw, size_t nbytes, HashNumber hash)
{
    MOZ_ASSERT(dataType == DataMissing || dataType == DataUncompressed);
    if (dataType == DataUncompressed && ownsUncompressedChars())
        js_free(const_cast<char16_t*>(uncompressedChars()));

    dataType = DataCompressed;
    data.compressed.raw = raw;
    data.compressed.nbytes = nbytes;
    data.compressed.hash = hash;

    if (maybert)
        updateCompressedSourceSet(maybert);
}

void
ScriptSource::updateCompressedSourceSet(JSRuntime* rt)
{
    MOZ_ASSERT(dataType == DataCompressed);
    MOZ_ASSERT(!inCompressedSourceSet);

    CompressedSourceSet::AddPtr p = rt->compressedSourceSet.lookupForAdd(this);
    if (p) {
        // There is another ScriptSource with the same compressed data.
        // Mark that ScriptSource as the parent and use it for all attempts to
        // get the source for this ScriptSource.
        ScriptSource* parent = *p;
        parent->incref();

        js_free(compressedData());
        dataType = DataParent;
        data.parent = parent;
    } else {
        if (rt->compressedSourceSet.add(p, this))
            inCompressedSourceSet = true;
    }
}

bool
ScriptSource::ensureOwnsSource(ExclusiveContext* cx)
{
    MOZ_ASSERT(dataType == DataUncompressed);
    if (ownsUncompressedChars())
        return true;

    char16_t* uncompressed = cx->zone()->pod_malloc<char16_t>(Max<size_t>(length_, 1));
    if (!uncompressed) {
        ReportOutOfMemory(cx);
        return false;
    }
    PodCopy(uncompressed, uncompressedChars(), length_);

    data.uncompressed.chars = uncompressed;
    data.uncompressed.ownsChars = true;
    return true;
}

bool
ScriptSource::setSourceCopy(ExclusiveContext* cx, SourceBufferHolder& srcBuf,
                            bool argumentsNotIncluded, SourceCompressionTask* task)
{
    MOZ_ASSERT(!hasSourceData());
    argumentsNotIncluded_ = argumentsNotIncluded;

    bool owns = srcBuf.ownsChars();
    setSource(owns ? srcBuf.take() : srcBuf.get(), srcBuf.length(), owns);

    // There are several cases where source compression is not a good idea:
    //  - If the script is tiny, then compression will save little or no space.
    //  - If the script is enormous, then decompression can take seconds. With
    //    lazy parsing, decompression is not uncommon, so this can significantly
    //    increase latency.
    //  - If there is only one core, then compression will contend with JS
    //    execution (which hurts benchmarketing).
    //  - If the source contains a giant string, then parsing will finish much
    //    faster than compression which increases latency (this case is handled
    //    in Parser::stringLiteral).
    //
    // Lastly, since the parsing thread will eventually perform a blocking wait
    // on the compression task's thread, require that there are at least 2
    // helper threads:
    //  - If we are on a helper thread, there must be another helper thread to
    //    execute our compression task.
    //  - If we are on the main thread, there must be at least two helper
    //    threads since at most one helper thread can be blocking on the main
    //    thread (see HelperThreadState::canStartParseTask) which would cause a
    //    deadlock if there wasn't a second helper thread that could make
    //    progress on our compression task.
    bool canCompressOffThread =
        HelperThreadState().cpuCount > 1 &&
        HelperThreadState().threadCount >= 2 &&
        CanUseExtraThreads();
    const size_t TINY_SCRIPT = 256;
    const size_t HUGE_SCRIPT = 5 * 1024 * 1024;
    if (TINY_SCRIPT <= srcBuf.length() && srcBuf.length() < HUGE_SCRIPT && canCompressOffThread) {
        task->ss = this;
        if (!StartOffThreadCompression(cx, task))
            return false;
    } else if (!ensureOwnsSource(cx)) {
        return false;
    }

    return true;
}

SourceCompressionTask::ResultType
SourceCompressionTask::work()
{
    // Try to keep the maximum memory usage down by only allocating half the
    // size of the string, first.
    size_t inputBytes = ss->length() * sizeof(char16_t);
    size_t firstSize = inputBytes / 2;
    compressed = js_malloc(firstSize);
    if (!compressed)
        return OOM;

    Compressor comp(reinterpret_cast<const unsigned char*>(ss->uncompressedChars()), inputBytes);
    if (!comp.init())
        return OOM;

    comp.setOutput((unsigned char*) compressed, firstSize);
    bool cont = true;
    while (cont) {
        if (abort_)
            return Aborted;

        switch (comp.compressMore()) {
          case Compressor::CONTINUE:
            break;
          case Compressor::MOREOUTPUT: {
            if (comp.outWritten() == inputBytes) {
                // The compressed string is longer than the original string.
                return Aborted;
            }

            // The compressed output is greater than half the size of the
            // original string. Reallocate to the full size.
            compressed = js_realloc(compressed, inputBytes);
            if (!compressed)
                return OOM;

            comp.setOutput((unsigned char*) compressed, inputBytes);
            break;
          }
          case Compressor::DONE:
            cont = false;
            break;
          case Compressor::OOM:
            return OOM;
        }
    }
    compressedBytes = comp.outWritten();
    compressedHash = CompressedSourceHasher::computeHash(compressed, compressedBytes);

    // Shrink the buffer to the size of the compressed data.
    if (void* newCompressed = js_realloc(compressed, compressedBytes))
        compressed = newCompressed;

    return Success;
}

ScriptSource::~ScriptSource()
{
    MOZ_ASSERT_IF(inCompressedSourceSet, dataType == DataCompressed);

    switch (dataType) {
      case DataUncompressed:
        if (ownsUncompressedChars())
            js_free(const_cast<char16_t*>(uncompressedChars()));
        break;

      case DataCompressed:
        // Script source references are only manipulated on the main thread,
        // except during off thread parsing when the source may be created
        // and used exclusively by the thread doing the parse. In this case the
        // ScriptSource might be destroyed while off the main thread, but it
        // will not have been added to the runtime's compressed source set
        // until the parse is finished on the main thread.
        if (inCompressedSourceSet)
            TlsPerThreadData.get()->runtimeFromMainThread()->compressedSourceSet.remove(this);
        js_free(compressedData());
        break;

      case DataParent:
        parent()->decref();
        break;

      default:
        break;
    }
}

void
ScriptSource::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                     JS::ScriptSourceInfo* info) const
{
    if (dataType == DataUncompressed && ownsUncompressedChars())
        info->uncompressed += mallocSizeOf(uncompressedChars());
    else if (dataType == DataCompressed)
        info->compressed += mallocSizeOf(compressedData());
    info->misc += mallocSizeOf(this) +
                  mallocSizeOf(filename_.get()) +
                  mallocSizeOf(introducerFilename_.get());
    info->numScripts++;
}

template<XDRMode mode>
bool
ScriptSource::performXDR(XDRState<mode>* xdr)
{
    uint8_t hasSource = hasSourceData();
    if (!xdr->codeUint8(&hasSource))
        return false;

    uint8_t retrievable = sourceRetrievable_;
    if (!xdr->codeUint8(&retrievable))
        return false;
    sourceRetrievable_ = retrievable;

    if (hasSource && !sourceRetrievable_) {
        if (!xdr->codeUint32(&length_))
            return false;

        uint32_t compressedLength;
        if (mode == XDR_ENCODE) {
            switch (dataType) {
              case DataUncompressed:
                compressedLength = 0;
                break;
              case DataCompressed:
                compressedLength = compressedBytes();
                break;
              case DataParent:
                compressedLength = parent()->compressedBytes();
                break;
              default:
                MOZ_CRASH();
            }
        }
        if (!xdr->codeUint32(&compressedLength))
            return false;

        {
            uint8_t argumentsNotIncluded;
            if (mode == XDR_ENCODE)
                argumentsNotIncluded = argumentsNotIncluded_;
            if (!xdr->codeUint8(&argumentsNotIncluded))
                return false;
            if (mode == XDR_DECODE)
                argumentsNotIncluded_ = argumentsNotIncluded;
        }

        size_t byteLen = compressedLength ? compressedLength : (length_ * sizeof(char16_t));
        if (mode == XDR_DECODE) {
            uint8_t* p = xdr->cx()->template pod_malloc<uint8_t>(Max<size_t>(byteLen, 1));
            if (!p || !xdr->codeBytes(p, byteLen)) {
                js_free(p);
                return false;
            }

            if (compressedLength)
                setCompressedSource(xdr->cx()->runtime(), p, compressedLength,
                                    CompressedSourceHasher::computeHash(p, compressedLength));
            else
                setSource((const char16_t*) p, length_);
        } else {
            void* p;
            switch (dataType) {
              case DataUncompressed:
                p = (void*) uncompressedChars();
                break;
              case DataCompressed:
                p = compressedData();
                break;
              case DataParent:
                p = parent()->compressedData();
                break;
              default:
                MOZ_CRASH();
            }
            if (!xdr->codeBytes(p, byteLen))
                return false;
        }
    }

    uint8_t haveSourceMap = hasSourceMapURL();
    if (!xdr->codeUint8(&haveSourceMap))
        return false;

    if (haveSourceMap) {
        uint32_t sourceMapURLLen = (mode == XDR_DECODE) ? 0 : js_strlen(sourceMapURL_.get());
        if (!xdr->codeUint32(&sourceMapURLLen))
            return false;

        if (mode == XDR_DECODE) {
            sourceMapURL_ = xdr->cx()->template make_pod_array<char16_t>(sourceMapURLLen + 1);
            if (!sourceMapURL_)
                return false;
        }
        if (!xdr->codeChars(sourceMapURL_.get(), sourceMapURLLen)) {
            if (mode == XDR_DECODE)
                sourceMapURL_ = nullptr;
            return false;
        }
        sourceMapURL_[sourceMapURLLen] = '\0';
    }

    uint8_t haveDisplayURL = hasDisplayURL();
    if (!xdr->codeUint8(&haveDisplayURL))
        return false;

    if (haveDisplayURL) {
        uint32_t displayURLLen = (mode == XDR_DECODE) ? 0 : js_strlen(displayURL_.get());
        if (!xdr->codeUint32(&displayURLLen))
            return false;

        if (mode == XDR_DECODE) {
            displayURL_ = xdr->cx()->template make_pod_array<char16_t>(displayURLLen + 1);
            if (!displayURL_)
                return false;
        }
        if (!xdr->codeChars(displayURL_.get(), displayURLLen)) {
            if (mode == XDR_DECODE)
                displayURL_ = nullptr;
            return false;
        }
        displayURL_[displayURLLen] = '\0';
    }

    uint8_t haveFilename = !!filename_;
    if (!xdr->codeUint8(&haveFilename))
        return false;

    if (haveFilename) {
        const char* fn = filename();
        if (!xdr->codeCString(&fn))
            return false;
        if (mode == XDR_DECODE && !setFilename(xdr->cx(), fn))
            return false;
    }

    return true;
}

// Format and return a cx->zone()->pod_malloc'ed URL for a generated script like:
//   {filename} line {lineno} > {introducer}
// For example:
//   foo.js line 7 > eval
// indicating code compiled by the call to 'eval' on line 7 of foo.js.
static char*
FormatIntroducedFilename(ExclusiveContext* cx, const char* filename, unsigned lineno,
                         const char* introducer)
{
    // Compute the length of the string in advance, so we can allocate a
    // buffer of the right size on the first shot.
    //
    // (JS_smprintf would be perfect, as that allocates the result
    // dynamically as it formats the string, but it won't allocate from cx,
    // and wants us to use a special free function.)
    char linenoBuf[15];
    size_t filenameLen = strlen(filename);
    size_t linenoLen = JS_snprintf(linenoBuf, 15, "%u", lineno);
    size_t introducerLen = strlen(introducer);
    size_t len = filenameLen                    +
                 6 /* == strlen(" line ") */    +
                 linenoLen                      +
                 3 /* == strlen(" > ") */       +
                 introducerLen                  +
                 1 /* \0 */;
    char* formatted = cx->zone()->pod_malloc<char>(len);
    if (!formatted) {
        ReportOutOfMemory(cx);
        return nullptr;
    }
    mozilla::DebugOnly<size_t> checkLen = JS_snprintf(formatted, len, "%s line %s > %s",
                                                      filename, linenoBuf, introducer);
    MOZ_ASSERT(checkLen == len - 1);

    return formatted;
}

bool
ScriptSource::initFromOptions(ExclusiveContext* cx, const ReadOnlyCompileOptions& options)
{
    MOZ_ASSERT(!filename_);
    MOZ_ASSERT(!introducerFilename_);

    mutedErrors_ = options.mutedErrors();

    introductionType_ = options.introductionType;
    setIntroductionOffset(options.introductionOffset);

    if (options.hasIntroductionInfo) {
        MOZ_ASSERT(options.introductionType != nullptr);
        const char* filename = options.filename() ? options.filename() : "<unknown>";
        char* formatted = FormatIntroducedFilename(cx, filename, options.introductionLineno,
                                                   options.introductionType);
        if (!formatted)
            return false;
        filename_.reset(formatted);
    } else if (options.filename()) {
        if (!setFilename(cx, options.filename()))
            return false;
    }

    if (options.introducerFilename()) {
        introducerFilename_ = DuplicateString(cx, options.introducerFilename());
        if (!introducerFilename_)
            return false;
    }

    return true;
}

bool
ScriptSource::setFilename(ExclusiveContext* cx, const char* filename)
{
    MOZ_ASSERT(!filename_);
    filename_ = DuplicateString(cx, filename);
    return filename_ != nullptr;
}

bool
ScriptSource::setDisplayURL(ExclusiveContext* cx, const char16_t* displayURL)
{
    MOZ_ASSERT(displayURL);
    if (hasDisplayURL()) {
        if (cx->isJSContext() &&
            !JS_ReportErrorFlagsAndNumber(cx->asJSContext(), JSREPORT_WARNING,
                                          GetErrorMessage, nullptr,
                                          JSMSG_ALREADY_HAS_PRAGMA, filename_.get(),
                                          "//# sourceURL"))
        {
            return false;
        }
    }
    size_t len = js_strlen(displayURL) + 1;
    if (len == 1)
        return true;

    displayURL_ = DuplicateString(cx, displayURL);
    return displayURL_ != nullptr;
}

bool
ScriptSource::setSourceMapURL(ExclusiveContext* cx, const char16_t* sourceMapURL)
{
    MOZ_ASSERT(sourceMapURL);

    size_t len = js_strlen(sourceMapURL) + 1;
    if (len == 1)
        return true;

    sourceMapURL_ = DuplicateString(cx, sourceMapURL);
    return sourceMapURL_ != nullptr;
}

size_t
ScriptSource::computedSizeOfData() const
{
    if (dataType == DataUncompressed && ownsUncompressedChars())
        return sizeof(char16_t) * length_;
    if (dataType == DataCompressed)
        return compressedBytes();
    return 0;
}

/*
 * Shared script data management.
 */

SharedScriptData*
js::SharedScriptData::new_(ExclusiveContext* cx, uint32_t codeLength,
                           uint32_t srcnotesLength, uint32_t natoms)
{
    /*
     * Ensure the atoms are aligned, as some architectures don't allow unaligned
     * access.
     */
    const uint32_t pointerSize = sizeof(JSAtom*);
    const uint32_t pointerMask = pointerSize - 1;
    const uint32_t dataOffset = offsetof(SharedScriptData, data);
    uint32_t baseLength = codeLength + srcnotesLength;
    uint32_t padding = (pointerSize - ((baseLength + dataOffset) & pointerMask)) & pointerMask;
    uint32_t length = baseLength + padding + pointerSize * natoms;

    SharedScriptData* entry = reinterpret_cast<SharedScriptData*>(
            cx->zone()->pod_malloc<uint8_t>(length + dataOffset));
    if (!entry) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    entry->length = length;
    entry->natoms = natoms;
    entry->marked = false;
    memset(entry->data + baseLength, 0, padding);

    /*
     * Call constructors to initialize the storage that will be accessed as a
     * HeapPtrAtom array via atoms().
     */
    HeapPtrAtom* atoms = entry->atoms();
    MOZ_ASSERT(reinterpret_cast<uintptr_t>(atoms) % sizeof(JSAtom*) == 0);
    for (unsigned i = 0; i < natoms; ++i)
        new (&atoms[i]) HeapPtrAtom();

    return entry;
}

/*
 * Takes ownership of its *ssd parameter and either adds it into the runtime's
 * ScriptDataTable or frees it if a matching entry already exists.
 *
 * Sets the |code| and |atoms| fields on the given JSScript.
 */
static bool
SaveSharedScriptData(ExclusiveContext* cx, Handle<JSScript*> script, SharedScriptData* ssd,
                     uint32_t nsrcnotes)
{
    MOZ_ASSERT(script != nullptr);
    MOZ_ASSERT(ssd != nullptr);

    AutoLockForExclusiveAccess lock(cx);

    ScriptBytecodeHasher::Lookup l(ssd);

    ScriptDataTable::AddPtr p = cx->scriptDataTable().lookupForAdd(l);
    if (p) {
        js_free(ssd);
        ssd = *p;
    } else {
        if (!cx->scriptDataTable().add(p, ssd)) {
            script->setCode(nullptr);
            script->atoms = nullptr;
            js_free(ssd);
            ReportOutOfMemory(cx);
            return false;
        }
    }

    /*
     * During the IGC we need to ensure that bytecode is marked whenever it is
     * accessed even if the bytecode was already in the table: at this point
     * old scripts or exceptions pointing to the bytecode may no longer be
     * reachable. This is effectively a read barrier.
     */
    if (cx->isJSContext()) {
        JSRuntime* rt = cx->asJSContext()->runtime();
        if (JS::IsIncrementalGCInProgress(rt) && rt->gc.isFullGc())
            ssd->marked = true;
    }

    script->setCode(ssd->data);
    script->atoms = ssd->atoms();
    return true;
}

static inline void
MarkScriptData(JSRuntime* rt, const jsbytecode* bytecode)
{
    /*
     * As an invariant, a ScriptBytecodeEntry should not be 'marked' outside of
     * a GC. Since SweepScriptBytecodes is only called during a full gc,
     * to preserve this invariant, only mark during a full gc.
     */
    if (rt->gc.isFullGc())
        SharedScriptData::fromBytecode(bytecode)->marked = true;
}

void
js::UnmarkScriptData(JSRuntime* rt)
{
    MOZ_ASSERT(rt->gc.isFullGc());
    ScriptDataTable& table = rt->scriptDataTable();
    for (ScriptDataTable::Enum e(table); !e.empty(); e.popFront()) {
        SharedScriptData* entry = e.front();
        entry->marked = false;
    }
}

void
js::SweepScriptData(JSRuntime* rt)
{
    MOZ_ASSERT(rt->gc.isFullGc());
    ScriptDataTable& table = rt->scriptDataTable();

    if (rt->keepAtoms())
        return;

    for (ScriptDataTable::Enum e(table); !e.empty(); e.popFront()) {
        SharedScriptData* entry = e.front();
        if (!entry->marked) {
            js_free(entry);
            e.removeFront();
        }
    }
}

void
js::FreeScriptData(JSRuntime* rt)
{
    ScriptDataTable& table = rt->scriptDataTable();
    if (!table.initialized())
        return;

    for (ScriptDataTable::Enum e(table); !e.empty(); e.popFront())
        js_free(e.front());

    table.clear();
}

/*
 * JSScript::data and SharedScriptData::data have complex,
 * manually-controlled, memory layouts.
 *
 * JSScript::data begins with some optional array headers. They are optional
 * because they often aren't needed, i.e. the corresponding arrays often have
 * zero elements. Each header has a bit in JSScript::hasArrayBits that
 * indicates if it's present within |data|; from this the offset of each
 * present array header can be computed. Each header has an accessor function
 * in JSScript that encapsulates this offset computation.
 *
 * Array type       Array elements  Accessor
 * ----------       --------------  --------
 * ConstArray       Consts          consts()
 * ObjectArray      Objects         objects()
 * ObjectArray      Regexps         regexps()
 * TryNoteArray     Try notes       trynotes()
 * BlockScopeArray  Scope notes     blockScopes()
 *
 * Then are the elements of several arrays.
 * - Most of these arrays have headers listed above (if present). For each of
 *   these, the array pointer and the array length is stored in the header.
 * - The remaining arrays have pointers and lengths that are stored directly in
 *   JSScript. This is because, unlike the others, they are nearly always
 *   non-zero length and so the optional-header space optimization isn't
 *   worthwhile.
 *
 * Array elements   Pointed to by         Length
 * --------------   -------------         ------
 * Consts           consts()->vector      consts()->length
 * Objects          objects()->vector     objects()->length
 * Regexps          regexps()->vector     regexps()->length
 * Try notes        trynotes()->vector    trynotes()->length
 * Scope notes      blockScopes()->vector blockScopes()->length
 *
 * IMPORTANT: This layout has two key properties.
 * - It ensures that everything has sufficient alignment; in particular, the
 *   consts() elements need Value alignment.
 * - It ensures there are no gaps between elements, which saves space and makes
 *   manual layout easy. In particular, in the second part, arrays with larger
 *   elements precede arrays with smaller elements.
 *
 * SharedScriptData::data contains data that can be shared within a
 * runtime. These items' layout is manually controlled to make it easier to
 * manage both during (temporary) allocation and during matching against
 * existing entries in the runtime. As the jsbytecode has to come first to
 * enable lookup by bytecode identity, SharedScriptData::data, the atoms part
 * has to manually be aligned sufficiently by adding padding after the notes
 * part.
 *
 * Array elements   Pointed to by         Length
 * --------------   -------------         ------
 * jsbytecode       code                  length
 * jsscrnote        notes()               numNotes()
 * Atoms            atoms                 natoms
 *
 * The following static assertions check JSScript::data's alignment properties.
 */

#define KEEPS_JSVAL_ALIGNMENT(T) \
    (JS_ALIGNMENT_OF(JS::Value) % JS_ALIGNMENT_OF(T) == 0 && \
     sizeof(T) % sizeof(JS::Value) == 0)

#define HAS_JSVAL_ALIGNMENT(T) \
    (JS_ALIGNMENT_OF(JS::Value) == JS_ALIGNMENT_OF(T) && \
     sizeof(T) == sizeof(JS::Value))

#define NO_PADDING_BETWEEN_ENTRIES(T1, T2) \
    (JS_ALIGNMENT_OF(T1) % JS_ALIGNMENT_OF(T2) == 0)

/*
 * These assertions ensure that there is no padding between the array headers,
 * and also that the consts() elements (which follow immediately afterward) are
 * Value-aligned.  (There is an assumption that |data| itself is Value-aligned;
 * we check this below).
 */
JS_STATIC_ASSERT(KEEPS_JSVAL_ALIGNMENT(ConstArray));
JS_STATIC_ASSERT(KEEPS_JSVAL_ALIGNMENT(ObjectArray));       /* there are two of these */
JS_STATIC_ASSERT(KEEPS_JSVAL_ALIGNMENT(TryNoteArray));
JS_STATIC_ASSERT(KEEPS_JSVAL_ALIGNMENT(BlockScopeArray));

/* These assertions ensure there is no padding required between array elements. */
JS_STATIC_ASSERT(HAS_JSVAL_ALIGNMENT(HeapValue));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(HeapValue, HeapPtrObject));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(HeapPtrObject, HeapPtrObject));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(HeapPtrObject, JSTryNote));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(JSTryNote, uint32_t));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(uint32_t, uint32_t));

JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(HeapValue, BlockScopeNote));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(BlockScopeNote, BlockScopeNote));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(JSTryNote, BlockScopeNote));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(HeapPtrObject, BlockScopeNote));
JS_STATIC_ASSERT(NO_PADDING_BETWEEN_ENTRIES(BlockScopeNote, uint32_t));

static inline size_t
ScriptDataSize(uint32_t nbindings, uint32_t nconsts, uint32_t nobjects, uint32_t nregexps,
               uint32_t ntrynotes, uint32_t nblockscopes, uint32_t nyieldoffsets)
{
    size_t size = 0;

    if (nconsts != 0)
        size += sizeof(ConstArray) + nconsts * sizeof(Value);
    if (nobjects != 0)
        size += sizeof(ObjectArray) + nobjects * sizeof(NativeObject*);
    if (nregexps != 0)
        size += sizeof(ObjectArray) + nregexps * sizeof(NativeObject*);
    if (ntrynotes != 0)
        size += sizeof(TryNoteArray) + ntrynotes * sizeof(JSTryNote);
    if (nblockscopes != 0)
        size += sizeof(BlockScopeArray) + nblockscopes * sizeof(BlockScopeNote);
    if (nyieldoffsets != 0)
        size += sizeof(YieldOffsetArray) + nyieldoffsets * sizeof(uint32_t);

    if (nbindings != 0) {
	// Make sure bindings are sufficiently aligned.
        size = JS_ROUNDUP(size, JS_ALIGNMENT_OF(Binding)) + nbindings * sizeof(Binding);
    }

    return size;
}

void
JSScript::initCompartment(ExclusiveContext* cx)
{
    compartment_ = cx->compartment_;
}

/* static */ JSScript*
JSScript::Create(ExclusiveContext* cx, HandleObject enclosingScope, bool savedCallerFun,
                 const ReadOnlyCompileOptions& options, HandleObject sourceObject,
                 uint32_t bufStart, uint32_t bufEnd)
{
    MOZ_ASSERT(bufStart <= bufEnd);

    RootedScript script(cx, Allocate<JSScript>(cx));
    if (!script)
        return nullptr;

    PodZero(script.get());
    new (&script->bindings) Bindings;

    script->enclosingStaticScope_ = enclosingScope;
    script->savedCallerFun_ = savedCallerFun;
    script->initCompartment(cx);

    script->selfHosted_ = options.selfHostingMode;
    script->noScriptRval_ = options.noScriptRval;
    script->treatAsRunOnce_ = options.isRunOnce;

    // Compute whether this script is under a non-syntactic scope. We don't
    // need to walk the entire static scope chain if the script is nested in a
    // function. In that case, we can propagate the cached value from the
    // outer script.
    script->hasNonSyntacticScope_ = HasNonSyntacticStaticScopeChain(enclosingScope);

    script->version = options.version;
    MOZ_ASSERT(script->getVersion() == options.version);     // assert that no overflow occurred

    script->setSourceObject(sourceObject);
    script->sourceStart_ = bufStart;
    script->sourceEnd_ = bufEnd;

    return script;
}

static inline uint8_t*
AllocScriptData(JS::Zone* zone, size_t size)
{
    if (!size)
        return nullptr;

    uint8_t* data = zone->pod_calloc<uint8_t>(JS_ROUNDUP(size, sizeof(Value)));
    if (!data)
        return nullptr;
    MOZ_ASSERT(size_t(data) % sizeof(Value) == 0);
    return data;
}

/* static */ bool
JSScript::partiallyInit(ExclusiveContext* cx, HandleScript script, uint32_t nconsts,
                        uint32_t nobjects, uint32_t nregexps, uint32_t ntrynotes,
                        uint32_t nblockscopes, uint32_t nyieldoffsets, uint32_t nTypeSets)
{
    size_t size = ScriptDataSize(script->bindings.count(), nconsts, nobjects, nregexps, ntrynotes,
                                 nblockscopes, nyieldoffsets);
    script->data = AllocScriptData(script->zone(), size);
    if (size && !script->data) {
        ReportOutOfMemory(cx);
        return false;
    }
    script->dataSize_ = size;

    MOZ_ASSERT(nTypeSets <= UINT16_MAX);
    script->nTypeSets_ = uint16_t(nTypeSets);

    uint8_t* cursor = script->data;
    if (nconsts != 0) {
        script->setHasArray(CONSTS);
        cursor += sizeof(ConstArray);
    }
    if (nobjects != 0) {
        script->setHasArray(OBJECTS);
        cursor += sizeof(ObjectArray);
    }
    if (nregexps != 0) {
        script->setHasArray(REGEXPS);
        cursor += sizeof(ObjectArray);
    }
    if (ntrynotes != 0) {
        script->setHasArray(TRYNOTES);
        cursor += sizeof(TryNoteArray);
    }
    if (nblockscopes != 0) {
        script->setHasArray(BLOCK_SCOPES);
        cursor += sizeof(BlockScopeArray);
    }

    YieldOffsetArray* yieldOffsets = nullptr;
    if (nyieldoffsets != 0) {
        yieldOffsets = reinterpret_cast<YieldOffsetArray*>(cursor);
        cursor += sizeof(YieldOffsetArray);
    }

    if (nconsts != 0) {
        MOZ_ASSERT(reinterpret_cast<uintptr_t>(cursor) % sizeof(JS::Value) == 0);
        script->consts()->length = nconsts;
        script->consts()->vector = (HeapValue*)cursor;
        cursor += nconsts * sizeof(script->consts()->vector[0]);
    }

    if (nobjects != 0) {
        script->objects()->length = nobjects;
        script->objects()->vector = (HeapPtrObject*)cursor;
        cursor += nobjects * sizeof(script->objects()->vector[0]);
    }

    if (nregexps != 0) {
        script->regexps()->length = nregexps;
        script->regexps()->vector = (HeapPtrObject*)cursor;
        cursor += nregexps * sizeof(script->regexps()->vector[0]);
    }

    if (ntrynotes != 0) {
        script->trynotes()->length = ntrynotes;
        script->trynotes()->vector = reinterpret_cast<JSTryNote*>(cursor);
        size_t vectorSize = ntrynotes * sizeof(script->trynotes()->vector[0]);
#ifdef DEBUG
        memset(cursor, 0, vectorSize);
#endif
        cursor += vectorSize;
    }

    if (nblockscopes != 0) {
        script->blockScopes()->length = nblockscopes;
        script->blockScopes()->vector = reinterpret_cast<BlockScopeNote*>(cursor);
        size_t vectorSize = nblockscopes * sizeof(script->blockScopes()->vector[0]);
#ifdef DEBUG
        memset(cursor, 0, vectorSize);
#endif
        cursor += vectorSize;
    }

    if (nyieldoffsets != 0) {
        yieldOffsets->init(reinterpret_cast<uint32_t*>(cursor), nyieldoffsets);
        size_t vectorSize = nyieldoffsets * sizeof(script->yieldOffsets()[0]);
#ifdef DEBUG
        memset(cursor, 0, vectorSize);
#endif
        cursor += vectorSize;
    }

    if (script->bindings.count() != 0) {
        // Make sure bindings are sufficiently aligned.
        cursor = reinterpret_cast<uint8_t*>
            (JS_ROUNDUP(reinterpret_cast<uintptr_t>(cursor), JS_ALIGNMENT_OF(Binding)));
    }
    cursor = script->bindings.switchToScriptStorage(reinterpret_cast<Binding*>(cursor));

    MOZ_ASSERT(cursor == script->data + size);
    return true;
}

/* static */ bool
JSScript::fullyInitTrivial(ExclusiveContext* cx, Handle<JSScript*> script)
{
    if (!script->bindings.initTrivial(cx))
        return false;

    if (!partiallyInit(cx, script, 0, 0, 0, 0, 0, 0, 0))
        return false;

    SharedScriptData* ssd = SharedScriptData::new_(cx, 1, 1, 0);
    if (!ssd)
        return false;

    ssd->data[0] = JSOP_RETRVAL;
    ssd->data[1] = SRC_NULL;
    script->setLength(1);
    return SaveSharedScriptData(cx, script, ssd, 1);
}

/* static */ void
JSScript::linkToFunctionFromEmitter(js::ExclusiveContext* cx, JS::Handle<JSScript*> script,
                                    js::frontend::FunctionBox* funbox)
{
    script->funHasExtensibleScope_ = funbox->hasExtensibleScope();
    script->funNeedsDeclEnvObject_ = funbox->needsDeclEnvObject();
    script->needsHomeObject_       = funbox->needsHomeObject();
    script->isDerivedClassConstructor_ = funbox->isDerivedClassConstructor();

    if (funbox->argumentsHasLocalBinding()) {
        script->setArgumentsHasVarBinding();
        if (funbox->definitelyNeedsArgsObj())
            script->setNeedsArgsObj(true);
    } else {
        MOZ_ASSERT(!funbox->definitelyNeedsArgsObj());
    }
    script->hasMappedArgsObj_ = funbox->hasMappedArgsObj();

    script->functionHasThisBinding_ = funbox->hasThisBinding();

    script->funLength_ = funbox->length;

    script->isGeneratorExp_ = funbox->inGenexpLambda;
    script->setGeneratorKind(funbox->generatorKind());

    // Link the function and the script to each other, so that StaticScopeIter
    // may walk the scope chain of currently compiling scripts.
    RootedFunction fun(cx, funbox->function());
    MOZ_ASSERT(fun->isInterpreted());

    script->setFunction(fun);

    if (fun->isInterpretedLazy())
        fun->setUnlazifiedScript(script);
    else
        fun->setScript(script);
}

/* static */ void
JSScript::linkToModuleFromEmitter(js::ExclusiveContext* cx, JS::Handle<JSScript*> script,
                                    js::frontend::ModuleBox* modulebox)
{
    script->funHasExtensibleScope_ = false;
    script->funNeedsDeclEnvObject_ = false;
    script->needsHomeObject_ = false;
    script->isDerivedClassConstructor_ = false;
    script->funLength_ = 0;

    script->isGeneratorExp_ = false;
    script->setGeneratorKind(NotGenerator);

    // Link the module and the script to each other, so that StaticScopeIter
    // may walk the scope chain of currently compiling scripts.
    RootedModuleObject module(cx, modulebox->module());
    script->setModule(module);
}

/* static */ bool
JSScript::fullyInitFromEmitter(ExclusiveContext* cx, HandleScript script, BytecodeEmitter* bce)
{
    /* The counts of indexed things must be checked during code generation. */
    MOZ_ASSERT(bce->atomIndices->count() <= INDEX_LIMIT);
    MOZ_ASSERT(bce->objectList.length <= INDEX_LIMIT);
    MOZ_ASSERT(bce->regexpList.length <= INDEX_LIMIT);

    uint32_t mainLength = bce->offset();
    uint32_t prologueLength = bce->prologueOffset();
    uint32_t nsrcnotes;
    if (!bce->finishTakingSrcNotes(&nsrcnotes))
        return false;
    uint32_t natoms = bce->atomIndices->count();
    if (!partiallyInit(cx, script,
                       bce->constList.length(), bce->objectList.length, bce->regexpList.length,
                       bce->tryNoteList.length(), bce->blockScopeList.length(),
                       bce->yieldOffsetList.length(), bce->typesetCount))
    {
        return false;
    }

    MOZ_ASSERT(script->mainOffset() == 0);
    script->mainOffset_ = prologueLength;

    script->lineno_ = bce->firstLine;

    script->setLength(prologueLength + mainLength);
    script->natoms_ = natoms;
    SharedScriptData* ssd = SharedScriptData::new_(cx, script->length(), nsrcnotes, natoms);
    if (!ssd)
        return false;

    jsbytecode* code = ssd->data;
    PodCopy<jsbytecode>(code, bce->prologue.code.begin(), prologueLength);
    PodCopy<jsbytecode>(code + prologueLength, bce->code().begin(), mainLength);
    bce->copySrcNotes((jssrcnote*)(code + script->length()), nsrcnotes);
    InitAtomMap(bce->atomIndices.getMap(), ssd->atoms());

    if (!SaveSharedScriptData(cx, script, ssd, nsrcnotes))
        return false;

#ifdef DEBUG
    FunctionBox* funbox = bce->sc->isFunctionBox() ? bce->sc->asFunctionBox() : nullptr;

    // Assert that the properties set by linkToFunctionFromEmitter are
    // correct.
    if (funbox) {
        MOZ_ASSERT(script->funHasExtensibleScope_ == funbox->hasExtensibleScope());
        MOZ_ASSERT(script->funNeedsDeclEnvObject_ == funbox->needsDeclEnvObject());
        MOZ_ASSERT(script->needsHomeObject_ == funbox->needsHomeObject());
        MOZ_ASSERT(script->isDerivedClassConstructor_ == funbox->isDerivedClassConstructor());
        MOZ_ASSERT(script->argumentsHasVarBinding() == funbox->argumentsHasLocalBinding());
        MOZ_ASSERT(script->hasMappedArgsObj() == funbox->hasMappedArgsObj());
        MOZ_ASSERT(script->functionHasThisBinding() == funbox->hasThisBinding());
        MOZ_ASSERT(script->functionNonDelazifying() == funbox->function());
        MOZ_ASSERT(script->isGeneratorExp_ == funbox->inGenexpLambda);
        MOZ_ASSERT(script->generatorKind() == funbox->generatorKind());
    } else {
        MOZ_ASSERT(!script->funHasExtensibleScope_);
        MOZ_ASSERT(!script->funNeedsDeclEnvObject_);
        MOZ_ASSERT(!script->needsHomeObject_);
        MOZ_ASSERT(!script->isDerivedClassConstructor_);
        MOZ_ASSERT(!script->argumentsHasVarBinding());
        MOZ_ASSERT(!script->hasMappedArgsObj());
        MOZ_ASSERT(!script->isGeneratorExp_);
        MOZ_ASSERT(script->generatorKind() == NotGenerator);
    }
#endif

    if (bce->constList.length() != 0)
        bce->constList.finish(script->consts());
    if (bce->objectList.length != 0)
        bce->objectList.finish(script->objects());
    if (bce->regexpList.length != 0)
        bce->regexpList.finish(script->regexps());
    if (bce->tryNoteList.length() != 0)
        bce->tryNoteList.finish(script->trynotes());
    if (bce->blockScopeList.length() != 0)
        bce->blockScopeList.finish(script->blockScopes(), prologueLength);
    script->strict_ = bce->sc->strict();
    script->explicitUseStrict_ = bce->sc->hasExplicitUseStrict();
    script->bindingsAccessedDynamically_ = bce->sc->bindingsAccessedDynamically();
    script->hasSingletons_ = bce->hasSingletons;

    if (bce->yieldOffsetList.length() != 0)
        bce->yieldOffsetList.finish(script->yieldOffsets(), prologueLength);

    // The call to nfixed() depends on the above setFunction() call.
    if (UINT32_MAX - script->nfixed() < bce->maxStackDepth) {
        bce->reportError(nullptr, JSMSG_NEED_DIET, "script");
        return false;
    }
    script->nslots_ = script->nfixed() + bce->maxStackDepth;

    for (unsigned i = 0, n = script->bindings.numArgs(); i < n; ++i) {
        if (script->formalIsAliased(i)) {
            script->funHasAnyAliasedFormal_ = true;
            break;
        }
    }

    return true;
}

size_t
JSScript::computedSizeOfData() const
{
    return dataSize();
}

size_t
JSScript::sizeOfData(mozilla::MallocSizeOf mallocSizeOf) const
{
    return mallocSizeOf(data);
}

size_t
JSScript::sizeOfTypeScript(mozilla::MallocSizeOf mallocSizeOf) const
{
    return types_->sizeOfIncludingThis(mallocSizeOf);
}

/*
 * Nb: srcnotes are variable-length.  This function computes the number of
 * srcnote *slots*, which may be greater than the number of srcnotes.
 */
uint32_t
JSScript::numNotes()
{
    jssrcnote* sn;
    jssrcnote* notes_ = notes();
    for (sn = notes_; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn))
        continue;
    return sn - notes_ + 1;    /* +1 for the terminator */
}

js::GlobalObject&
JSScript::uninlinedGlobal() const
{
    return global();
}

void
JSScript::fixEnclosingStaticGlobalLexicalScope()
{
    MOZ_ASSERT(IsStaticGlobalLexicalScope(enclosingStaticScope_));
    enclosingStaticScope_ = &global().lexicalScope().staticBlock();
}

void
LazyScript::fixEnclosingStaticGlobalLexicalScope()
{
    MOZ_ASSERT(IsStaticGlobalLexicalScope(enclosingScope_));
    enclosingScope_ = &function_->global().lexicalScope().staticBlock();
}

void
JSScript::finalize(FreeOp* fop)
{
    // NOTE: this JSScript may be partially initialized at this point.  E.g. we
    // may have created it and partially initialized it with
    // JSScript::Create(), but not yet finished initializing it with
    // fullyInitFromEmitter() or fullyInitTrivial().

    // Collect code coverage information for this script and all its inner
    // scripts, and store the aggregated information on the compartment.
    if (fop->runtime()->lcovOutput.isEnabled())
        compartment()->lcovOutput.collectCodeCoverageInfo(compartment(), sourceObject(), this);

    fop->runtime()->spsProfiler.onScriptFinalized(this);

    if (types_)
        types_->destroy();

    jit::DestroyJitScripts(fop, this);

    destroyScriptCounts(fop);
    destroyDebugScript(fop);

    if (data) {
        JS_POISON(data, 0xdb, computedSizeOfData());
        fop->free_(data);
    }

    fop->runtime()->lazyScriptCache.remove(this);

    // In most cases, our LazyScript's script pointer will reference this
    // script, and thus be nulled out by normal weakref processing. However, if
    // we unlazified the LazyScript during incremental sweeping, it will have a
    // completely different JSScript.
    MOZ_ASSERT_IF(lazyScript && !IsAboutToBeFinalizedUnbarriered(&lazyScript),
                  !lazyScript->hasScript() || lazyScript->maybeScriptUnbarriered() != this);
}

static const uint32_t GSN_CACHE_THRESHOLD = 100;

void
GSNCache::purge()
{
    code = nullptr;
    if (map.initialized())
        map.finish();
}

jssrcnote*
js::GetSrcNote(GSNCache& cache, JSScript* script, jsbytecode* pc)
{
    size_t target = pc - script->code();
    if (target >= script->length())
        return nullptr;

    if (cache.code == script->code()) {
        MOZ_ASSERT(cache.map.initialized());
        GSNCache::Map::Ptr p = cache.map.lookup(pc);
        return p ? p->value() : nullptr;
    }

    size_t offset = 0;
    jssrcnote* result;
    for (jssrcnote* sn = script->notes(); ; sn = SN_NEXT(sn)) {
        if (SN_IS_TERMINATOR(sn)) {
            result = nullptr;
            break;
        }
        offset += SN_DELTA(sn);
        if (offset == target && SN_IS_GETTABLE(sn)) {
            result = sn;
            break;
        }
    }

    if (cache.code != script->code() && script->length() >= GSN_CACHE_THRESHOLD) {
        unsigned nsrcnotes = 0;
        for (jssrcnote* sn = script->notes(); !SN_IS_TERMINATOR(sn);
             sn = SN_NEXT(sn))
        {
            if (SN_IS_GETTABLE(sn))
                ++nsrcnotes;
        }
        if (cache.code) {
            MOZ_ASSERT(cache.map.initialized());
            cache.map.finish();
            cache.code = nullptr;
        }
        if (cache.map.init(nsrcnotes)) {
            pc = script->code();
            for (jssrcnote* sn = script->notes(); !SN_IS_TERMINATOR(sn);
                 sn = SN_NEXT(sn))
            {
                pc += SN_DELTA(sn);
                if (SN_IS_GETTABLE(sn))
                    cache.map.putNewInfallible(pc, sn);
            }
            cache.code = script->code();
        }
    }

    return result;
}

jssrcnote*
js::GetSrcNote(JSContext* cx, JSScript* script, jsbytecode* pc)
{
    return GetSrcNote(cx->runtime()->gsnCache, script, pc);
}

unsigned
js::PCToLineNumber(unsigned startLine, jssrcnote* notes, jsbytecode* code, jsbytecode* pc,
                   unsigned* columnp)
{
    unsigned lineno = startLine;
    unsigned column = 0;

    /*
     * Walk through source notes accumulating their deltas, keeping track of
     * line-number notes, until we pass the note for pc's offset within
     * script->code.
     */
    ptrdiff_t offset = 0;
    ptrdiff_t target = pc - code;
    for (jssrcnote* sn = notes; !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        offset += SN_DELTA(sn);
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            if (offset <= target)
                lineno = unsigned(GetSrcNoteOffset(sn, 0));
            column = 0;
        } else if (type == SRC_NEWLINE) {
            if (offset <= target)
                lineno++;
            column = 0;
        }

        if (offset > target)
            break;

        if (type == SRC_COLSPAN) {
            ptrdiff_t colspan = SN_OFFSET_TO_COLSPAN(GetSrcNoteOffset(sn, 0));
            MOZ_ASSERT(ptrdiff_t(column) + colspan >= 0);
            column += colspan;
        }
    }

    if (columnp)
        *columnp = column;

    return lineno;
}

unsigned
js::PCToLineNumber(JSScript* script, jsbytecode* pc, unsigned* columnp)
{
    /* Cope with InterpreterFrame.pc value prior to entering Interpret. */
    if (!pc)
        return 0;

    return PCToLineNumber(script->lineno(), script->notes(), script->code(), pc, columnp);
}

jsbytecode*
js::LineNumberToPC(JSScript* script, unsigned target)
{
    ptrdiff_t offset = 0;
    ptrdiff_t best = -1;
    unsigned lineno = script->lineno();
    unsigned bestdiff = SN_MAX_OFFSET;
    for (jssrcnote* sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        /*
         * Exact-match only if offset is not in the prologue; otherwise use
         * nearest greater-or-equal line number match.
         */
        if (lineno == target && offset >= ptrdiff_t(script->mainOffset()))
            goto out;
        if (lineno >= target) {
            unsigned diff = lineno - target;
            if (diff < bestdiff) {
                bestdiff = diff;
                best = offset;
            }
        }
        offset += SN_DELTA(sn);
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE) {
            lineno = unsigned(GetSrcNoteOffset(sn, 0));
        } else if (type == SRC_NEWLINE) {
            lineno++;
        }
    }
    if (best >= 0)
        offset = best;
out:
    return script->offsetToPC(offset);
}

JS_FRIEND_API(unsigned)
js::GetScriptLineExtent(JSScript* script)
{
    unsigned lineno = script->lineno();
    unsigned maxLineNo = lineno;
    for (jssrcnote* sn = script->notes(); !SN_IS_TERMINATOR(sn); sn = SN_NEXT(sn)) {
        SrcNoteType type = (SrcNoteType) SN_TYPE(sn);
        if (type == SRC_SETLINE)
            lineno = unsigned(GetSrcNoteOffset(sn, 0));
        else if (type == SRC_NEWLINE)
            lineno++;

        if (maxLineNo < lineno)
            maxLineNo = lineno;
    }

    return 1 + maxLineNo - script->lineno();
}

void
js::DescribeScriptedCallerForCompilation(JSContext* cx, MutableHandleScript maybeScript,
                                         const char** file, unsigned* linenop,
                                         uint32_t* pcOffset, bool* mutedErrors,
                                         LineOption opt)
{
    if (opt == CALLED_FROM_JSOP_EVAL) {
        jsbytecode* pc = nullptr;
        maybeScript.set(cx->currentScript(&pc));
        static_assert(JSOP_SPREADEVAL_LENGTH == JSOP_STRICTSPREADEVAL_LENGTH,
                    "next op after a spread must be at consistent offset");
        static_assert(JSOP_EVAL_LENGTH == JSOP_STRICTEVAL_LENGTH,
                    "next op after a direct eval must be at consistent offset");
        MOZ_ASSERT(JSOp(*pc) == JSOP_EVAL || JSOp(*pc) == JSOP_STRICTEVAL ||
                   JSOp(*pc) == JSOP_SPREADEVAL || JSOp(*pc) == JSOP_STRICTSPREADEVAL);

        bool isSpread = JSOp(*pc) == JSOP_SPREADEVAL || JSOp(*pc) == JSOP_STRICTSPREADEVAL;
        jsbytecode* nextpc = pc + (isSpread ? JSOP_SPREADEVAL_LENGTH : JSOP_EVAL_LENGTH);
        MOZ_ASSERT(*nextpc == JSOP_LINENO);

        *file = maybeScript->filename();
        *linenop = GET_UINT32(nextpc);
        *pcOffset = pc - maybeScript->code();
        *mutedErrors = maybeScript->mutedErrors();
        return;
    }

    NonBuiltinFrameIter iter(cx);

    if (iter.done()) {
        maybeScript.set(nullptr);
        *file = nullptr;
        *linenop = 0;
        *pcOffset = 0;
        *mutedErrors = false;
        return;
    }

    *file = iter.scriptFilename();
    *linenop = iter.computeLine();
    *mutedErrors = iter.mutedErrors();

    // These values are only used for introducer fields which are debugging
    // information and can be safely left null for asm.js frames.
    if (iter.hasScript()) {
        maybeScript.set(iter.script());
        *pcOffset = iter.pc() - maybeScript->code();
    } else {
        maybeScript.set(nullptr);
        *pcOffset = 0;
    }
}

template <class T>
static inline T*
Rebase(JSScript* dst, JSScript* src, T* srcp)
{
    size_t off = reinterpret_cast<uint8_t*>(srcp) - src->data;
    return reinterpret_cast<T*>(dst->data + off);
}

static JSObject*
CloneInnerInterpretedFunction(JSContext* cx, HandleObject enclosingScope, HandleFunction srcFun)
{
    /* NB: Keep this in sync with XDRInterpretedFunction. */
    RootedObject cloneProto(cx);
    if (srcFun->isStarGenerator()) {
        cloneProto = GlobalObject::getOrCreateStarGeneratorFunctionPrototype(cx, cx->global());
        if (!cloneProto)
            return nullptr;
    }

    gc::AllocKind allocKind = srcFun->getAllocKind();
    RootedFunction clone(cx, NewFunctionWithProto(cx, nullptr, 0,
                                                  JSFunction::INTERPRETED, nullptr, nullptr,
                                                  cloneProto, allocKind, TenuredObject));
    if (!clone)
        return nullptr;

    JSScript::AutoDelazify srcScript(cx, srcFun);
    if (!srcScript)
        return nullptr;
    JSScript* cloneScript = CloneScriptIntoFunction(cx, enclosingScope, clone, srcScript);
    if (!cloneScript)
        return nullptr;

    clone->setArgCount(srcFun->nargs());
    clone->setFlags(srcFun->flags());
    clone->initAtom(srcFun->displayAtom());
    if (!JSFunction::setTypeForScriptedFunction(cx, clone))
        return nullptr;

    return clone;
}

bool
js::detail::CopyScript(JSContext* cx, HandleObject scriptStaticScope, HandleScript src,
                       HandleScript dst)
{
    if (src->treatAsRunOnce() && !src->functionNonDelazifying()) {
        // Toplevel run-once scripts may not be cloned.
        JS_ReportError(cx, "No cloning toplevel run-once scripts");
        return false;
    }

    /* NB: Keep this in sync with XDRScript. */

    /* Some embeddings are not careful to use ExposeObjectToActiveJS as needed. */
    MOZ_ASSERT(!src->sourceObject()->asTenured().isMarked(gc::GRAY));

    uint32_t nconsts   = src->hasConsts()   ? src->consts()->length   : 0;
    uint32_t nobjects  = src->hasObjects()  ? src->objects()->length  : 0;
    uint32_t nregexps  = src->hasRegexps()  ? src->regexps()->length  : 0;
    uint32_t ntrynotes = src->hasTrynotes() ? src->trynotes()->length : 0;
    uint32_t nblockscopes = src->hasBlockScopes() ? src->blockScopes()->length : 0;
    uint32_t nyieldoffsets = src->hasYieldOffsets() ? src->yieldOffsets().length() : 0;

    /* Script data */

    size_t size = src->dataSize();
    ScopedJSFreePtr<uint8_t> data(AllocScriptData(cx->zone(), size));
    if (size && !data) {
        ReportOutOfMemory(cx);
        return false;
    }

    /* Bindings */

    Rooted<Bindings> bindings(cx);
    if (!Bindings::clone(cx, &bindings, data, src))
        return false;

    /* Objects */

    AutoObjectVector objects(cx);
    if (nobjects != 0) {
        HeapPtrObject* vector = src->objects()->vector;
        for (unsigned i = 0; i < nobjects; i++) {
            RootedObject obj(cx, vector[i]);
            RootedObject clone(cx);
            if (obj->is<NestedScopeObject>()) {
                Rooted<NestedScopeObject*> innerBlock(cx, &obj->as<NestedScopeObject>());

                RootedObject enclosingScope(cx);
                if (NestedScopeObject* enclosingBlock = innerBlock->enclosingNestedScope()) {
                    if (IsStaticGlobalLexicalScope(enclosingBlock)) {
                        MOZ_ASSERT(IsStaticGlobalLexicalScope(scriptStaticScope) ||
                                   scriptStaticScope->is<StaticNonSyntacticScopeObjects>());
                        enclosingScope = scriptStaticScope;
                    } else {
                        enclosingScope = objects[FindScopeObjectIndex(src, *enclosingBlock)];
                    }
                } else {
                    enclosingScope = scriptStaticScope;
                }

                clone = CloneNestedScopeObject(cx, enclosingScope, innerBlock);
            } else if (obj->is<JSFunction>()) {
                RootedFunction innerFun(cx, &obj->as<JSFunction>());
                if (innerFun->isNative()) {
                    if (cx->compartment() != innerFun->compartment()) {
                        MOZ_ASSERT(innerFun->isAsmJSNative());
                        JS_ReportError(cx, "AsmJS modules do not yet support cloning.");
                        return false;
                    }
                    clone = innerFun;
                } else {
                    if (innerFun->isInterpretedLazy()) {
                        AutoCompartment ac(cx, innerFun);
                        if (!innerFun->getOrCreateScript(cx))
                            return false;
                    }
                    RootedObject staticScope(cx, innerFun->nonLazyScript()->enclosingStaticScope());
                    StaticScopeIter<CanGC> ssi(cx, staticScope);
                    RootedObject enclosingScope(cx);
                    if (ssi.done() || ssi.type() == StaticScopeIter<CanGC>::NonSyntactic) {
                        enclosingScope = scriptStaticScope;
                    } else if (ssi.type() == StaticScopeIter<CanGC>::Function) {
                        MOZ_ASSERT(scriptStaticScope->is<JSFunction>());
                        enclosingScope = scriptStaticScope;
                    } else if (ssi.type() == StaticScopeIter<CanGC>::Block) {
                        if (ssi.block().isGlobal()) {
                            MOZ_ASSERT(IsStaticGlobalLexicalScope(scriptStaticScope) ||
                                       scriptStaticScope->is<StaticNonSyntacticScopeObjects>());
                            enclosingScope = scriptStaticScope;
                        } else {
                            enclosingScope = objects[FindScopeObjectIndex(src, ssi.block())];
                        }
                    } else {
                        enclosingScope = objects[FindScopeObjectIndex(src, ssi.staticWith())];
                    }

                    clone = CloneInnerInterpretedFunction(cx, enclosingScope, innerFun);
                }
            } else {
                clone = DeepCloneObjectLiteral(cx, obj, TenuredObject);
            }
            if (!clone || !objects.append(clone))
                return false;
        }
    }

    /* RegExps */

    AutoObjectVector regexps(cx);
    for (unsigned i = 0; i < nregexps; i++) {
        HeapPtrObject* vector = src->regexps()->vector;
        for (unsigned i = 0; i < nregexps; i++) {
            JSObject* clone = CloneScriptRegExpObject(cx, vector[i]->as<RegExpObject>());
            if (!clone || !regexps.append(clone))
                return false;
        }
    }

    /* Now that all fallible allocation is complete, do the copying. */

    dst->bindings = bindings;

    /* This assignment must occur before all the Rebase calls. */
    dst->data = data.forget();
    dst->dataSize_ = size;
    memcpy(dst->data, src->data, size);

    /* Script filenames, bytecodes and atoms are runtime-wide. */
    dst->setCode(src->code());
    dst->atoms = src->atoms;

    dst->setLength(src->length());
    dst->lineno_ = src->lineno();
    dst->mainOffset_ = src->mainOffset();
    dst->natoms_ = src->natoms();
    dst->funLength_ = src->funLength();
    dst->nTypeSets_ = src->nTypeSets();
    dst->nslots_ = src->nslots();
    if (src->argumentsHasVarBinding()) {
        dst->setArgumentsHasVarBinding();
        if (src->analyzedArgsUsage())
            dst->setNeedsArgsObj(src->needsArgsObj());
    }
    dst->hasMappedArgsObj_ = src->hasMappedArgsObj();
    dst->functionHasThisBinding_ = src->functionHasThisBinding();
    dst->cloneHasArray(src);
    dst->strict_ = src->strict();
    dst->explicitUseStrict_ = src->explicitUseStrict();
    dst->bindingsAccessedDynamically_ = src->bindingsAccessedDynamically();
    dst->funHasExtensibleScope_ = src->funHasExtensibleScope();
    dst->funNeedsDeclEnvObject_ = src->funNeedsDeclEnvObject();
    dst->funHasAnyAliasedFormal_ = src->funHasAnyAliasedFormal();
    dst->hasSingletons_ = src->hasSingletons();
    dst->treatAsRunOnce_ = src->treatAsRunOnce();
    dst->hasInnerFunctions_ = src->hasInnerFunctions();
    dst->isGeneratorExp_ = src->isGeneratorExp();
    dst->setGeneratorKind(src->generatorKind());

    if (nconsts != 0) {
        HeapValue* vector = Rebase<HeapValue>(dst, src, src->consts()->vector);
        dst->consts()->vector = vector;
        for (unsigned i = 0; i < nconsts; ++i)
            MOZ_ASSERT_IF(vector[i].isMarkable(), vector[i].toString()->isAtom());
    }
    if (nobjects != 0) {
        HeapPtrObject* vector = Rebase<HeapPtrObject>(dst, src, src->objects()->vector);
        dst->objects()->vector = vector;
        for (unsigned i = 0; i < nobjects; ++i)
            vector[i].init(&objects[i]->as<NativeObject>());
    }
    if (nregexps != 0) {
        HeapPtrObject* vector = Rebase<HeapPtrObject>(dst, src, src->regexps()->vector);
        dst->regexps()->vector = vector;
        for (unsigned i = 0; i < nregexps; ++i)
            vector[i].init(&regexps[i]->as<NativeObject>());
    }
    if (ntrynotes != 0)
        dst->trynotes()->vector = Rebase<JSTryNote>(dst, src, src->trynotes()->vector);
    if (nblockscopes != 0)
        dst->blockScopes()->vector = Rebase<BlockScopeNote>(dst, src, src->blockScopes()->vector);
    if (nyieldoffsets != 0)
        dst->yieldOffsets().vector_ = Rebase<uint32_t>(dst, src, src->yieldOffsets().vector_);

    /*
     * Function delazification assumes that their script does not have a
     * non-syntactic global scope.  We ensure that as follows:
     *
     * 1) Initial parsing only creates lazy functions if
     *    !hasNonSyntacticScope.
     * 2) Cloning a lazy function into a non-global scope will always require
     *    that its script be cloned.  See comments in
     *    CloneFunctionObjectUseSameScript.
     * 3) Cloning a script never sets a lazyScript on the clone, so the function
     *    cannot be relazified.
     *
     * If you decide that lazy functions should be supported with a
     * non-syntactic global scope, make sure delazification can deal.
     */
    MOZ_ASSERT_IF(dst->hasNonSyntacticScope(), !dst->maybeLazyScript());
    MOZ_ASSERT_IF(dst->hasNonSyntacticScope(), !dst->isRelazifiable());
    return true;
}

static JSScript*
CreateEmptyScriptForClone(JSContext* cx, HandleObject enclosingScope, HandleScript src)
{
    /*
     * Wrap the script source object as needed. Self-hosted scripts may be
     * in another runtime, so lazily create a new script source object to
     * use for them.
     */
    RootedObject sourceObject(cx);
    if (cx->runtime()->isSelfHostingCompartment(src->compartment())) {
        if (!cx->compartment()->selfHostingScriptSource) {
            CompileOptions options(cx);
            FillSelfHostingCompileOptions(options);

            ScriptSourceObject* obj = frontend::CreateScriptSourceObject(cx, options);
            if (!obj)
                return nullptr;
            cx->compartment()->selfHostingScriptSource.set(obj);
        }
        sourceObject = cx->compartment()->selfHostingScriptSource;
    } else {
        sourceObject = src->sourceObject();
        if (!cx->compartment()->wrap(cx, &sourceObject))
            return nullptr;
    }

    CompileOptions options(cx);
    options.setMutedErrors(src->mutedErrors())
           .setSelfHostingMode(src->selfHosted())
           .setNoScriptRval(src->noScriptRval())
           .setVersion(src->getVersion());

    return JSScript::Create(cx, enclosingScope, src->savedCallerFun(),
                            options, sourceObject, src->sourceStart(), src->sourceEnd());
}

JSScript*
js::CloneGlobalScript(JSContext* cx, Handle<ScopeObject*> enclosingScope, HandleScript src)
{
    MOZ_ASSERT(IsStaticGlobalLexicalScope(enclosingScope) ||
               enclosingScope->is<StaticNonSyntacticScopeObjects>());

    RootedScript dst(cx, CreateEmptyScriptForClone(cx, enclosingScope, src));
    if (!dst)
        return nullptr;

    if (!detail::CopyScript(cx, enclosingScope, src, dst))
        return nullptr;

    return dst;
}

JSScript*
js::CloneScriptIntoFunction(JSContext* cx, HandleObject enclosingScope, HandleFunction fun,
                            HandleScript src)
{
    MOZ_ASSERT(fun->isInterpreted());

    // Allocate the destination script up front and set it as the script of
    // |fun|, which is to be its container.
    //
    // This is so that when cloning nested functions, they can walk the static
    // scope chain via fun and correctly compute the presence of a
    // non-syntactic global.
    RootedScript dst(cx, CreateEmptyScriptForClone(cx, enclosingScope, src));
    if (!dst)
        return nullptr;

    // Save flags in case we need to undo the early mutations.
    const int preservedFlags = fun->flags();

    dst->setFunction(fun);
    Rooted<LazyScript*> lazy(cx);
    if (fun->isInterpretedLazy()) {
        lazy = fun->lazyScriptOrNull();
        fun->setUnlazifiedScript(dst);
    } else {
        fun->initScript(dst);
    }

    if (!detail::CopyScript(cx, fun, src, dst)) {
        if (lazy)
            fun->initLazyScript(lazy);
        else
            fun->setScript(nullptr);
        fun->setFlags(preservedFlags);
        return nullptr;
    }

    return dst;
}

DebugScript*
JSScript::debugScript()
{
    MOZ_ASSERT(hasDebugScript_);
    DebugScriptMap* map = compartment()->debugScriptMap;
    MOZ_ASSERT(map);
    DebugScriptMap::Ptr p = map->lookup(this);
    MOZ_ASSERT(p);
    return p->value();
}

DebugScript*
JSScript::releaseDebugScript()
{
    MOZ_ASSERT(hasDebugScript_);
    DebugScriptMap* map = compartment()->debugScriptMap;
    MOZ_ASSERT(map);
    DebugScriptMap::Ptr p = map->lookup(this);
    MOZ_ASSERT(p);
    DebugScript* debug = p->value();
    map->remove(p);
    hasDebugScript_ = false;
    return debug;
}

void
JSScript::destroyDebugScript(FreeOp* fop)
{
    if (hasDebugScript_) {
#ifdef DEBUG
        for (jsbytecode* pc = code(); pc < codeEnd(); pc++) {
            if (BreakpointSite* site = getBreakpointSite(pc)) {
                /* Breakpoints are swept before finalization. */
                MOZ_ASSERT(site->firstBreakpoint() == nullptr);
                MOZ_ASSERT(getBreakpointSite(pc) == nullptr);
            }
        }
#endif
        fop->free_(releaseDebugScript());
    }
}

bool
JSScript::ensureHasDebugScript(JSContext* cx)
{
    if (hasDebugScript_)
        return true;

    size_t nbytes = offsetof(DebugScript, breakpoints) + length() * sizeof(BreakpointSite*);
    DebugScript* debug = (DebugScript*) zone()->pod_calloc<uint8_t>(nbytes);
    if (!debug)
        return false;

    /* Create compartment's debugScriptMap if necessary. */
    DebugScriptMap* map = compartment()->debugScriptMap;
    if (!map) {
        map = cx->new_<DebugScriptMap>();
        if (!map || !map->init()) {
            js_free(debug);
            js_delete(map);
            return false;
        }
        compartment()->debugScriptMap = map;
    }

    if (!map->putNew(this, debug)) {
        js_free(debug);
        return false;
    }
    hasDebugScript_ = true; // safe to set this;  we can't fail after this point

    /*
     * Ensure that any Interpret() instances running on this script have
     * interrupts enabled. The interrupts must stay enabled until the
     * debug state is destroyed.
     */
    for (ActivationIterator iter(cx->runtime()); !iter.done(); ++iter) {
        if (iter->isInterpreter())
            iter->asInterpreter()->enableInterruptsIfRunning(this);
    }

    return true;
}

void
JSScript::setNewStepMode(FreeOp* fop, uint32_t newValue)
{
    DebugScript* debug = debugScript();
    uint32_t prior = debug->stepMode;
    debug->stepMode = newValue;

    if (!prior != !newValue) {
        if (hasBaselineScript())
            baseline->toggleDebugTraps(this, nullptr);

        if (!stepModeEnabled() && !debug->numSites)
            fop->free_(releaseDebugScript());
    }
}

bool
JSScript::incrementStepModeCount(JSContext* cx)
{
    assertSameCompartment(cx, this);
    MOZ_ASSERT(cx->compartment()->isDebuggee());

    if (!ensureHasDebugScript(cx))
        return false;

    DebugScript* debug = debugScript();
    uint32_t count = debug->stepMode;
    setNewStepMode(cx->runtime()->defaultFreeOp(), count + 1);
    return true;
}

void
JSScript::decrementStepModeCount(FreeOp* fop)
{
    DebugScript* debug = debugScript();
    uint32_t count = debug->stepMode;
    MOZ_ASSERT(count > 0);
    setNewStepMode(fop, count - 1);
}

BreakpointSite*
JSScript::getOrCreateBreakpointSite(JSContext* cx, jsbytecode* pc)
{
    if (!ensureHasDebugScript(cx))
        return nullptr;

    DebugScript* debug = debugScript();
    BreakpointSite*& site = debug->breakpoints[pcToOffset(pc)];

    if (!site) {
        site = cx->runtime()->new_<BreakpointSite>(this, pc);
        if (!site) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        debug->numSites++;
    }

    return site;
}

void
JSScript::destroyBreakpointSite(FreeOp* fop, jsbytecode* pc)
{
    DebugScript* debug = debugScript();
    BreakpointSite*& site = debug->breakpoints[pcToOffset(pc)];
    MOZ_ASSERT(site);

    fop->delete_(site);
    site = nullptr;

    if (--debug->numSites == 0 && !stepModeEnabled())
        fop->free_(releaseDebugScript());
}

void
JSScript::clearBreakpointsIn(FreeOp* fop, js::Debugger* dbg, JSObject* handler)
{
    if (!hasAnyBreakpointsOrStepMode())
        return;

    for (jsbytecode* pc = code(); pc < codeEnd(); pc++) {
        BreakpointSite* site = getBreakpointSite(pc);
        if (site) {
            Breakpoint* nextbp;
            for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
                nextbp = bp->nextInSite();
                if ((!dbg || bp->debugger == dbg) && (!handler || bp->getHandler() == handler))
                    bp->destroy(fop);
            }
        }
    }
}

bool
JSScript::hasBreakpointsAt(jsbytecode* pc)
{
    BreakpointSite* site = getBreakpointSite(pc);
    if (!site)
        return false;

    return site->enabledCount > 0;
}

void
JSScript::traceChildren(JSTracer* trc)
{
    // NOTE: this JSScript may be partially initialized at this point.  E.g. we
    // may have created it and partially initialized it with
    // JSScript::Create(), but not yet finished initializing it with
    // fullyInitFromEmitter() or fullyInitTrivial().

    MOZ_ASSERT_IF(trc->isMarkingTracer() &&
                  static_cast<GCMarker*>(trc)->shouldCheckCompartments(),
                  zone()->isCollecting());

    if (atoms) {
        for (uint32_t i = 0; i < natoms(); ++i) {
            if (atoms[i])
                TraceEdge(trc, &atoms[i], "atom");
        }
    }

    if (hasObjects()) {
        ObjectArray* objarray = objects();
        TraceRange(trc, objarray->length, objarray->vector, "objects");
    }

    if (hasRegexps()) {
        ObjectArray* objarray = regexps();
        TraceRange(trc, objarray->length, objarray->vector, "regexps");
    }

    if (hasConsts()) {
        ConstArray* constarray = consts();
        TraceRange(trc, constarray->length, constarray->vector, "consts");
    }

    if (sourceObject()) {
        MOZ_ASSERT(MaybeForwarded(sourceObject())->compartment() == compartment());
        TraceEdge(trc, &sourceObject_, "sourceObject");
    }

    if (functionNonDelazifying())
        TraceEdge(trc, &function_, "function");

    if (module_)
        TraceEdge(trc, &module_, "module");

    if (enclosingStaticScope_)
        TraceEdge(trc, &enclosingStaticScope_, "enclosingStaticScope");

    if (maybeLazyScript())
        TraceManuallyBarrieredEdge(trc, &lazyScript, "lazyScript");

    if (trc->isMarkingTracer()) {
        compartment()->mark();

        if (code())
            MarkScriptData(trc->runtime(), code());
    }

    bindings.trace(trc);

    jit::TraceJitScripts(trc, this);
}

void
LazyScript::finalize(FreeOp* fop)
{
    if (table_)
        fop->free_(table_);
}

size_t
JSScript::calculateLiveFixed(jsbytecode* pc)
{
    size_t nlivefixed = nbodyfixed();

    if (nfixed() != nlivefixed) {
        NestedScopeObject* staticScope = getStaticBlockScope(pc);
        if (staticScope)
            staticScope = MaybeForwarded(staticScope);
        while (staticScope && !staticScope->is<StaticBlockObject>()) {
            staticScope = staticScope->enclosingNestedScope();
            if (staticScope)
                staticScope = MaybeForwarded(staticScope);
        }

        if (staticScope && !IsStaticGlobalLexicalScope(staticScope)) {
            StaticBlockObject& blockObj = staticScope->as<StaticBlockObject>();
            nlivefixed = blockObj.localOffset() + blockObj.numVariables();
        }
    }

    MOZ_ASSERT(nlivefixed <= nfixed());
    MOZ_ASSERT(nlivefixed >= nbodyfixed());

    return nlivefixed;
}

NestedScopeObject*
JSScript::getStaticBlockScope(jsbytecode* pc)
{
    MOZ_ASSERT(containsPC(pc));

    if (!hasBlockScopes())
        return nullptr;

    size_t offset = pc - code();

    BlockScopeArray* scopes = blockScopes();
    NestedScopeObject* blockChain = nullptr;

    // Find the innermost block chain using a binary search.
    size_t bottom = 0;
    size_t top = scopes->length;

    while (bottom < top) {
        size_t mid = bottom + (top - bottom) / 2;
        const BlockScopeNote* note = &scopes->vector[mid];
        if (note->start <= offset) {
            // Block scopes are ordered in the list by their starting offset, and since
            // blocks form a tree ones earlier in the list may cover the pc even if
            // later blocks end before the pc. This only happens when the earlier block
            // is a parent of the later block, so we need to check parents of |mid| in
            // the searched range for coverage.
            size_t check = mid;
            while (check >= bottom) {
                const BlockScopeNote* checkNote = &scopes->vector[check];
                MOZ_ASSERT(checkNote->start <= offset);
                if (offset < checkNote->start + checkNote->length) {
                    // We found a matching block chain but there may be inner ones
                    // at a higher block chain index than mid. Continue the binary search.
                    if (checkNote->index == BlockScopeNote::NoBlockScopeIndex)
                        blockChain = nullptr;
                    else
                        blockChain = &getObject(checkNote->index)->as<NestedScopeObject>();
                    break;
                }
                if (checkNote->parent == UINT32_MAX)
                    break;
                check = checkNote->parent;
            }
            bottom = mid + 1;
        } else {
            top = mid;
        }
    }

    return blockChain;
}

JSObject*
JSScript::innermostStaticScopeInScript(jsbytecode* pc)
{
    if (JSObject* scope = getStaticBlockScope(pc))
        return scope;
    if (module())
        return module();
    return functionNonDelazifying();
}

JSObject*
JSScript::innermostStaticScope(jsbytecode* pc)
{
    if (JSObject* scope = innermostStaticScopeInScript(pc))
        return scope;
    return enclosingStaticScope();
}

void
JSScript::setArgumentsHasVarBinding()
{
    argsHasVarBinding_ = true;
    needsArgsAnalysis_ = true;
}

void
JSScript::setNeedsArgsObj(bool needsArgsObj)
{
    MOZ_ASSERT_IF(needsArgsObj, argumentsHasVarBinding());
    needsArgsAnalysis_ = false;
    needsArgsObj_ = needsArgsObj;
}

void
js::SetFrameArgumentsObject(JSContext* cx, AbstractFramePtr frame,
                            HandleScript script, JSObject* argsobj)
{
    /*
     * Replace any optimized arguments in the frame with an explicit arguments
     * object. Note that 'arguments' may have already been overwritten.
     */

    BindingIter bi = Bindings::argumentsBinding(cx, script);

    if (script->bindingIsAliased(bi)) {
        /*
         * Scan the script to find the slot in the call object that 'arguments'
         * is assigned to.
         */
        jsbytecode* pc = script->code();
        while (*pc != JSOP_ARGUMENTS)
            pc += GetBytecodeLength(pc);
        pc += JSOP_ARGUMENTS_LENGTH;
        MOZ_ASSERT(*pc == JSOP_SETALIASEDVAR);

        // Note that here and below, it is insufficient to only check for
        // JS_OPTIMIZED_ARGUMENTS, as Ion could have optimized out the
        // arguments slot.
        if (IsOptimizedPlaceholderMagicValue(frame.callObj().as<ScopeObject>().aliasedVar(ScopeCoordinate(pc))))
            frame.callObj().as<ScopeObject>().setAliasedVar(cx, ScopeCoordinate(pc), cx->names().arguments, ObjectValue(*argsobj));
    } else {
        if (IsOptimizedPlaceholderMagicValue(frame.unaliasedLocal(bi.frameIndex())))
            frame.unaliasedLocal(bi.frameIndex()) = ObjectValue(*argsobj);
    }
}

/* static */ bool
JSScript::argumentsOptimizationFailed(JSContext* cx, HandleScript script)
{
    MOZ_ASSERT(script->functionNonDelazifying());
    MOZ_ASSERT(script->analyzedArgsUsage());
    MOZ_ASSERT(script->argumentsHasVarBinding());

    /*
     * It is possible that the arguments optimization has already failed,
     * everything has been fixed up, but there was an outstanding magic value
     * on the stack that has just now flowed into an apply. In this case, there
     * is nothing to do; GuardFunApplySpeculation will patch in the real
     * argsobj.
     */
    if (script->needsArgsObj())
        return true;

    MOZ_ASSERT(!script->isGenerator());

    script->needsArgsObj_ = true;

    /*
     * Since we can't invalidate baseline scripts, set a flag that's checked from
     * JIT code to indicate the arguments optimization failed and JSOP_ARGUMENTS
     * should create an arguments object next time.
     */
    if (script->hasBaselineScript())
        script->baselineScript()->setNeedsArgsObj();

    /*
     * By design, the arguments optimization is only made when there are no
     * outstanding cases of MagicValue(JS_OPTIMIZED_ARGUMENTS) at any points
     * where the optimization could fail, other than an active invocation of
     * 'f.apply(x, arguments)'. Thus, there are no outstanding values of
     * MagicValue(JS_OPTIMIZED_ARGUMENTS) on the stack. However, there are
     * three things that need fixup:
     *  - there may be any number of activations of this script that don't have
     *    an argsObj that now need one.
     *  - jit code compiled (and possible active on the stack) with the static
     *    assumption of !script->needsArgsObj();
     *  - type inference data for the script assuming script->needsArgsObj
     */
    for (AllFramesIter i(cx); !i.done(); ++i) {
        /*
         * We cannot reliably create an arguments object for Ion activations of
         * this script.  To maintain the invariant that "script->needsArgsObj
         * implies fp->hasArgsObj", the Ion bail mechanism will create an
         * arguments object right after restoring the BaselineFrame and before
         * entering Baseline code (in jit::FinishBailoutToBaseline).
         */
        if (i.isIon())
            continue;
        AbstractFramePtr frame = i.abstractFramePtr();
        if (frame.isFunctionFrame() && frame.script() == script) {
            /* We crash on OOM since cleaning up here would be complicated. */
            AutoEnterOOMUnsafeRegion oomUnsafe;
            ArgumentsObject* argsobj = ArgumentsObject::createExpected(cx, frame);
            if (!argsobj)
                oomUnsafe.crash("JSScript::argumentsOptimizationFailed");
            SetFrameArgumentsObject(cx, frame, script, argsobj);
        }
    }

    return true;
}

bool
JSScript::bindingIsAliased(const BindingIter& bi)
{
    return bindings.bindingIsAliased(bi.i_);
}

bool
JSScript::formalIsAliased(unsigned argSlot)
{
    MOZ_ASSERT(argSlot < bindings.numArgs());
    return bindings.bindingIsAliased(argSlot);
}

bool
JSScript::localIsAliased(unsigned localSlot)
{
    return bindings.bindingIsAliased(bindings.numArgs() + localSlot);
}

bool
JSScript::formalLivesInArgumentsObject(unsigned argSlot)
{
    return argsObjAliasesFormals() && !formalIsAliased(argSlot);
}

LazyScript::LazyScript(JSFunction* fun, void* table, uint64_t packedFields, uint32_t begin, uint32_t end, uint32_t lineno, uint32_t column)
  : script_(nullptr),
    function_(fun),
    enclosingScope_(nullptr),
    sourceObject_(nullptr),
    table_(table),
    packedFields_(packedFields),
    begin_(begin),
    end_(end),
    lineno_(lineno),
    column_(column)
{
    MOZ_ASSERT(begin <= end);
}

void
LazyScript::initScript(JSScript* script)
{
    MOZ_ASSERT(script);
    MOZ_ASSERT(!script_.unbarrieredGet());
    script_.set(script);
}

void
LazyScript::resetScript()
{
    MOZ_ASSERT(script_.unbarrieredGet());
    script_.set(nullptr);
}

void
LazyScript::setParent(JSObject* enclosingScope, ScriptSourceObject* sourceObject)
{
    MOZ_ASSERT(!sourceObject_ && !enclosingScope_);
    MOZ_ASSERT_IF(enclosingScope, function_->compartment() == enclosingScope->compartment());
    MOZ_ASSERT(function_->compartment() == sourceObject->compartment());

    enclosingScope_ = enclosingScope;
    sourceObject_ = sourceObject;
}

ScriptSourceObject*
LazyScript::sourceObject() const
{
    return sourceObject_ ? &sourceObject_->as<ScriptSourceObject>() : nullptr;
}

ScriptSource*
LazyScript::maybeForwardedScriptSource() const
{
    return UncheckedUnwrap(MaybeForwarded(sourceObject()))->as<ScriptSourceObject>().source();
}

/* static */ LazyScript*
LazyScript::CreateRaw(ExclusiveContext* cx, HandleFunction fun,
                      uint64_t packedFields, uint32_t begin, uint32_t end,
                      uint32_t lineno, uint32_t column)
{
    union {
        PackedView p;
        uint64_t packed;
    };

    packed = packedFields;

    // Reset runtime flags to obtain a fresh LazyScript.
    p.hasBeenCloned = false;
    p.treatAsRunOnce = false;

    size_t bytes = (p.numFreeVariables * sizeof(FreeVariable))
                 + (p.numInnerFunctions * sizeof(HeapPtrFunction));

    ScopedJSFreePtr<uint8_t> table(bytes ? fun->zone()->pod_malloc<uint8_t>(bytes) : nullptr);
    if (bytes && !table) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    LazyScript* res = Allocate<LazyScript>(cx);
    if (!res)
        return nullptr;

    cx->compartment()->scheduleDelazificationForDebugger();

    return new (res) LazyScript(fun, table.forget(), packed, begin, end, lineno, column);
}

/* static */ LazyScript*
LazyScript::CreateRaw(ExclusiveContext* cx, HandleFunction fun,
                      uint32_t numFreeVariables, uint32_t numInnerFunctions, JSVersion version,
                      uint32_t begin, uint32_t end, uint32_t lineno, uint32_t column)
{
    union {
        PackedView p;
        uint64_t packedFields;
    };

    p.version = version;
    p.numFreeVariables = numFreeVariables;
    p.numInnerFunctions = numInnerFunctions;
    p.generatorKindBits = GeneratorKindAsBits(NotGenerator);
    p.strict = false;
    p.bindingsAccessedDynamically = false;
    p.hasDebuggerStatement = false;
    p.hasDirectEval = false;
    p.usesArgumentsApplyAndThis = false;
    p.isDerivedClassConstructor = false;
    p.needsHomeObject = false;

    LazyScript* res = LazyScript::CreateRaw(cx, fun, packedFields, begin, end, lineno, column);
    MOZ_ASSERT_IF(res, res->version() == version);
    return res;
}

/* static */ LazyScript*
LazyScript::Create(ExclusiveContext* cx, HandleFunction fun,
                   HandleScript script, HandleObject enclosingScope,
                   HandleScript sourceObjectScript,
                   uint64_t packedFields, uint32_t begin, uint32_t end,
                   uint32_t lineno, uint32_t column)
{
    // Dummy atom which is not a valid property name.
    RootedAtom dummyAtom(cx, cx->names().comma);

    // Dummy function which is not a valid function as this is the one which is
    // holding this lazy script.
    HandleFunction dummyFun = fun;

    LazyScript* res = LazyScript::CreateRaw(cx, fun, packedFields, begin, end, lineno, column);
    if (!res)
        return nullptr;

    // Fill with dummies, to be GC-safe after the initialization of the free
    // variables and inner functions.
    size_t i, num;
    FreeVariable* variables = res->freeVariables();
    for (i = 0, num = res->numFreeVariables(); i < num; i++)
        variables[i] = FreeVariable(dummyAtom);

    HeapPtrFunction* functions = res->innerFunctions();
    for (i = 0, num = res->numInnerFunctions(); i < num; i++)
        functions[i].init(dummyFun);

    // Set the enclosing scope of the lazy function, this would later be
    // used to define the environment when the function would be used.
    MOZ_ASSERT(!res->sourceObject());
    res->setParent(enclosingScope, &sourceObjectScript->scriptSourceUnwrap());

    MOZ_ASSERT(!res->hasScript());
    if (script)
        res->initScript(script);

    return res;
}

void
LazyScript::initRuntimeFields(uint64_t packedFields)
{
    union {
        PackedView p;
        uint64_t packed;
    };

    packed = packedFields;
    p_.hasBeenCloned = p.hasBeenCloned;
    p_.treatAsRunOnce = p.treatAsRunOnce;
}

bool
LazyScript::hasUncompiledEnclosingScript() const
{
    // It can happen that we created lazy scripts while compiling an enclosing
    // script, but we errored out while compiling that script. When we iterate
    // over lazy script in a compartment, we might see lazy scripts that never
    // escaped to script and should be ignored.
    //
    // If the enclosing scope is a function with a null script or has a script
    // without code, it was not successfully compiled.

    if (!enclosingScope() || !enclosingScope()->is<JSFunction>())
        return false;

    JSFunction& fun = enclosingScope()->as<JSFunction>();
    return !fun.hasScript() || fun.hasUncompiledScript() || !fun.nonLazyScript()->code();
}

void
JSScript::updateBaselineOrIonRaw(JSContext* maybecx)
{
    if (hasBaselineScript() && baseline->hasPendingIonBuilder()) {
        MOZ_ASSERT(maybecx);
        MOZ_ASSERT(!isIonCompilingOffThread());
        baselineOrIonRaw = maybecx->runtime()->jitRuntime()->lazyLinkStub()->raw();
        baselineOrIonSkipArgCheck = maybecx->runtime()->jitRuntime()->lazyLinkStub()->raw();
    } else if (hasIonScript()) {
        baselineOrIonRaw = ion->method()->raw();
        baselineOrIonSkipArgCheck = ion->method()->raw() + ion->getSkipArgCheckEntryOffset();
    } else if (hasBaselineScript()) {
        baselineOrIonRaw = baseline->method()->raw();
        baselineOrIonSkipArgCheck = baseline->method()->raw();
    } else {
        baselineOrIonRaw = nullptr;
        baselineOrIonSkipArgCheck = nullptr;
    }
}

bool
JSScript::hasLoops()
{
    if (!hasTrynotes())
        return false;
    JSTryNote* tn = trynotes()->vector;
    JSTryNote* tnlimit = tn + trynotes()->length;
    for (; tn < tnlimit; tn++) {
        if (tn->kind == JSTRY_FOR_IN || tn->kind == JSTRY_LOOP)
            return true;
    }
    return false;
}

bool
JSScript::mayReadFrameArgsDirectly()
{
    return argumentsHasVarBinding() || (function_ && function_->hasRest());
}

static inline void
LazyScriptHash(uint32_t lineno, uint32_t column, uint32_t begin, uint32_t end,
               HashNumber hashes[3])
{
    HashNumber hash = lineno;
    hash = RotateLeft(hash, 4) ^ column;
    hash = RotateLeft(hash, 4) ^ begin;
    hash = RotateLeft(hash, 4) ^ end;

    hashes[0] = hash;
    hashes[1] = RotateLeft(hashes[0], 4) ^ begin;
    hashes[2] = RotateLeft(hashes[1], 4) ^ end;
}

void
LazyScriptHashPolicy::hash(const Lookup& lookup, HashNumber hashes[3])
{
    LazyScript* lazy = lookup.lazy;
    LazyScriptHash(lazy->lineno(), lazy->column(), lazy->begin(), lazy->end(), hashes);
}

void
LazyScriptHashPolicy::hash(JSScript* script, HashNumber hashes[3])
{
    LazyScriptHash(script->lineno(), script->column(), script->sourceStart(), script->sourceEnd(), hashes);
}

bool
LazyScriptHashPolicy::match(JSScript* script, const Lookup& lookup)
{
    JSContext* cx = lookup.cx;
    LazyScript* lazy = lookup.lazy;

    // To be a match, the script and lazy script need to have the same line
    // and column and to be at the same position within their respective
    // source blobs, and to have the same source contents and version.
    //
    // While the surrounding code in the source may differ, this is
    // sufficient to ensure that compiling the lazy script will yield an
    // identical result to compiling the original script.
    //
    // Note that the filenames and origin principals of the lazy script and
    // original script can differ. If there is a match, these will be fixed
    // up in the resulting clone by the caller.

    if (script->lineno() != lazy->lineno() ||
        script->column() != lazy->column() ||
        script->getVersion() != lazy->version() ||
        script->sourceStart() != lazy->begin() ||
        script->sourceEnd() != lazy->end())
    {
        return false;
    }

    UncompressedSourceCache::AutoHoldEntry holder;

    const char16_t* scriptChars = script->scriptSource()->chars(cx, holder);
    if (!scriptChars)
        return false;

    const char16_t* lazyChars = lazy->scriptSource()->chars(cx, holder);
    if (!lazyChars)
        return false;

    size_t begin = script->sourceStart();
    size_t length = script->sourceEnd() - begin;
    return !memcmp(scriptChars + begin, lazyChars + begin, length);
}

void
JSScript::AutoDelazify::holdScript(JS::HandleFunction fun)
{
    if (fun) {
        if (fun->compartment()->isSelfHosting) {
            // The self-hosting compartment is shared across runtimes, so we
            // can't use JSAutoCompartment: it could cause races. Functions in
            // the self-hosting compartment will never be lazy, so we can safely
            // assume we don't have to delazify.
            script_ = fun->nonLazyScript();
        } else {
            JSAutoCompartment ac(cx_, fun);
            script_ = fun->getOrCreateScript(cx_);
            if (script_) {
                oldDoNotRelazify_ = script_->doNotRelazify_;
                script_->setDoNotRelazify(true);
            }
        }
    }
}

void
JSScript::AutoDelazify::dropScript()
{
    // Don't touch script_ if it's in the self-hosting compartment, see the
    // comment in holdScript.
    if (script_ && !script_->compartment()->isSelfHosting)
        script_->setDoNotRelazify(oldDoNotRelazify_);
    script_ = nullptr;
}

JS::ubi::Node::Size
JS::ubi::Concrete<JSScript>::size(mozilla::MallocSizeOf mallocSizeOf) const
{
    Size size = Arena::thingSize(get().asTenured().getAllocKind());

    size += get().sizeOfData(mallocSizeOf);
    size += get().sizeOfTypeScript(mallocSizeOf);

    size_t baselineSize = 0;
    size_t baselineStubsSize = 0;
    jit::AddSizeOfBaselineData(&get(), mallocSizeOf, &baselineSize, &baselineStubsSize);
    size += baselineSize;
    size += baselineStubsSize;

    size += jit::SizeOfIonData(&get(), mallocSizeOf);

    MOZ_ASSERT(size > 0);
    return size;
}

const char*
JS::ubi::Concrete<JSScript>::scriptFilename() const
{
    return get().filename();
}

JS::ubi::Node::Size
JS::ubi::Concrete<js::LazyScript>::size(mozilla::MallocSizeOf mallocSizeOf) const
{
    Size size = js::gc::Arena::thingSize(get().asTenured().getAllocKind());
    size += get().sizeOfExcludingThis(mallocSizeOf);
    return size;
}

const char*
JS::ubi::Concrete<js::LazyScript>::scriptFilename() const
{
    auto sourceObject = get().sourceObject();
    if (!sourceObject)
        return nullptr;

    auto source = sourceObject->source();
    if (!source)
        return nullptr;

    return source->filename();
}
