// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

/**
 * Allows other components to observe when storage engine flushes all files.
 */
class [[MONGO_MOD_OPEN]] FlushAllFilesObserver {
public:
    virtual ~FlushAllFilesObserver() = default;

    virtual void onFlushAllFiles() = 0;
};

}  // namespace mongo
