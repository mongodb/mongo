/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FunctionSyntaxKind_h
#define frontend_FunctionSyntaxKind_h

#include <stdint.h>  // uint8_t

namespace js {
namespace frontend {

enum class FunctionSyntaxKind : uint8_t {
  // A non-arrow function expression.
  Expression,

  // A named function appearing as a Statement.
  Statement,

  Arrow,

  // Method of a class or object. Field initializers also desugar to methods.
  Method,
  FieldInitializer,

  // Mostly static class blocks act similar to field initializers, however,
  // there is some difference in static semantics.
  StaticClassBlock,

  ClassConstructor,
  DerivedClassConstructor,
  Getter,
  Setter,
};

} /* namespace frontend */
} /* namespace js */

#endif /* frontend_FunctionSyntaxKind_h */
