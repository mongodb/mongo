// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * Hashes the password so that it can be used for SCRAM-SHA-1 in MONGODB-CR compatability mode.
 */
std::string createPasswordDigest(std::string_view username, std::string_view clearTextPassword);

}  // namespace mongo
