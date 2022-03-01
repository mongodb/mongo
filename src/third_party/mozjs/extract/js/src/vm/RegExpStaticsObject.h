/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_RegExpStaticsObject_h
#define vm_RegExpStaticsObject_h

#include "vm/JSObject.h"

namespace js {

class RegExpStaticsObject : public NativeObject {
 public:
  static const JSClass class_;

  size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf) {
    // XXX: should really call RegExpStatics::sizeOfIncludingThis() here
    // instead, but the extra memory it would measure is insignificant.
    return mallocSizeOf(getPrivate());
  }
};

}  // namespace js

#endif /* vm_RegExpStaticsObject_h */
