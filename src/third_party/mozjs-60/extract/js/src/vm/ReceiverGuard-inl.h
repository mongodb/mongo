/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_ReceiverGuard_inl_h
#define vm_ReceiverGuard_inl_h

#include "vm/ReceiverGuard.h"

#include "builtin/TypedObject.h"
#include "vm/JSObject.h"
#include "vm/ShapedObject.h"
#include "vm/UnboxedObject.h"

namespace js {

MOZ_ALWAYS_INLINE
ReceiverGuard::ReceiverGuard(JSObject* obj)
  : group(nullptr), shape(nullptr)
{
    if (!obj->isNative()) {
        if (obj->is<UnboxedPlainObject>()) {
            group = obj->group();
            if (UnboxedExpandoObject* expando = obj->as<UnboxedPlainObject>().maybeExpando())
                shape = expando->lastProperty();
            return;
        }
        if (obj->is<TypedObject>()) {
            group = obj->group();
            return;
        }
    }
    shape = obj->as<ShapedObject>().shape();
}

MOZ_ALWAYS_INLINE
ReceiverGuard::ReceiverGuard(ObjectGroup* group, Shape* shape)
  : group(group), shape(shape)
{
    if (group) {
        const Class* clasp = group->clasp();
        if (clasp == &UnboxedPlainObject::class_) {
            // Keep both group and shape.
        } else if (IsTypedObjectClass(clasp)) {
            this->shape = nullptr;
        } else {
            this->group = nullptr;
        }
    }
}

} // namespace js

#endif /* vm_ReceiverGuard_inl_h */
