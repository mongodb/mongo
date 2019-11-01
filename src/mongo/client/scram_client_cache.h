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

#include <boost/optional.hpp>
#include <string>

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * A cache for the intermediate steps of the SCRAM-SHA-1 computation.
 *
 * Clients wishing to authenticate to a server using SCRAM-SHA-1
 * must produce a set of credential objects from their password,
 * a salt, and an iteration count. The computation to generate these
 * is very expensive, proportional to the iteration count. The high
 * cost of this computation prevents brute force attacks on
 * intercepted SCRAM authentication data, or a stolen password
 * database. The inputs to the function are unlikely to frequently
 * change. Caching the relationship between the inputs and the
 * resulting output should make repeated authentication attempts
 * to a single server much faster.
 *
 * This is explicitly permitted by RFC5802, section 5.1:
 *
 * "Note that a client implementation MAY cache
 * ClientKey&ServerKey (or just SaltedPassword) for later
 * reauthentication to the same service, as it is likely that the
 * server is going to advertise the same salt value upon
 * reauthentication.  This might be useful for mobile clients where
 * CPU usage is a concern."
 */
template <typename HashBlock>
class SCRAMClientCache {
private:
    using HostToSecretsPair = std::pair<scram::Presecrets<HashBlock>, scram::Secrets<HashBlock>>;
    using HostToSecretsMap = stdx::unordered_map<HostAndPort, HostToSecretsPair>;

public:
    /**
     * Returns precomputed SCRAMSecrets, if one has already been
     * stored for the specified hostname and the provided presecrets
     * match those recorded for the hostname. Otherwise, no secrets
     * are returned.
     */
    scram::Secrets<HashBlock> getCachedSecrets(
        const HostAndPort& target, const scram::Presecrets<HashBlock>& presecrets) const {
        const stdx::lock_guard<Latch> lock(_hostToSecretsMutex);

        // Search the cache for a record associated with the host we're trying to connect to.
        auto foundSecret = _hostToSecrets.find(target);
        if (foundSecret == _hostToSecrets.end()) {
            return {};
        }

        // Presecrets contain parameters provided by the server, which may change. If the
        // cached presecrets don't match the presecrets we have on hand, we must not return the
        // stale cached secrets. We'll need to rerun the SCRAM computation.
        const auto& foundPresecrets = foundSecret->second.first;
        if (foundPresecrets == presecrets) {
            return foundSecret->second.second;
        } else {
            return {};
        }
    }

    /**
     * Records a set of precomputed SCRAMSecrets for the specified
     * host, along with the presecrets used to generate them.
     */
    void setCachedSecrets(HostAndPort target,
                          scram::Presecrets<HashBlock> presecrets,
                          scram::Secrets<HashBlock> secrets) {
        const stdx::lock_guard<Latch> lock(_hostToSecretsMutex);

        typename HostToSecretsMap::iterator it;
        bool insertionSuccessful;
        auto cacheRecord = std::make_pair(std::move(presecrets), std::move(secrets));
        // Insert the presecrets, and the secrets we computed for them into the cache
        std::tie(it, insertionSuccessful) = _hostToSecrets.emplace(std::move(target), cacheRecord);
        // If there was already a cache entry for the target HostAndPort, we should overwrite it.
        // We have fresher presecrets and secrets.
        if (!insertionSuccessful) {
            it->second = std::move(cacheRecord);
        }
    }

private:
    mutable Mutex _hostToSecretsMutex = MONGO_MAKE_LATCH("SCRAMClientCache::_hostToSecretsMutex");
    HostToSecretsMap _hostToSecrets;
};

}  // namespace mongo
