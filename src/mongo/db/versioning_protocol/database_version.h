// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/versioning_protocol/database_version_base_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <iosfwd>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * This class is used to represent a specific version of a Database.
 *
 * Currently it is implemented as a (uuid, timestamp, lastMod) triplet, where the
 * timestamp is optional in versions prior 4.9. The uuid is going to be removed soon,
 * since they are not comparable.
 *
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DatabaseVersion : public DatabaseVersionBase {
public:
    /**
     * The name for the database version information field, which shard-aware commands should
     * include if they want to convey database version.
     */
    static constexpr std::string_view kDatabaseVersionField = "databaseVersion"sv;

    using DatabaseVersionBase::getTimestamp;

    DatabaseVersion() = default;

    explicit DatabaseVersion(const BSONObj& obj) {
        DatabaseVersionBase::parseProtected(obj, IDLParserContext("DatabaseVersion"));
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

    // TODO (SERVER-115178): Remove once v9.0 branches out
    const boost::optional<mongo::LogicalTime>& getPlacementConflictTime_DEPRECATED() const {
        return DatabaseVersionBase::getPlacementConflictTime();
    }

    // TODO (SERVER-115178): Remove once v9.0 branches out
    void setPlacementConflictTime_DEPRECATED(boost::optional<mongo::LogicalTime> value) {
        DatabaseVersionBase::setPlacementConflictTime(value);
    }

private:
    // TODO (SERVER-115178): Remove once v9.0 branches out
    const boost::optional<mongo::LogicalTime>& getPlacementConflictTime() const {
        MONGO_UNREACHABLE_TASSERT(10909302);
    }

    // TODO (SERVER-115178): Remove once v9.0 branches out
    void setPlacementConflictTime(boost::optional<mongo::LogicalTime> value) {
        MONGO_UNREACHABLE_TASSERT(10909303);
    }
};

inline std::ostream& operator<<(std::ostream& s, const DatabaseVersion& v) {
    return s << v.toString();
}

inline StringBuilder& operator<<(StringBuilder& s, const DatabaseVersion& v) {
    return s << v.toString();
}

}  // namespace mongo
