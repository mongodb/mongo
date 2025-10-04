/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/FunctionFlags.h"  // js::FunctionFlags::Flags
#include "jsfriendapi.h"       // js::JS_FUNCTION_INTERPRETED_BITS

static_assert((js::FunctionFlags::Flags::BASESCRIPT |
               js::FunctionFlags::Flags::SELFHOSTLAZY) ==
                  js::JS_FUNCTION_INTERPRETED_BITS,
              "jsfriendapi.h's FunctionFlags::INTERPRETED-alike is wrong");
