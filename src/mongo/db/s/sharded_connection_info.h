/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <map>
#include <string>

#include "mongo/base/disallow_copying.h"

namespace mongo {

struct ChunkVersion;
class Client;

/**
 * There is one instance of these per each connection from mongos. Holds version state for each
 * namespace.
 */
class ShardedConnectionInfo {
    MONGO_DISALLOW_COPYING(ShardedConnectionInfo);

public:
    ShardedConnectionInfo();
    ~ShardedConnectionInfo();

    static ShardedConnectionInfo* get(Client* client, bool create);

    /**
     * Returns the shard version associated with the specified namespace on this connection. If no
     * version is associated with the namespace returns ChunkVersion::UNSHARDED.
     */
    ChunkVersion getVersion(const std::string& ns) const;

    /**
     * Assigns a new version on the connection to the specified namespace.
     */
    void setVersion(const std::string& ns, const ChunkVersion& version);

    static void reset(Client* client);
    static void addHook();

private:
    typedef std::map<std::string, ChunkVersion> NSVersionMap;

    // Map from a namespace string to the chunk version with which this connection has been
    // initialized for the specified namespace
    NSVersionMap _versions;

    // If this is true, then chunk versions aren't checked, and all operations are allowed
    bool _forceVersionOk;
};


}  // namespace mongo
