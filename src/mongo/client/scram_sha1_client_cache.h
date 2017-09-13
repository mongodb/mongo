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
#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/crypto/mechanism_scram.h"
#include "mongo/stdx/mutex.h"
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
class SCRAMSHA1ClientCache {
public:
    /**
     * Returns precomputed SCRAMSecrets, if one has already been
     * stored for the specified hostname and the provided presecrets
     * match those recorded for the hostname. Otherwise, no secrets
     * are returned.
     */
    scram::SCRAMSecrets getCachedSecrets(const HostAndPort& target,
                                         const scram::SCRAMPresecrets& presecrets) const;

    /**
     * Records a set of precomputed SCRAMSecrets for the specified
     * host, along with the presecrets used to generate them.
     */
    void setCachedSecrets(HostAndPort target,
                          scram::SCRAMPresecrets presecrets,
                          scram::SCRAMSecrets secrets);

private:
    mutable stdx::mutex _hostToSecretsMutex;
    stdx::unordered_map<HostAndPort, std::pair<scram::SCRAMPresecrets, scram::SCRAMSecrets>>
        _hostToSecrets;
};

}  // namespace mongo
