// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/tenant_id.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Validation callback for setParameter 'opensslCipherConfig'.
 */
Status validateOpensslCipherConfig(const std::string&, const boost::optional<TenantId>&);

/**
 * Validation callback for setParameter 'disableNonTLSConnectionLogging'.
 */
Status validateDisableNonTLSConnectionLogging(const bool&, const boost::optional<TenantId>&);

/**
 * Records that disableNonTLSConnectionLogging has been set.
 */
Status onUpdateDisableNonTLSConnectionLogging(const bool&);

}  // namespace mongo
