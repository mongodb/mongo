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

#include "mongo/s/database_version_gen.h"

namespace mongo {

/**
 * This class is used to represent a specific version of a Database.
 *
 * Currently it is implemented as a (uuid, timestamp, lastMod) triplet, where the
 * timestamp is optional in versions prior 4.9. The uuid is going to be removed soon,
 * since they are not comparable.
 *
 */
class DatabaseVersion : public DatabaseVersionBase {
public:
    /**
     * The name for the database version information field, which shard-aware commands should
     * include if they want to convey database version.
     */
    static constexpr StringData kDatabaseVersionField = "databaseVersion"_sd;

    using DatabaseVersionBase::getTimestamp;

    DatabaseVersion() = default;

    explicit DatabaseVersion(const BSONObj& obj) {
        DatabaseVersionBase::parseProtected(IDLParserContext("DatabaseVersion"), obj);
    }

    explicit DatabaseVersion(const DatabaseVersionBase& dbv) : DatabaseVersionBase(dbv) {}

    /**
     * Constructor of a DatabaseVersion based on epochs and timestamps
     */
    DatabaseVersion(mongo::UUID uuid, mongo::Timestamp timestamp)
        : DatabaseVersionBase(timestamp, 1 /* lastMod */) {
        setUuid(uuid);
    }

    // Returns a new hardcoded DatabaseVersion value, which is used to distinguish databases that do
    // not have entries in the sharding catalog, namely 'config' and 'admin'.
    static DatabaseVersion makeFixed();

    // Returns a new DatabaseVersion with just the lastMod incremented. This indicates that the
    // database changed primary, as opposed to being dropped and recreated.
    DatabaseVersion makeUpdated() const;

    /**
     * It returns true if the Timestamp and lastmod of both versions are the same.
     */
    bool operator==(const DatabaseVersion& other) const {
        return getTimestamp() == other.getTimestamp() && getLastMod() == other.getLastMod();
    }

    bool operator!=(const DatabaseVersion& other) const {
        return !(*this == other);
    }

    bool operator<(const DatabaseVersion& other) const;

    bool operator>(const DatabaseVersion& other) const {
        return other < *this;
    }

    bool operator<=(const DatabaseVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const DatabaseVersion& other) const {
        return !(*this < other);
    }

    bool isFixed() const {
        return getLastMod() == 0;
    }

    UUID getUuid() const {
        return *DatabaseVersionBase::getUuid();
    }

    std::string toString() const;
};

inline std::ostream& operator<<(std::ostream& s, const DatabaseVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const DatabaseVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
