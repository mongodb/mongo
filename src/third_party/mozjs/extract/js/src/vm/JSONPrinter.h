/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSONPrinter_h
#define vm_JSONPrinter_h

#include "mozilla/TimeStamp.h"

#include <stdio.h>

#include "js/TypeDecls.h"
#include "vm/Printer.h"

namespace js {

class JSONPrinter {
 protected:
  int indentLevel_;
  bool indent_;
  bool first_;
  GenericPrinter& out_;

  void indent();

 public:
  explicit JSONPrinter(GenericPrinter& out, bool indent = true)
      : indentLevel_(0), indent_(indent), first_(true), out_(out) {}

  void setIndentLevel(int indentLevel) { indentLevel_ = indentLevel; }

  void beginObject();
  void beginList();
  void beginObjectProperty(const char* name);
  void beginListProperty(const char* name);

  void value(const char* format, ...) MOZ_FORMAT_PRINTF(2, 3);
  void value(int value);

  void boolProperty(const char* name, bool value);

  void property(const char* name, const char* value);
  void property(const char* name, int32_t value);
  void property(const char* name, uint32_t value);
  void property(const char* name, int64_t value);
  void property(const char* name, uint64_t value);
#if defined(XP_DARWIN) || defined(__OpenBSD__) || defined(__wasi__)
  // On OSX and OpenBSD, size_t is long unsigned, uint32_t is unsigned, and
  // uint64_t is long long unsigned. Everywhere else, size_t matches either
  // uint32_t or uint64_t.
  void property(const char* name, size_t value);
#endif

  void formatProperty(const char* name, const char* format, ...)
      MOZ_FORMAT_PRINTF(3, 4);
  void formatProperty(const char* name, const char* format, va_list ap);

  // JSON requires decimals to be separated by periods, but the LC_NUMERIC
  // setting may cause printf to use commas in some locales.
  enum TimePrecision { SECONDS, MILLISECONDS, MICROSECONDS };
  void property(const char* name, const mozilla::TimeDuration& dur,
                TimePrecision precision);

  void floatProperty(const char* name, double value, size_t precision);

  GenericPrinter& beginStringProperty(const char* name);
  void endStringProperty();

  GenericPrinter& beginString();
  void endString();

  void nullProperty(const char* name);
  void nullValue();

  void endObject();
  void endList();

  // Notify the output that the caller has detected OOM and should transition
  // to its saw-OOM state.
  void outOfMemory() { out_.reportOutOfMemory(); }

 protected:
  void propertyName(const char* name);
};

}  // namespace js

#endif /* vm_JSONPrinter_h */
