/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_EmitterScope_h
#define frontend_EmitterScope_h

#include "mozilla/Maybe.h"

#include <stdint.h>

#include "ds/Nestable.h"
#include "frontend/AbstractScopePtr.h"
#include "frontend/NameAnalysisTypes.h"
#include "frontend/NameCollections.h"
#include "frontend/ParseContext.h"
#include "frontend/ParserAtom.h"  // TaggedParserAtomIndex
#include "frontend/SharedContext.h"
#include "js/TypeDecls.h"
#include "vm/BytecodeUtil.h"   // JSOp
#include "vm/SharedStencil.h"  // GCThingIndex

namespace js {

class Scope;

namespace frontend {

struct BytecodeEmitter;

// A scope that introduces bindings.
class EmitterScope : public Nestable<EmitterScope> {
  // The cache of bound names that may be looked up in the
  // scope. Initially populated as the set of names this scope binds. As
  // names are looked up in enclosing scopes, they are cached on the
  // current scope.
  PooledMapPtr<NameLocationMap> nameCache_;

  // If this scope's cache does not include free names, such as the
  // global scope, the NameLocation to return.
  mozilla::Maybe<NameLocation> fallbackFreeNameLocation_;

  // True if there is a corresponding EnvironmentObject on the environment
  // chain, false if all bindings are stored in frame slots on the stack.
  bool hasEnvironment_;

  // The number of enclosing environments. Used for error checking.
  uint8_t environmentChainLength_;

  // The next usable slot on the frame for not-closed over bindings.
  //
  // The initial frame slot when assigning slots to bindings is the
  // enclosing scope's nextFrameSlot. For the first scope in a frame,
  // the initial frame slot is 0.
  uint32_t nextFrameSlot_;

  // The index in the BytecodeEmitter's interned scope vector, otherwise
  // ScopeNote::NoScopeIndex.
  GCThingIndex scopeIndex_;

  // If kind is Lexical, Catch, or With, the index in the BytecodeEmitter's
  // block scope note list. Otherwise ScopeNote::NoScopeNote.
  uint32_t noteIndex_;

  [[nodiscard]] bool ensureCache(BytecodeEmitter* bce);

  [[nodiscard]] bool checkSlotLimits(BytecodeEmitter* bce,
                                     const ParserBindingIter& bi);

  [[nodiscard]] bool checkEnvironmentChainLength(BytecodeEmitter* bce);

  void updateFrameFixedSlots(BytecodeEmitter* bce, const ParserBindingIter& bi);

  [[nodiscard]] bool putNameInCache(BytecodeEmitter* bce,
                                    TaggedParserAtomIndex name,
                                    NameLocation loc);

  mozilla::Maybe<NameLocation> lookupInCache(BytecodeEmitter* bce,
                                             TaggedParserAtomIndex name);

  EmitterScope* enclosing(BytecodeEmitter** bce) const;

  mozilla::Maybe<ScopeIndex> enclosingScopeIndex(BytecodeEmitter* bce) const;

  static bool nameCanBeFree(BytecodeEmitter* bce, TaggedParserAtomIndex name);

  NameLocation searchAndCache(BytecodeEmitter* bce, TaggedParserAtomIndex name);

  [[nodiscard]] bool internEmptyGlobalScopeAsBody(BytecodeEmitter* bce);

  [[nodiscard]] bool internScopeStencil(BytecodeEmitter* bce, ScopeIndex index);

  [[nodiscard]] bool internBodyScopeStencil(BytecodeEmitter* bce,
                                            ScopeIndex index);
  [[nodiscard]] bool appendScopeNote(BytecodeEmitter* bce);

  [[nodiscard]] bool clearFrameSlotRange(BytecodeEmitter* bce, JSOp opcode,
                                         uint32_t slotStart,
                                         uint32_t slotEnd) const;

  [[nodiscard]] bool deadZoneFrameSlotRange(BytecodeEmitter* bce,
                                            uint32_t slotStart,
                                            uint32_t slotEnd) const {
    return clearFrameSlotRange(bce, JSOp::Uninitialized, slotStart, slotEnd);
  }

 public:
  explicit EmitterScope(BytecodeEmitter* bce);

  void dump(BytecodeEmitter* bce);

  [[nodiscard]] bool enterLexical(BytecodeEmitter* bce, ScopeKind kind,
                                  LexicalScope::ParserData* bindings);
  [[nodiscard]] bool enterClassBody(BytecodeEmitter* bce, ScopeKind kind,
                                    ClassBodyScope::ParserData* bindings);
  [[nodiscard]] bool enterNamedLambda(BytecodeEmitter* bce,
                                      FunctionBox* funbox);
  [[nodiscard]] bool enterFunction(BytecodeEmitter* bce, FunctionBox* funbox);
  [[nodiscard]] bool enterFunctionExtraBodyVar(BytecodeEmitter* bce,
                                               FunctionBox* funbox);
  [[nodiscard]] bool enterGlobal(BytecodeEmitter* bce,
                                 GlobalSharedContext* globalsc);
  [[nodiscard]] bool enterEval(BytecodeEmitter* bce, EvalSharedContext* evalsc);
  [[nodiscard]] bool enterModule(BytecodeEmitter* module,
                                 ModuleSharedContext* modulesc);
  [[nodiscard]] bool enterWith(BytecodeEmitter* bce);
  [[nodiscard]] bool deadZoneFrameSlots(BytecodeEmitter* bce) const;

  [[nodiscard]] bool leave(BytecodeEmitter* bce, bool nonLocal = false);

  GCThingIndex index() const {
    MOZ_ASSERT(scopeIndex_ != ScopeNote::NoScopeIndex,
               "Did you forget to intern a Scope?");
    return scopeIndex_;
  }

  uint32_t noteIndex() const { return noteIndex_; }

  AbstractScopePtr scope(const BytecodeEmitter* bce) const;
  mozilla::Maybe<ScopeIndex> scopeIndex(const BytecodeEmitter* bce) const;

  bool hasEnvironment() const { return hasEnvironment_; }

  // The first frame slot used.
  uint32_t frameSlotStart() const {
    if (EmitterScope* inFrame = enclosingInFrame()) {
      return inFrame->nextFrameSlot_;
    }
    return 0;
  }

  // The last frame slot used + 1.
  uint32_t frameSlotEnd() const { return nextFrameSlot_; }

  EmitterScope* enclosingInFrame() const {
    return Nestable<EmitterScope>::enclosing();
  }

  NameLocation lookup(BytecodeEmitter* bce, TaggedParserAtomIndex name);

  // Find both the slot associated with a private name and the location of the
  // corresponding `.privateBrand` binding.
  //
  // Simply doing two separate lookups, one for `name` and another for
  // `.privateBrand`, would give the wrong answer in this case:
  //
  //     class Outer {
  //       #outerMethod() { reutrn "ok"; }
  //
  //       test() {
  //         class Inner {
  //           #innerMethod() {}
  //           test(outer) {
  //             return outer.#outerMethod();
  //           }
  //         }
  //         return new Inner().test(this);
  //       }
  //     }
  //
  //    new Outer().test();  // should return "ok"
  //
  // At the point in Inner.test where `#outerMethod` is called, we need to
  // check for the private brand of `Outer`, not `Inner`; but both class bodies
  // have `.privateBrand` bindings. In a normal `lookup`, the inner binding
  // would shadow the outer one.
  //
  // This method instead sets `brandLoc` to the location of the `.privateBrand`
  // binding in the same class body as the private name `name`, ignoring
  // shadowing. If `name` refers to a name that is actually stamped onto the
  // target object (anything other than a non-static private method), then
  // `brandLoc` is set to Nothing.
  //
  // To handle cases where it's not possible to find the private brand, this
  // method has to be fallible.
  bool lookupPrivate(BytecodeEmitter* bce, TaggedParserAtomIndex name,
                     NameLocation& loc, mozilla::Maybe<NameLocation>& brandLoc);

  mozilla::Maybe<NameLocation> locationBoundInScope(TaggedParserAtomIndex name,
                                                    EmitterScope* target);

  // For a given emitter scope, return the number of enclosing environments in
  // the current compilation (this excludes environments that could enclose the
  // compilation, like would happen for an eval copmilation).
  static uint32_t CountEnclosingCompilationEnvironments(
      BytecodeEmitter* bce, EmitterScope* emitterScope);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_EmitterScope_h */
