// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"

#include <string>

namespace mongo {

/**
 * Validation callback for setParameter 'opensslCipherConfig'.
 */
Status validateOpensslCipherConfig(const std::string&);

/**
 * Validation callback for setParameter 'disableNonTLSConnectionLogging'.
 */
Status validateDisableNonTLSConnectionLogging(const bool&);

/**
 * Records that disableNonTLSConnectionLogging has been set.
 */
Status onUpdateDisableNonTLSConnectionLogging(const bool&);

}  // namespace mongo
