/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineInspector_h
#define jit_BaselineInspector_h

#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/MIR.h"

namespace js {
namespace jit {

class BaselineInspector;

class ICInspector
{
  protected:
    BaselineInspector* inspector_;
    jsbytecode* pc_;
    ICEntry* icEntry_;

    ICInspector(BaselineInspector* inspector, jsbytecode* pc, ICEntry* icEntry)
      : inspector_(inspector), pc_(pc), icEntry_(icEntry)
    { }
};

class SetElemICInspector : public ICInspector
{
  public:
    SetElemICInspector(BaselineInspector* inspector, jsbytecode* pc, ICEntry* icEntry)
      : ICInspector(inspector, pc, icEntry)
    { }

    bool sawOOBDenseWrite() const;
    bool sawOOBTypedArrayWrite() const;
    bool sawDenseWrite() const;
    bool sawTypedArrayWrite() const;
};

class BaselineInspector
{
  private:
    JSScript* script;
    ICEntry* prevLookedUpEntry;

  public:
    explicit BaselineInspector(JSScript* script)
      : script(script), prevLookedUpEntry(nullptr)
    {
        MOZ_ASSERT(script);
    }

    bool hasBaselineScript() const {
        return script->hasBaselineScript();
    }

    BaselineScript* baselineScript() const {
        return script->baselineScript();
    }

  private:
#ifdef DEBUG
    bool isValidPC(jsbytecode* pc) {
        return script->containsPC(pc);
    }
#endif

    ICEntry& icEntryFromPC(jsbytecode* pc) {
        MOZ_ASSERT(hasBaselineScript());
        MOZ_ASSERT(isValidPC(pc));
        ICEntry& ent = baselineScript()->icEntryFromPCOffset(script->pcToOffset(pc), prevLookedUpEntry);
        MOZ_ASSERT(ent.isForOp());
        prevLookedUpEntry = &ent;
        return ent;
    }

    template <typename ICInspectorType>
    ICInspectorType makeICInspector(jsbytecode* pc, ICStub::Kind expectedFallbackKind) {
        ICEntry* ent = nullptr;
        if (hasBaselineScript()) {
            ent = &icEntryFromPC(pc);
            MOZ_ASSERT(ent->fallbackStub()->kind() == expectedFallbackKind);
        }
        return ICInspectorType(this, pc, ent);
    }

    ICStub* monomorphicStub(jsbytecode* pc);
    bool dimorphicStub(jsbytecode* pc, ICStub** pfirst, ICStub** psecond);

  public:
    typedef Vector<ReceiverGuard, 4, JitAllocPolicy> ReceiverVector;
    typedef Vector<ObjectGroup*, 4, JitAllocPolicy> ObjectGroupVector;
    bool maybeInfoForPropertyOp(jsbytecode* pc, ReceiverVector& receivers,
                                ObjectGroupVector& convertUnboxedGroups);

    SetElemICInspector setElemICInspector(jsbytecode* pc) {
        return makeICInspector<SetElemICInspector>(pc, ICStub::SetElem_Fallback);
    }

    MIRType expectedResultType(jsbytecode* pc);
    MCompare::CompareType expectedCompareType(jsbytecode* pc);
    MIRType expectedBinaryArithSpecialization(jsbytecode* pc);
    MIRType expectedPropertyAccessInputType(jsbytecode* pc);

    bool hasSeenNonNativeGetElement(jsbytecode* pc);
    bool hasSeenNegativeIndexGetElement(jsbytecode* pc);
    bool hasSeenAccessedGetter(jsbytecode* pc);
    bool hasSeenDoubleResult(jsbytecode* pc);
    bool hasSeenNonStringIterMore(jsbytecode* pc);

    bool isOptimizableCallStringSplit(jsbytecode* pc, JSString** stringOut, JSString** stringArg,
                                      JSObject** objOut);
    JSObject* getTemplateObject(jsbytecode* pc);
    JSObject* getTemplateObjectForNative(jsbytecode* pc, Native native);
    JSObject* getTemplateObjectForClassHook(jsbytecode* pc, const Class* clasp);

    // Sometimes the group a template object will have is known, even if the
    // object itself isn't.
    ObjectGroup* getTemplateObjectGroup(jsbytecode* pc);

    JSFunction* getSingleCallee(jsbytecode* pc);

    DeclEnvObject* templateDeclEnvObject();
    CallObject* templateCallObject();

    bool commonGetPropFunction(jsbytecode* pc, JSObject** holder, Shape** holderShape,
                               JSFunction** commonGetter, Shape** globalShape, bool* isOwnProperty,
                               ReceiverVector& receivers, ObjectGroupVector& convertUnboxedGroups);
    bool commonSetPropFunction(jsbytecode* pc, JSObject** holder, Shape** holderShape,
                               JSFunction** commonSetter, bool* isOwnProperty,
                               ReceiverVector& receivers, ObjectGroupVector& convertUnboxedGroups);

    bool instanceOfData(jsbytecode* pc, Shape** shape, uint32_t* slot, JSObject** prototypeObject);
};

} // namespace jit
} // namespace js

#endif /* jit_BaselineInspector_h */
