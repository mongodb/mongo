/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/LabelEmitter.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "frontend/BytecodeEmitter.h"  // BytecodeEmitter

using namespace js;
using namespace js::frontend;

void LabelEmitter::emitLabel(TaggedParserAtomIndex name) {
  MOZ_ASSERT(state_ == State::Start);

  controlInfo_.emplace(bce_, name, bce_->bytecodeSection().offset());

#ifdef DEBUG
  state_ = State::Label;
#endif
}

bool LabelEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Label);

  // Patch the break/continue to this label.
  if (!controlInfo_->patchBreaks(bce_)) {
    return false;
  }

  controlInfo_.reset();

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
