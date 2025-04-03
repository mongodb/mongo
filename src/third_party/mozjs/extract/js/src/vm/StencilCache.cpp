/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/StencilCache.h"

#include "frontend/CompilationStencil.h"
#include "js/experimental/JSStencil.h"
#include "vm/MutexIDs.h"

js::StencilCache::StencilCache()
    : cache(js::mutexid::StencilCache), enabled(false) {}

js::StencilCache::AccessKey js::StencilCache::isSourceCached(
    ScriptSource* src) {
  if (!enabled) {
    return cache.noAccess();
  }

  AccessKey lock(cache.lock());
  if (!enabled) {
    // As we checked the flag before taking the lock, we have to check again to
    // avoid races on the cache manipulation.
    return cache.noAccess();
  }
  if (!lock->watched.has(src)) {
    // If the source does not have any cached function, and we do not expect to
    // cache any delazification in the future, then skip any cache handling.
    return cache.noAccess();
  }
  return lock;
}

bool js::StencilCache::startCaching(RefPtr<ScriptSource>&& src) {
  auto guard = cache.lock();
  if (!guard->watched.putNew(std::move(src))) {
    return false;
  }
  enabled = true;
  return true;
}

js::frontend::CompilationStencil* js::StencilCache::lookup(
    AccessKey& guard, const StencilContext& key) {
  auto ptr = guard->functions.lookup(key);
  if (!ptr) {
    return nullptr;
  }

  return ptr->value().get();
}

bool js::StencilCache::putNew(AccessKey& guard, const StencilContext& key,
                              js::frontend::CompilationStencil* value) {
  return guard->functions.putNew(key, value);
}

// Important: This function should not be called within a scope checking for
// isSourceCached, as this would cause a dead-lock.
void js::StencilCache::clearAndDisable() {
  auto guard = cache.lock();
  guard->functions.clearAndCompact();
  guard->watched.clearAndCompact();
  enabled = false;
}
