/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/BytecodeLocation-inl.h"

#include "vm/JSScript.h"

using namespace js;

#ifdef DEBUG
bool BytecodeLocation::isValid(const JSScript* script) const {
  // Note: Don't create a new BytecodeLocation during the implementation of
  // this, as it is used in the constructor, and will recurse forever.
  return script->contains(*this) || toRawBytecode() == script->codeEnd();
}

bool BytecodeLocation::isInBounds(const JSScript* script) const {
  return script->contains(*this);
}

const JSScript* BytecodeLocation::getDebugOnlyScript() const {
  return this->debugOnlyScript_;
}

#endif  // DEBUG
