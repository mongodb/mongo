/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Warnings_h
#define vm_Warnings_h

struct JSContext;

namespace js {

// Internal API mirroring the public JS_ReportErrorNumber API.
// We currently re-use the same errorNumbers.

bool WarnNumberASCII(JSContext* cx, const unsigned errorNumber, ...);

bool WarnNumberLatin1(JSContext* cx, const unsigned errorNumber, ...);

bool WarnNumberUTF8(JSContext* cx, const unsigned errorNumber, ...);

bool WarnNumberUC(JSContext* cx, const unsigned errorNumber, ...);

}  // namespace js

#endif /* vm_Warnings_h */
