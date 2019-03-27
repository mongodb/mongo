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
#include <map>
#include <string>

#include "mongo/s/chunk_version.h"

namespace mongo {

class Client;

/**
 * There is one instance of these per each connection from mongos. Holds version state for each
 * namespace.
 */
class ShardedConnectionInfo {
    ShardedConnectionInfo(const ShardedConnectionInfo&) = delete;
    ShardedConnectionInfo& operator=(const ShardedConnectionInfo&) = delete;

public:
    ShardedConnectionInfo();
    ~ShardedConnectionInfo();

    static ShardedConnectionInfo* get(Client* client, bool create);
    static void reset(Client* client);

    /**
     * Returns the shard version associated with the specified namespace on this connection. If no
     * version is associated with the namespace returns boost::none.
     */
    boost::optional<ChunkVersion> getVersion(const std::string& ns) const;

    /**
     * Assigns a new version on the connection to the specified namespace.
     */
    void setVersion(const std::string& ns, const ChunkVersion& version);

private:
    typedef std::map<std::string, ChunkVersion> NSVersionMap;

    // Map from a namespace string to the chunk version with which this connection has been
    // initialized for the specified namespace
    NSVersionMap _versions;
};


}  // namespace mongo
