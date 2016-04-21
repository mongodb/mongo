/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_NameFunctions_h
#define frontend_NameFunctions_h

#include "js/TypeDecls.h"

namespace js {

class ExclusiveContext;

namespace frontend {

class ParseNode;

bool
NameFunctions(ExclusiveContext* cx, ParseNode* pn);

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_NameFunctions_h */
