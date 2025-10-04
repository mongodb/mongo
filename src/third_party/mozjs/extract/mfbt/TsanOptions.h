/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Default options for ThreadSanitizer. */

#ifndef mozilla_TsanOptions_h
#define mozilla_TsanOptions_h

#include "mozilla/Compiler.h"

#ifndef _MSC_VER  // Not supported by clang-cl yet

//
// When running with ThreadSanitizer, we need to explicitly set some
// options specific to our codebase to prevent errors during runtime.
// To override these, set the TSAN_OPTIONS environment variable.
//
// Currently, these are:
//
//   abort_on_error=1 - Causes TSan to abort instead of using exit().
//   halt_on_error=1 - Causes TSan to stop on the first race detected.
//
//   report_signal_unsafe=0 - Required to avoid TSan deadlocks when
//   receiving external signals (e.g. SIGINT manually on console).
//
//   allocator_may_return_null=1 - Tell TSan to return NULL when an allocation
//   fails instead of aborting the program. This allows us to handle failing
//   allocations the same way we would handle them with a regular allocator and
//   also uncovers potential bugs that might occur in these situations.
//
extern "C" const char* __tsan_default_options() {
  return "halt_on_error=1:abort_on_error=1:report_signal_unsafe=0"
         ":allocator_may_return_null=1";
}

// These are default suppressions for external libraries that probably
// every application would want to include if it potentially loads external
// libraries like GTK/X and hence their dependencies.
#  define MOZ_TSAN_DEFAULT_EXTLIB_SUPPRESSIONS \
    "called_from_lib:libappmenu-gtk3-parser\n" \
    "called_from_lib:libatk-1\n"               \
    "called_from_lib:libcairo.so\n"            \
    "called_from_lib:libcairo-gobject\n"       \
    "called_from_lib:libdconfsettings\n"       \
    "called_from_lib:libEGL_nvidia\n"          \
    "called_from_lib:libfontconfig.so\n"       \
    "called_from_lib:libfontconfig1\n"         \
    "called_from_lib:libgdk-3\n"               \
    "called_from_lib:libgdk_pixbuf\n"          \
    "called_from_lib:libgdk-x11\n"             \
    "called_from_lib:libgio-2\n"               \
    "called_from_lib:libglib-1\n"              \
    "called_from_lib:libglib-2\n"              \
    "called_from_lib:libgobject\n"             \
    "called_from_lib:libgtk-3\n"               \
    "called_from_lib:libgtk-x11\n"             \
    "called_from_lib:libgvfscommon\n"          \
    "called_from_lib:libgvfsdbus\n"            \
    "called_from_lib:libibus-1\n"              \
    "called_from_lib:libnvidia-eglcore\n"      \
    "called_from_lib:libnvidia-glsi\n"         \
    "called_from_lib:libogg.so\n"              \
    "called_from_lib:libpango-1\n"             \
    "called_from_lib:libpangocairo\n"          \
    "called_from_lib:libpangoft2\n"            \
    "called_from_lib:pango-basic-fc\n"         \
    "called_from_lib:libpixman-1\n"            \
    "called_from_lib:libpulse.so\n"            \
    "called_from_lib:libpulsecommon\n"         \
    "called_from_lib:libsecret-1\n"            \
    "called_from_lib:libunity-gtk3-parser\n"   \
    "called_from_lib:libvorbis.so\n"           \
    "called_from_lib:libvorbisfile\n"          \
    "called_from_lib:libwayland-client\n"      \
    "called_from_lib:libX11.so\n"              \
    "called_from_lib:libX11-xcb\n"             \
    "called_from_lib:libXau\n"                 \
    "called_from_lib:libxcb.so\n"              \
    "called_from_lib:libXcomposite\n"          \
    "called_from_lib:libXcursor\n"             \
    "called_from_lib:libXdamage\n"             \
    "called_from_lib:libXdmcp\n"               \
    "called_from_lib:libXext\n"                \
    "called_from_lib:libXfixes\n"              \
    "called_from_lib:libXi.so\n"               \
    "called_from_lib:libXrandr\n"              \
    "called_from_lib:libXrender\n"             \
    "called_from_lib:libXss\n"

#endif  // _MSC_VER

#endif /* mozilla_TsanOptions_h */
