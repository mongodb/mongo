/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MacroArgs.h"

MOZ_STATIC_ASSERT_VALID_ARG_COUNT(1);
MOZ_STATIC_ASSERT_VALID_ARG_COUNT(1, 2);

static_assert(MOZ_PASTE_PREFIX_AND_ARG_COUNT(100, a, b, c) == 1003, "");

static_assert(MOZ_PASTE_PREFIX_AND_ARG_COUNT(, a, b, c) == 3, "");
static_assert(MOZ_PASTE_PREFIX_AND_ARG_COUNT(, a) == 1, "");
static_assert(MOZ_PASTE_PREFIX_AND_ARG_COUNT(, !a) == 1, "");
static_assert(MOZ_PASTE_PREFIX_AND_ARG_COUNT(, (a, b)) == 1, "");

static_assert(MOZ_PASTE_PREFIX_AND_ARG_COUNT(, MOZ_ARGS_AFTER_1(a, b, c)) == 2,
              "MOZ_ARGS_AFTER_1(a, b, c) should expand to 'b, c'");
static_assert(MOZ_ARGS_AFTER_2(a, b, 3) == 3,
              "MOZ_ARGS_AFTER_2(a, b, 3) should expand to '3'");

static_assert(MOZ_ARG_1(10, 20, 30) == 10, "");
static_assert(MOZ_ARG_2(10, 20, 30) == 20, "");

int
main()
{
  return 0;
}
