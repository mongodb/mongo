/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_MemoryProtectionExceptionHandler_h
#define ds_MemoryProtectionExceptionHandler_h

#include "jstypes.h"

namespace js {

/*
 * This structure allows you to annotate crashes resulting from unauthorized
 * access to protected memory in regions of interest, to make them stand out
 * from other heap corruption crashes.
 */

struct MemoryProtectionExceptionHandler
{
    /* Installs the exception handler; called early during initialization. */
    static bool install();

    /* If the exception handler is disabled, it won't be installed. */
    static bool isDisabled();

    /*
     * Marks a region of memory as important; if something tries to access this
     * region in an unauthorized way (e.g. writing to read-only memory),
     * the resulting crash will be annotated to stand out from other random
     * heap corruption.
     */
    static void addRegion(void* addr, size_t size);

    /* Removes a previously added region. */
    static void removeRegion(void* addr);

    /* Uninstalls the exception handler; called late during shutdown. */
    static void uninstall();
};

} /* namespace js */

#endif /* ds_MemoryProtectionExceptionHandler_h */
