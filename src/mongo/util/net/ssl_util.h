// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"

#include <fstream>
#include <string_view>

namespace mongo {

namespace ssl_util {

/**
 * Find a specific kind of PEM blob marked by BEGIN and END in a string.
 */
StatusWith<std::string_view> findPEMBlob(std::string_view blob,
                                         std::string_view type,
                                         size_t position = 0,
                                         bool allowEmpty = false);

/**
 * Read the contents of a PEM-encoded file to a std::string.
 */
StatusWith<std::string> readPEMFile(std::string_view fileName);

}  // namespace ssl_util

}  // namespace mongo
