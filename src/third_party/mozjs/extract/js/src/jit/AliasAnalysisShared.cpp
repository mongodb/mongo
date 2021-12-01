/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/AliasAnalysisShared.h"

#include "jit/MIR.h"

namespace js {
namespace jit {

void
AliasAnalysisShared::spewDependencyList()
{
#ifdef JS_JITSPEW
    if (JitSpewEnabled(JitSpew_AliasSummaries)) {
        Fprinter &print = JitSpewPrinter();
        JitSpewHeader(JitSpew_AliasSummaries);
        print.printf("Dependency list for other passes:\n");

        for (ReversePostorderIterator block(graph_.rpoBegin()); block != graph_.rpoEnd(); block++) {
            for (MInstructionIterator def(block->begin()), end(block->begin(block->lastIns()));
                 def != end;
                 ++def)
            {
                if (!def->dependency())
                    continue;
                if (!def->getAliasSet().isLoad())
                    continue;

                JitSpewHeader(JitSpew_AliasSummaries);
                print.printf(" ");
                MDefinition::PrintOpcodeName(print, def->op());
                print.printf("%d marked depending on ", def->id());
                MDefinition::PrintOpcodeName(print, def->dependency()->op());
                print.printf("%d\n", def->dependency()->id());
            }
        }
    }
#endif
}

// Unwrap any slot or element to its corresponding object.
static inline const MDefinition*
MaybeUnwrap(const MDefinition* object)
{

    while (object->isSlots() || object->isElements() || object->isConvertElementsToDoubles()) {
        MOZ_ASSERT(object->numOperands() == 1);
        object = object->getOperand(0);
    }

    if (object->isTypedArrayElements())
        return nullptr;
    if (object->isTypedObjectElements())
        return nullptr;
    if (object->isConstantElements())
        return nullptr;

    return object;
}

// Get the object of any load/store. Returns nullptr if not tied to
// an object.
static inline const MDefinition*
GetObject(const MDefinition* ins)
{
    if (!ins->getAliasSet().isStore() && !ins->getAliasSet().isLoad())
        return nullptr;

    // Note: only return the object if that objects owns that property.
    // I.e. the poperty isn't on the prototype chain.
    const MDefinition* object = nullptr;
    switch (ins->op()) {
      case MDefinition::Opcode::InitializedLength:
      case MDefinition::Opcode::LoadElement:
      case MDefinition::Opcode::LoadUnboxedScalar:
      case MDefinition::Opcode::LoadUnboxedObjectOrNull:
      case MDefinition::Opcode::LoadUnboxedString:
      case MDefinition::Opcode::StoreElement:
      case MDefinition::Opcode::StoreUnboxedObjectOrNull:
      case MDefinition::Opcode::StoreUnboxedString:
      case MDefinition::Opcode::StoreUnboxedScalar:
      case MDefinition::Opcode::SetInitializedLength:
      case MDefinition::Opcode::ArrayLength:
      case MDefinition::Opcode::SetArrayLength:
      case MDefinition::Opcode::StoreElementHole:
      case MDefinition::Opcode::FallibleStoreElement:
      case MDefinition::Opcode::TypedObjectDescr:
      case MDefinition::Opcode::Slots:
      case MDefinition::Opcode::Elements:
      case MDefinition::Opcode::MaybeCopyElementsForWrite:
      case MDefinition::Opcode::MaybeToDoubleElement:
      case MDefinition::Opcode::TypedArrayLength:
      case MDefinition::Opcode::SetTypedObjectOffset:
      case MDefinition::Opcode::SetDisjointTypedElements:
      case MDefinition::Opcode::ArrayPopShift:
      case MDefinition::Opcode::ArrayPush:
      case MDefinition::Opcode::ArraySlice:
      case MDefinition::Opcode::LoadTypedArrayElementHole:
      case MDefinition::Opcode::StoreTypedArrayElementHole:
      case MDefinition::Opcode::LoadFixedSlot:
      case MDefinition::Opcode::LoadFixedSlotAndUnbox:
      case MDefinition::Opcode::StoreFixedSlot:
      case MDefinition::Opcode::GetPropertyPolymorphic:
      case MDefinition::Opcode::SetPropertyPolymorphic:
      case MDefinition::Opcode::GuardShape:
      case MDefinition::Opcode::GuardReceiverPolymorphic:
      case MDefinition::Opcode::GuardObjectGroup:
      case MDefinition::Opcode::GuardObjectIdentity:
      case MDefinition::Opcode::GuardUnboxedExpando:
      case MDefinition::Opcode::LoadUnboxedExpando:
      case MDefinition::Opcode::LoadSlot:
      case MDefinition::Opcode::StoreSlot:
      case MDefinition::Opcode::InArray:
      case MDefinition::Opcode::LoadElementHole:
      case MDefinition::Opcode::TypedArrayElements:
      case MDefinition::Opcode::TypedObjectElements:
      case MDefinition::Opcode::CopyLexicalEnvironmentObject:
      case MDefinition::Opcode::IsPackedArray:
        object = ins->getOperand(0);
        break;
      case MDefinition::Opcode::GetPropertyCache:
      case MDefinition::Opcode::GetDOMProperty:
      case MDefinition::Opcode::GetDOMMember:
      case MDefinition::Opcode::Call:
      case MDefinition::Opcode::Compare:
      case MDefinition::Opcode::GetArgumentsObjectArg:
      case MDefinition::Opcode::SetArgumentsObjectArg:
      case MDefinition::Opcode::GetFrameArgument:
      case MDefinition::Opcode::SetFrameArgument:
      case MDefinition::Opcode::CompareExchangeTypedArrayElement:
      case MDefinition::Opcode::AtomicExchangeTypedArrayElement:
      case MDefinition::Opcode::AtomicTypedArrayElementBinop:
      case MDefinition::Opcode::AsmJSLoadHeap:
      case MDefinition::Opcode::AsmJSStoreHeap:
      case MDefinition::Opcode::WasmLoadTls:
      case MDefinition::Opcode::WasmLoad:
      case MDefinition::Opcode::WasmStore:
      case MDefinition::Opcode::WasmCompareExchangeHeap:
      case MDefinition::Opcode::WasmAtomicBinopHeap:
      case MDefinition::Opcode::WasmAtomicExchangeHeap:
      case MDefinition::Opcode::WasmLoadGlobalVar:
      case MDefinition::Opcode::WasmStoreGlobalVar:
      case MDefinition::Opcode::ArrayJoin:
        return nullptr;
      default:
#ifdef DEBUG
        // Crash when the default aliasSet is overriden, but when not added in the list above.
        if (!ins->getAliasSet().isStore() || ins->getAliasSet().flags() != AliasSet::Flag::Any)
            MOZ_CRASH("Overridden getAliasSet without updating AliasAnalysisShared GetObject");
#endif

        return nullptr;
    }

    MOZ_ASSERT(!ins->getAliasSet().isStore() || ins->getAliasSet().flags() != AliasSet::Flag::Any);
    object = MaybeUnwrap(object);
    MOZ_ASSERT_IF(object, object->type() == MIRType::Object);
    return object;
}

// Generic comparing if a load aliases a store using TI information.
MDefinition::AliasType
AliasAnalysisShared::genericMightAlias(const MDefinition* load, const MDefinition* store)
{
    const MDefinition* loadObject = GetObject(load);
    const MDefinition* storeObject = GetObject(store);
    if (!loadObject || !storeObject)
        return MDefinition::AliasType::MayAlias;

    if (!loadObject->resultTypeSet() || !storeObject->resultTypeSet())
        return MDefinition::AliasType::MayAlias;

    if (loadObject->resultTypeSet()->objectsIntersect(storeObject->resultTypeSet()))
        return MDefinition::AliasType::MayAlias;

    return MDefinition::AliasType::NoAlias;
}


} // namespace jit
} // namespace js
