/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ScriptIndex_h
#define frontend_ScriptIndex_h

#include "frontend/TypedIndex.h"  // TypedIndex

namespace js {
namespace frontend {

class ScriptStencil;

using ScriptIndex = TypedIndex<ScriptStencil>;

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_ScriptIndex_h */
