/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <unistd.h>

#include "threading/CpuCount.h"

uint32_t
js::GetCPUCount()
{
    static uint32_t ncpus = 0;

    // _SC_NPROCESSORS_CONF and _SC_NPROCESSORS_ONLN are common, but not
    // standard.
    if (ncpus == 0) {
#if defined(_SC_NPROCESSORS_CONF)
        long n = sysconf(_SC_NPROCESSORS_CONF);
        ncpus = (n > 0) ? uint32_t(n) : 1;
#elif defined(_SC_NPROCESSORS_ONLN)
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        ncpus = (n > 0) ? uint32_t(n) : 1;
#else
        ncpus = 1;
#endif
    }

    return ncpus;
}
