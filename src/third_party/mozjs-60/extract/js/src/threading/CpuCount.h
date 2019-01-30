/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <inttypes.h>

namespace js {

/*
 * Return the number of cores on the system.  If the system provides logical
 * cores (such as hyperthreads) then count each logical core as an actual core.
 */
uint32_t GetCPUCount();

}
