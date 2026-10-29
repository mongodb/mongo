// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace mongo {
template <class T>
class StatusWith;

/**
 * This method takes in a filename and returns the contents as a vector of strings.
 *
 * The contents of the file are interpreted as a YAML file and may either contain a scalar (string)
 * value or a sequence of scalar values. Each value may only contain valid base-64 characters.
 *
 * Whitespace within each key will be stripped from the final keys (e.g. "key 1" = "key1").
 *
 * This will return an error if the file was empty or contained invalid characters.
 *
 *
 */
[[MONGO_MOD_PUBLIC]] StatusWith<std::vector<std::string>> readSecurityFile(
    const std::string& filename);

}  // namespace mongo
