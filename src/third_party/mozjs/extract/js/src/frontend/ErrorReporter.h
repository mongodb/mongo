/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ErrorReporter_h
#define frontend_ErrorReporter_h

#include <stdarg.h> // for va_list
#include <stddef.h> // for size_t
#include <stdint.h> // for uint32_t

#include "jsapi.h" // for JS::ReadOnlyCompileOptions

namespace js {
namespace frontend {

class ErrorReporter
{
  public:
    virtual const JS::ReadOnlyCompileOptions& options() const = 0;

    virtual void lineAndColumnAt(size_t offset, uint32_t* line, uint32_t* column) const = 0;
    virtual void currentLineAndColumn(uint32_t* line, uint32_t* column) const = 0;

    virtual bool hasTokenizationStarted() const = 0;
    virtual void reportErrorNoOffsetVA(unsigned errorNumber, va_list args) = 0;
    virtual const char* getFilename() const = 0;

    void reportErrorNoOffset(unsigned errorNumber, ...) {
        va_list args;
        va_start(args, errorNumber);

        reportErrorNoOffsetVA(errorNumber, args);

        va_end(args);
    }
};

} // namespace frontend
} // namespace js

#endif // frontend_ErrorReporter_h
