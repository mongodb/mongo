// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {
template <class T>
class StatusWith;

/**
 * This method checks the validity of filename as a security key, hashes its
 * contents, and stores it in the internalSecurity variable.  Prints an
 * error message to the logs if there's an error.
 * @param filename the file containing the key
 * @return if the key was successfully stored
 */
bool setUpSecurityKey(const std::string& filename, ClusterAuthMode mode);

}  // namespace mongo
