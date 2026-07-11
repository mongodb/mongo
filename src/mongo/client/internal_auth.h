// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/credential.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] auth {

/**
 * Sets the keys used by authenticateInternalClient - these should be a vector of raw passwords,
 * they will be digested and prepped appropriately by authenticateInternalClient depending
 * on what mechanism is used.
 */
void setInternalAuthKeys(const std::vector<std::string>& keys);

/**
 * Sets the parameters for non-password based internal authentication.
 */
void setInternalUserAuthParams(Credential credential);

/**
 * Returns whether there are multiple keys that will be tried while authenticating an internal
 * client (used for logging a startup warning).
 */
bool hasMultipleInternalAuthKeys();

/**
 * Returns whether there are any internal auth data set.
 */
bool isInternalAuthSet();

/**
 * Returns the AuthDB used by internal authentication.
 */
std::string getInternalAuthDB();

/**
 * Returns the internal auth credential for the given index and mechanism.
 * Returns boost::none if no internal auth has been configured or index is out of range.
 */
boost::optional<Credential> getInternalAuthParams(size_t idx, std::string_view mechanism);

/**
 * Create a Credential for internal X.509 authentication.
 */
Credential createInternalX509AuthCredential(
    boost::optional<std::string_view> userName = boost::none);

}  // namespace auth
}  // namespace mongo
