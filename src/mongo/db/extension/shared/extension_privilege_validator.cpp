// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/extension_privilege_validator.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/extension/public/extension_privilege_gen.h"

namespace mongo::extension {

Status validateActionsNotEmpty(const std::vector<MongoExtensionPrivilegeActionEntry>& actions) {
    if (actions.empty()) {
        return Status(ErrorCodes::ExtensionError, "requiredPrivileges.actions must not be empty.");
    }
    return Status::OK();
}

}  // namespace mongo::extension
