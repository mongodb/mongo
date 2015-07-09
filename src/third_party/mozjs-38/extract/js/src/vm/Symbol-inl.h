/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Symbol_inl_h
#define vm_Symbol_inl_h

#include "vm/Symbol.h"

#include "gc/Marking.h"

#include "js/RootingAPI.h"

#include "jsgcinlines.h"

inline void
JS::Symbol::markChildren(JSTracer* trc)
{
    if (description_)
        MarkStringUnbarriered(trc, &description_, "description");
}

#endif /* vm_Symbol_inl_h */
