/**
 *    Copyright (C) 2012 10gen Inc.
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

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class BSONObj;
class Status;
template <typename T>
class StatusWith;


/**
 * This class represents the layout and contents of documents contained in the config.databases
 * collection. All manipulation of documents coming from that collection should be done with
 * this class.
 */
class DatabaseType {
public:
    DatabaseType(const std::string& dbName,
                 const ShardId& primaryShard,
                 bool sharded,
                 DatabaseVersion);

#ifdef _WIN32
    // TODO: Remove this when Microsoft's implementation of std::future doesn't require a default
    // constructor.
    // This type should not normally have a default constructor, however Microsoft's implementation
    // of future requires one in violation of the standard so we're providing one only for Windows.
    DatabaseType() = default;
#endif

    // Name of the databases collection in the config server.
    static const NamespaceString ConfigNS;

    static const BSONField<std::string> name;
    static const BSONField<std::string> primary;
    static const BSONField<bool> sharded;
    static const BSONField<BSONObj> version;

    /**
     * Constructs a new DatabaseType object from BSON. Also does validation of the contents.
     */
    static StatusWith<DatabaseType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise returns NoSuchKey and information
     * about what is the first field which is missing.
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

    const std::string& getName() const {
        return _name;
    }
    void setName(const std::string& name);

    const ShardId& getPrimary() const {
        return _primary;
    }
    void setPrimary(const ShardId& primary);

    bool getSharded() const {
        return _sharded;
    }
    void setSharded(bool sharded);

    DatabaseVersion getVersion() const {
        return _version;
    }
    void setVersion(const DatabaseVersion& version);

private:
    std::string _name;
    ShardId _primary;
    bool _sharded;
    DatabaseVersion _version;
};

}  // namespace mongo
