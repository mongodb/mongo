/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "threading/CpuCount.h"

#include "util/Windows.h"

uint32_t
js::GetCPUCount()
{
    static uint32_t ncpus = 0;

    if (ncpus == 0) {
        SYSTEM_INFO sysinfo;
        GetSystemInfo(&sysinfo);
        ncpus = uint32_t(sysinfo.dwNumberOfProcessors);
    }

    return ncpus;
}
