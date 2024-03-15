/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/TDZCheckCache.h"

#include "frontend/BytecodeEmitter.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

TDZCheckCache::TDZCheckCache(BytecodeEmitter* bce)
    : Nestable<TDZCheckCache>(&bce->innermostTDZCheckCache),
      cache_(bce->fc->nameCollectionPool()) {}

bool TDZCheckCache::ensureCache(BytecodeEmitter* bce) {
  return cache_ || cache_.acquire(bce->fc);
}

Maybe<MaybeCheckTDZ> TDZCheckCache::needsTDZCheck(BytecodeEmitter* bce,
                                                  TaggedParserAtomIndex name) {
  if (!ensureCache(bce)) {
    return Nothing();
  }

  CheckTDZMap::AddPtr p = cache_->lookupForAdd(name);
  if (p) {
    return Some(p->value().wrapped);
  }

  MaybeCheckTDZ rv = CheckTDZ;
  for (TDZCheckCache* it = enclosing(); it; it = it->enclosing()) {
    if (it->cache_) {
      if (CheckTDZMap::Ptr p2 = it->cache_->lookup(name)) {
        rv = p2->value();
        break;
      }
    }
  }

  if (!cache_->add(p, name, rv)) {
    ReportOutOfMemory(bce->fc);
    return Nothing();
  }

  return Some(rv);
}

bool TDZCheckCache::noteTDZCheck(BytecodeEmitter* bce,
                                 TaggedParserAtomIndex name,
                                 MaybeCheckTDZ check) {
  if (!ensureCache(bce)) {
    return false;
  }

  CheckTDZMap::AddPtr p = cache_->lookupForAdd(name);
  if (p) {
    MOZ_ASSERT(
        !check,
        "TDZ only needs to be checked once per binding per basic block.");
    p->value() = check;
  } else {
    if (!cache_->add(p, name, check)) {
      ReportOutOfMemory(bce->fc);
      return false;
    }
  }

  return true;
}
