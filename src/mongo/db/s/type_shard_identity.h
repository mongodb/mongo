/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#include <string>

#include "mongo/client/connection_string.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/write_ops/batched_update_request.h"

namespace mongo {

/**
 * Contains all the information needed to make a mongod instance shard aware.
 */
class ShardIdentityType {
public:
    // The _id value for this document type.
    static const std::string IdName;

    // Field names and types in a shardIdentity document.
    static const BSONField<std::string> configsvrConnString;
    static const BSONField<std::string> shardName;
    static const BSONField<OID> clusterId;

    ShardIdentityType() = default;

    /**
     * Constructs a new ShardIdentityType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ShardIdentityType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    bool isConfigsvrConnStringSet() const;
    const ConnectionString& getConfigsvrConnString() const;
    void setConfigsvrConnString(ConnectionString connString);

    bool isShardNameSet() const;
    const std::string& getShardName() const;
    void setShardName(std::string shardName);

    bool isClusterIdSet() const;
    const OID& getClusterId() const;
    void setClusterId(OID clusterId);

    /**
     * Returns an update object that can be used to update the config server field of the
     * shardIdentity document with the new connection string.
     */
    static BSONObj createConfigServerUpdateObject(const std::string& newConnString);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) connection string to the config server.
    boost::optional<ConnectionString> _configsvrConnString;
    // (M) contains the name of the shard.
    boost::optional<std::string> _shardName;
    // (M) contains the (unique) identifier of the cluster.
    boost::optional<OID> _clusterId;
};

}  // namespace mongo
