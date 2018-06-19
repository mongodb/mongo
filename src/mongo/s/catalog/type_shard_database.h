/**
 *    Copyright (C) 2018 10gen Inc.
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
#include "mongo/s/database_version_gen.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class Status;
template <typename T>
class StatusWith;

/**
 * This class represents the layout and contents of documents contained in the shard server's
 * config.databases collection. All manipulation of documents coming from that collection should
 * be done with this class.
 *
 * Expected shard server config.databases collection format:
 *   {
 *      "_id" : "foo",
 *      "version" : {
 *          "uuid" : UUID
 *          "lastMod" : 1
 *      },
 *      "primary": "shard0000",
 *      "partitioned": true,
 *      "enterCriticalSectionCounter" : 4                    // optional
 *   }
 *
 * enterCriticalSectionCounter is currently just an OpObserver signal, thus otherwise ignored here.
 */
class ShardDatabaseType {
public:
    static const BSONField<std::string> name;  // "_id"
    static const BSONField<DatabaseVersion> version;
    static const BSONField<std::string> primary;
    static const BSONField<bool> partitioned;
    static const BSONField<int> enterCriticalSectionCounter;

    ShardDatabaseType(const std::string dbName,
                      DatabaseVersion version,
                      const ShardId primary,
                      bool partitioned);

    /**
     * Constructs a new ShardDatabaseType object from BSON. Also does validation of the contents.
     */
    static StatusWith<ShardDatabaseType> fromBSON(const BSONObj& source);

    /**
     * Returns the BSON representation of this shard database type object.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getDbName() const {
        return _name;
    }
    void setDbName(const std::string& dbName);

    const DatabaseVersion getDbVersion() const {
        return _version;
    }
    void setDbVersion(DatabaseVersion version);

    const ShardId& getPrimary() const {
        return _primary;
    }
    void setPrimary(const ShardId& primary);

    bool getPartitioned() const {
        return _partitioned;
    }
    void setPartitioned(bool partitioned);

private:
    std::string _name;
    DatabaseVersion _version;
    ShardId _primary;
    bool _partitioned;
};

}  // namespace mongo
