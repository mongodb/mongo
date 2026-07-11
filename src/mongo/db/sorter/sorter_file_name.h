// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <boost/filesystem/path.hpp>

[[MONGO_MOD_PUBLIC]];
namespace mongo::sorter {
/**
 * Generates a new file name on each call using a static, atomic and monotonically increasing
 * number. Each name is suffixed with a random number generated at startup, to prevent name
 * collisions when the index build external sort files are preserved across restarts.
 */
boost::filesystem::path nextFileName(boost::filesystem::path path);
}  // namespace mongo::sorter
