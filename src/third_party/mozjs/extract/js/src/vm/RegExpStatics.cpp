/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/RegExpStatics.h"

#include "gc/Zone.h"
#include "vm/RegExpShared.h"

using namespace js;

// static
UniquePtr<RegExpStatics> RegExpStatics::create(JSContext* cx) {
  return cx->make_unique<RegExpStatics>();
}

bool RegExpStatics::executeLazy(JSContext* cx) {
  if (!pendingLazyEvaluation) {
    return true;
  }

  MOZ_ASSERT(lazySource);
  MOZ_ASSERT(matchesInput);
  MOZ_ASSERT(lazyIndex != size_t(-1));

  /* Retrieve or create the RegExpShared in this zone. */
  Rooted<JSAtom*> source(cx, lazySource);
  RootedRegExpShared shared(cx,
                            cx->zone()->regExps().get(cx, source, lazyFlags));
  if (!shared) {
    return false;
  }

  /*
   * It is not necessary to call aboutToWrite(): evaluation of
   * implicit copies is safe.
   */

  /* Execute the full regular expression. */
  Rooted<JSLinearString*> input(cx, matchesInput);
  RegExpRunStatus status =
      RegExpShared::execute(cx, &shared, input, lazyIndex, &this->matches);
  if (status == RegExpRunStatus_Error) {
    return false;
  }

  /*
   * RegExpStatics are only updated on successful (matching) execution.
   * Re-running the same expression must therefore produce a matching result.
   */
  MOZ_ASSERT(status == RegExpRunStatus_Success);

  /* Unset lazy state and remove rooted values that now have no use. */
  pendingLazyEvaluation = false;
  lazySource = nullptr;
  lazyIndex = size_t(-1);

  return true;
}
