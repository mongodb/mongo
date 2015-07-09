/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsbool_h
#define jsbool_h

/*
 * JS boolean interface.
 */

#include "NamespaceImports.h"

extern JSObject*
js_InitBooleanClass(JSContext* cx, js::HandleObject obj);

extern JSString*
js_BooleanToString(js::ExclusiveContext* cx, bool b);

#endif /* jsbool_h */
