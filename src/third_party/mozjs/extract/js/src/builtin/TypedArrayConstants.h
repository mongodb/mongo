/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Specialized .h file to be used by both JS and C++ code.

#ifndef builtin_TypedArrayConstants_h
#define builtin_TypedArrayConstants_h

///////////////////////////////////////////////////////////////////////////
// Slots for objects using the typed array layout

#define JS_TYPEDARRAYLAYOUT_BUFFER_SLOT 0

///////////////////////////////////////////////////////////////////////////
// Slots and flags for ArrayBuffer objects

#define JS_ARRAYBUFFER_FLAGS_SLOT 3

#define JS_ARRAYBUFFER_DETACHED_FLAG 0x8

#endif
