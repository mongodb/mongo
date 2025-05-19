/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/BytecodeSection.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "frontend/AbstractScopePtr.h"    // ScopeIndex
#include "frontend/CompilationStencil.h"  // CompilationStencil
#include "frontend/FrontendContext.h"     // FrontendContext
#include "frontend/SharedContext.h"       // FunctionBox
#include "js/ColumnNumber.h"              // JS::LimitedColumnNumberOneOrigin
#include "vm/BytecodeUtil.h"              // INDEX_LIMIT, StackUses, StackDefs
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"     // JSContext
#include "vm/RegExpObject.h"  // RegexpObject
#include "vm/Scope.h"         // GlobalScope

using namespace js;
using namespace js::frontend;

bool GCThingList::append(FunctionBox* funbox, GCThingIndex* index) {
  // Append the function to the vector and return the index in *index.
  *index = GCThingIndex(vector.length());

  if (!vector.emplaceBack(funbox->index())) {
    return false;
  }
  return true;
}

AbstractScopePtr GCThingList::getScope(size_t index) const {
  const TaggedScriptThingIndex& elem = vector[index];
  if (elem.isEmptyGlobalScope()) {
    // The empty enclosing scope should be stored by
    // CompilationInput::initForSelfHostingGlobal.
    return AbstractScopePtr::compilationEnclosingScope(compilationState);
  }
  return AbstractScopePtr(compilationState, elem.toScope());
}

mozilla::Maybe<ScopeIndex> GCThingList::getScopeIndex(size_t index) const {
  const TaggedScriptThingIndex& elem = vector[index];
  if (elem.isEmptyGlobalScope()) {
    return mozilla::Nothing();
  }
  return mozilla::Some(vector[index].toScope());
}

TaggedParserAtomIndex GCThingList::getAtom(size_t index) const {
  const TaggedScriptThingIndex& elem = vector[index];
  return elem.toAtom();
}

bool js::frontend::EmitScriptThingsVector(
    JSContext* cx, const CompilationAtomCache& atomCache,
    const CompilationStencil& stencil, CompilationGCOutput& gcOutput,
    mozilla::Span<const TaggedScriptThingIndex> things,
    mozilla::Span<JS::GCCellPtr> output) {
  MOZ_ASSERT(things.size() <= INDEX_LIMIT);
  MOZ_ASSERT(things.size() == output.size());

  for (uint32_t i = 0; i < things.size(); i++) {
    const auto& thing = things[i];
    switch (thing.tag()) {
      case TaggedScriptThingIndex::Kind::ParserAtomIndex:
      case TaggedScriptThingIndex::Kind::WellKnown: {
        JSString* str = atomCache.getExistingStringAt(cx, thing.toAtom());
        MOZ_ASSERT(str);
        output[i] = JS::GCCellPtr(str);
        break;
      }
      case TaggedScriptThingIndex::Kind::Null:
        output[i] = JS::GCCellPtr(nullptr);
        break;
      case TaggedScriptThingIndex::Kind::BigInt: {
        const BigIntStencil& data = stencil.bigIntData[thing.toBigInt()];
        BigInt* bi = data.createBigInt(cx);
        if (!bi) {
          return false;
        }
        output[i] = JS::GCCellPtr(bi);
        break;
      }
      case TaggedScriptThingIndex::Kind::ObjLiteral: {
        const ObjLiteralStencil& data =
            stencil.objLiteralData[thing.toObjLiteral()];
        JS::GCCellPtr ptr = data.create(cx, atomCache);
        if (!ptr) {
          return false;
        }
        output[i] = ptr;
        break;
      }
      case TaggedScriptThingIndex::Kind::RegExp: {
        RegExpStencil& data = stencil.regExpData[thing.toRegExp()];
        RegExpObject* regexp = data.createRegExp(cx, atomCache);
        if (!regexp) {
          return false;
        }
        output[i] = JS::GCCellPtr(regexp);
        break;
      }
      case TaggedScriptThingIndex::Kind::Scope:
        output[i] = JS::GCCellPtr(gcOutput.getScope(thing.toScope()));
        break;
      case TaggedScriptThingIndex::Kind::Function:
        output[i] = JS::GCCellPtr(gcOutput.getFunction(thing.toFunction()));
        break;
      case TaggedScriptThingIndex::Kind::EmptyGlobalScope: {
        Scope* scope = &cx->global()->emptyGlobalScope();
        output[i] = JS::GCCellPtr(scope);
        break;
      }
    }
  }

  return true;
}

bool CGTryNoteList::append(TryNoteKind kind, uint32_t stackDepth,
                           BytecodeOffset start, BytecodeOffset end) {
  MOZ_ASSERT(start <= end);

  // Offsets are given relative to sections, but we only expect main-section
  // to have TryNotes. In finish() we will fixup base offset.

  TryNote note(uint32_t(kind), stackDepth, start.toUint32(),
               (end - start).toUint32());

  return list.append(note);
}

bool CGScopeNoteList::append(GCThingIndex scopeIndex, BytecodeOffset offset,
                             uint32_t parent) {
  ScopeNote note;
  note.index = scopeIndex;
  note.start = offset.toUint32();
  note.length = 0;
  note.parent = parent;

  return list.append(note);
}

void CGScopeNoteList::recordEnd(uint32_t index, BytecodeOffset offset) {
  recordEndImpl(index, offset.toUint32());
}

void CGScopeNoteList::recordEndFunctionBodyVar(uint32_t index) {
  recordEndImpl(index, UINT32_MAX);
}

void CGScopeNoteList::recordEndImpl(uint32_t index, uint32_t offset) {
  MOZ_ASSERT(index < length());
  MOZ_ASSERT(list[index].length == 0);
  MOZ_ASSERT(offset >= list[index].start);
  list[index].length = offset - list[index].start;
}

BytecodeSection::BytecodeSection(FrontendContext* fc, uint32_t lineNum,
                                 JS::LimitedColumnNumberOneOrigin column)
    : code_(fc),
      notes_(fc),
      lastNoteOffset_(0),
      tryNoteList_(fc),
      scopeNoteList_(fc),
      resumeOffsetList_(fc),
      currentLine_(lineNum),
      lastColumn_(column) {}

void BytecodeSection::updateDepth(JSOp op, BytecodeOffset target) {
  jsbytecode* pc = code(target);

  int nuses = StackUses(op, pc);
  int ndefs = StackDefs(op);

  stackDepth_ -= nuses;
  MOZ_ASSERT(stackDepth_ >= 0);
  stackDepth_ += ndefs;

  if (uint32_t(stackDepth_) > maxStackDepth_) {
    maxStackDepth_ = stackDepth_;
  }
}

PerScriptData::PerScriptData(FrontendContext* fc,
                             frontend::CompilationState& compilationState)
    : gcThingList_(fc, compilationState),
      atomIndices_(fc->nameCollectionPool()) {}

bool PerScriptData::init(FrontendContext* fc) {
  return atomIndices_.acquire(fc);
}
