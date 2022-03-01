/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BCEParserHandle_h
#define frontend_BCEParserHandle_h

#include "frontend/ErrorReporter.h"
#include "frontend/FullParseHandler.h"
#include "frontend/Parser.h"
#include "frontend/ParserAtom.h"

namespace js {
namespace frontend {

struct BCEParserHandle {
  virtual ErrorReporter& errorReporter() = 0;
  virtual const ErrorReporter& errorReporter() const = 0;

  virtual const JS::ReadOnlyCompileOptions& options() const = 0;

  virtual FullParseHandler& astGenerator() = 0;
};

}  // namespace frontend
}  // namespace js

#endif  // frontend_BCEParserHandle_h
