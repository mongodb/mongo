/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_DestructuringFlavor_h
#define frontend_DestructuringFlavor_h

namespace js {
namespace frontend {

enum class DestructuringFlavor {
  // Destructuring into a declaration.
  Declaration,

  // Destructuring as part of an AssignmentExpression.
  Assignment
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_DestructuringFlavor_h */
