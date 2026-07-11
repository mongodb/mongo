// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <mutex>
#include <string>
#include <string_view>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {
using namespace std::literals::string_view_literals;

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
    struct Stats {
        // Count of cache entries
        int64_t count{0};
        // Number of cache hits
        int64_t hits{0};
        // Number of cache misses
        int64_t misses{0};
    };

    /**
     * Returns precomputed SCRAMSecrets, if one has already been
     * stored for the specified hostname and the provided presecrets
     * match those recorded for the hostname. Otherwise, no secrets
     * are returned.
     */
    scram::Secrets<HashBlock> getCachedSecrets(
        const HostAndPort& target, const scram::Presecrets<HashBlock>& presecrets) const {
        const std::lock_guard<std::mutex> lock(_hostToSecretsMutex);

        // Search the cache for a record associated with the host we're trying to connect to.
        auto foundSecret = _hostToSecrets.find(target);
        if (foundSecret == _hostToSecrets.end()) {
            ++_stats.misses;
            logCacheEvent("miss (secret not found)"sv);
            return {};
        }

        // Presecrets contain parameters provided by the server, which may change. If the
        // cached presecrets don't match the presecrets we have on hand, we must not return the
        // stale cached secrets. We'll need to rerun the SCRAM computation.
        const auto& foundPresecrets = foundSecret->second.first;
        if (foundPresecrets == presecrets) {
            ++_stats.hits;
            logCacheEvent("hit"sv);
            return foundSecret->second.second;
        } else {
            ++_stats.misses;
            logCacheEvent("miss (stale cached secret)"sv);
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
        const std::lock_guard<std::mutex> lock(_hostToSecretsMutex);

        typename HostToSecretsMap::iterator it;
        bool insertionSuccessful;
        auto cacheRecord = std::make_pair(std::move(presecrets), std::move(secrets));
        // Insert the presecrets, and the secrets we computed for them into the cache
        std::tie(it, insertionSuccessful) = _hostToSecrets.emplace(std::move(target), cacheRecord);
        // If there was already a cache entry for the target HostAndPort, we should overwrite it.
        // We have fresher presecrets and secrets.
        if (!insertionSuccessful) {
            it->second = std::move(cacheRecord);
            logCacheEvent("overwrite"sv);
        } else {
            logCacheEvent("insertion"sv);
        }
    }

    /**
     * Return metrics about the cache
     */
    Stats getStats() const {
        const std::lock_guard<std::mutex> lock(_hostToSecretsMutex);
        Stats stats = _stats;
        stats.count = _hostToSecrets.size();
        return stats;
    }

private:
    void logCacheEvent(std::string_view event) const {
        LOGV2_DEBUG(9542300,
                    5,
                    "Cache stats updated",
                    "event"_attr = event,
                    "addr"_attr = (std::size_t)this,
                    "count"_attr = _hostToSecrets.size(),
                    "hits"_attr = _stats.hits,
                    "misses"_attr = _stats.misses);
    }

    mutable std::mutex _hostToSecretsMutex;
    HostToSecretsMap _hostToSecrets;
    mutable Stats _stats;
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
