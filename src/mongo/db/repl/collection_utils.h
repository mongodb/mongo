// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace repl {
/**
 * Returns if the provided collection can skip acquiring the RSTL lock.
 */
inline bool canCollectionSkipRSTLLockAcquisition(const NamespaceString& nss) {
    // TODO SERVER-71536: Document all the collections that can skip the RSTL lock acquisition.
    //
    // system.profile is an unreplicated local collection that doesn't need to acquire
    // the RSTL lock.
    return nss.isSystemDotProfile();
}
}  // namespace repl
}  // namespace mongo
