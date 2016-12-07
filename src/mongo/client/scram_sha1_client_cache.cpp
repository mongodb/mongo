/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/scram_sha1_client_cache.h"

namespace mongo {

boost::optional<scram::SCRAMSecrets> SCRAMSHA1ClientCache::getCachedSecrets(
    const HostAndPort& target, const scram::SCRAMPresecrets& presecrets) const {
    const stdx::lock_guard<stdx::mutex> lock(_hostToSecretsMutex);

    // Search the cache for a record associated with the host we're trying to connect to.
    auto foundSecret = _hostToSecrets.find(target);
    if (foundSecret != _hostToSecrets.end()) {
        // Presecrets contain parameters provided by the server, which may change. If the
        // cached presecrets don't match the presecrets we have on hand, we must not return the
        // stale cached secrets. We'll need to rerun the SCRAM computation.
        const scram::SCRAMPresecrets& foundPresecrets = foundSecret->second.first;
        if (foundPresecrets == presecrets) {
            return foundSecret->second.second;
        }
    }
    return {};
}

void SCRAMSHA1ClientCache::setCachedSecrets(HostAndPort target,
                                            scram::SCRAMPresecrets presecrets,
                                            scram::SCRAMSecrets secrets) {
    const stdx::lock_guard<stdx::mutex> lock(_hostToSecretsMutex);

    decltype(_hostToSecrets)::iterator it;
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

}  // namespace mongo
