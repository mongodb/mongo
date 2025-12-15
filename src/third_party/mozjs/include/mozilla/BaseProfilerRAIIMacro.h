/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BaseProfilerRAIIMacro_h
#define BaseProfilerRAIIMacro_h

// Macros used by the AUTO_PROFILER_* macros to generate unique variable names.
#define PROFILER_RAII_PASTE(id, line) id##line
#define PROFILER_RAII_EXPAND(id, line) PROFILER_RAII_PASTE(id, line)
#define PROFILER_RAII PROFILER_RAII_EXPAND(raiiObject, __LINE__)

#endif  // BaseProfilerRAIIMacro_h
