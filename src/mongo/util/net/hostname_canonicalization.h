// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"

#include <string>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * DNS canonicalization converts a hostname into another potentially more globally useful hostname.
 *
 * This involves issuing some combination of IP and name lookups.
 * This enum controls which canonicalization process a client will perform on a
 * hostname it is canonicalizing.
 */
enum class HostnameCanonicalizationMode {
    kNone,              // Perform no canonicalization at all
    kForward,           // Perform a DNS lookup on the hostname, follow CNAMEs to find the A record
    kForwardAndReverse  // Forward resolve to get an IP, then perform reverse lookup on it
};

/**
 *  Returns zero or more fully qualified hostnames associated with the provided hostname.
 *
 *  May return an empty vector if no FQDNs can be determined, or if the underlying
 *  implementation returns an error. The returned information is advisory only.
 */
StatusWith<std::vector<std::string>> getHostFQDNs(std::string hostName,
                                                  HostnameCanonicalizationMode mode);

}  // namespace mongo
