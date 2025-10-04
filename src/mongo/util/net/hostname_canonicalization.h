/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status_with.h"

#include <string>
#include <vector>

namespace mongo {

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
