/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_RegExpStaticsObject_h
#define vm_RegExpStaticsObject_h

#include "vm/JSObject.h"

namespace js {

class RegExpStaticsObject : public NativeObject
{
  public:
    static const Class class_;

    size_t sizeOfData(mozilla::MallocSizeOf mallocSizeOf) {
        // XXX: should really call RegExpStatics::sizeOfIncludingThis() here
        // instead, but the extra memory it would measure is insignificant.
        return mallocSizeOf(getPrivate());
    }
};

} // namespace js

#endif /* vm_RegExpStaticsObject_h */
