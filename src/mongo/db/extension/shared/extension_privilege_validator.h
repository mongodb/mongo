// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo::extension {
class MongoExtensionPrivilegeActionEntry;

/**
 * Validates that a privilege names at least one action, since a privilege with no actions grants
 * nothing. Shared by every extension-defined entity that declares privileges.
 */
Status validateActionsNotEmpty(const std::vector<MongoExtensionPrivilegeActionEntry>& actions);

}  // namespace mongo::extension
