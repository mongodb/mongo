/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_TDZCheckCache_h
#define frontend_TDZCheckCache_h

#include "mozilla/Maybe.h"

#include "ds/Nestable.h"
#include "frontend/NameCollections.h"

namespace js {
namespace frontend {

struct BytecodeEmitter;
class TaggedParserAtomIndex;

enum MaybeCheckTDZ { CheckTDZ = true, DontCheckTDZ = false };

using CheckTDZMap = RecyclableNameMap<MaybeCheckTDZ>;

// A cache that tracks Temporal Dead Zone (TDZ) checks, so that any use of a
// lexical variable that's dominated by an earlier use, or by evaluation of its
// declaration (which will initialize it, perhaps to |undefined|), doesn't have
// to redundantly check that the lexical variable has been initialized
//
// Each basic block should have a TDZCheckCache in scope. Some NestableControl
// subclasses contain a TDZCheckCache.
//
// When a scope containing lexical variables is entered, all such variables are
// marked as CheckTDZ.  When a lexical variable is accessed, its entry is
// checked.  If it's CheckTDZ, a JSOp::CheckLexical is emitted and then the
// entry is marked DontCheckTDZ.  If it's DontCheckTDZ, no check is emitted
// because a prior check would have already failed.  Finally, because
// evaluating a lexical variable declaration initializes it (after any
// initializer is evaluated), evaluating a lexical declaration marks its entry
// as DontCheckTDZ.
class TDZCheckCache : public Nestable<TDZCheckCache> {
  PooledMapPtr<CheckTDZMap> cache_;

  [[nodiscard]] bool ensureCache(BytecodeEmitter* bce);

 public:
  explicit TDZCheckCache(BytecodeEmitter* bce);

  mozilla::Maybe<MaybeCheckTDZ> needsTDZCheck(BytecodeEmitter* bce,
                                              TaggedParserAtomIndex name);
  [[nodiscard]] bool noteTDZCheck(BytecodeEmitter* bce,
                                  TaggedParserAtomIndex name,
                                  MaybeCheckTDZ check);
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_TDZCheckCache_h */
