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

class DatabaseVersion : private DatabaseVersionBase {
public:
    using DatabaseVersionBase::getLastMod;
    using DatabaseVersionBase::toBSON;

    // It returns a new DatabaseVersion marked as fixed. A fixed database version is used to
    // distinguish databases that do not have entries in the sharding catalog, such as 'config' and
    // 'admin'
    static DatabaseVersion makeFixed();

    DatabaseVersion() = default;

    explicit DatabaseVersion(const BSONObj& obj) {
        DatabaseVersionBase::parseProtected(IDLParserErrorContext("DatabaseVersion"), obj);
    }

    /**
     * Constructor of a DatabaseVersion based on epochs
     */
    explicit DatabaseVersion(mongo::UUID uuid)
        : DatabaseVersion(uuid, boost::none /* timestamp */) {}

    /**
     * Constructor of a DatabaseVersion based on epochs and timestamps
     */
    DatabaseVersion(mongo::UUID uuid, boost::optional<mongo::Timestamp> timestamp)
        : DatabaseVersionBase(1 /* lastMod */) {
        setUuid(uuid);
        setTimestamp(timestamp);
    }

    DatabaseVersion makeUpdated() const;

    bool operator==(const DatabaseVersion& other) const {
        return getUuid() == other.getUuid() && getLastMod() == other.getLastMod();
    }

    bool operator!=(const DatabaseVersion& other) const {
        return !(*this == other);
    }

    bool isFixed() const {
        return getLastMod() == 0;
    }

    mongo::UUID getUuid() const {
        return *DatabaseVersionBase::getUuid();
    }
};

}  // namespace mongo
