// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/sharding_environment/shard_handle.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

struct BSONArray;
class BSONObj;
class Status;
template <typename T>
class StatusWith;

/**
 * This class represents the layout and contents of documents contained in the
 * config.shards collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardType {
public:
    enum class ShardState : int {
        kNotShardAware = 0,
        kShardAware,
    };

    // Hardcoded UUID for the config server.
    static const UUID kConfigServerUuid;

    // Field names and types in the shards collection type.
    static const BSONField<std::string> name;
    static const BSONField<UUID> uuid;
    static const BSONField<std::string> host;
    static const BSONField<bool> draining;
    static const BSONField<BSONArray> tags;
    static const BSONField<ShardState> state;
    static const BSONField<Timestamp> topologyTime;
    static const BSONField<long long> replSetConfigVersion;
    static const long long kUninitializedReplSetConfigVersion = -1;
    ShardType() = default;
    ShardType(std::string name, std::string host, std::vector<std::string> tags = {});
    ShardType(std::string name,
              boost::optional<UUID> uuid,
              std::string host,
              std::vector<std::string> tags = {});

    /**
     * Constructs a new ShardType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ShardType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise returns NoSuchKey
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

    const std::string& getName() const;

    const boost::optional<UUID>& getUuid() const;

    void setUuid(boost::optional<UUID> uuid);

    const ShardHandle& getHandle() const;
    void setHandle(ShardHandle handle);

    const std::string& getHost() const {
        return _host.get();
    }
    void setHost(const std::string& host);

    bool getDraining() const {
        return _draining.value_or(false);
    }
    void setDraining(bool draining);

    std::vector<std::string> getTags() const {
        return _tags.value_or(std::vector<std::string>());
    }
    void setTags(const std::vector<std::string>& tags);

    Timestamp getTopologyTime() const {
        return _topologyTime.value_or(Timestamp());
    }
    void setTopologyTime(const Timestamp& topologyTime);

    long long getReplSetConfigVersion() const {
        return _replSetConfigVersion;
    }
    void setReplSetConfigVersion(long long replSetConfigVersion);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) Handle object embedding the shard name (the _id field of the persisted doc)
    // and internal UUID.
    boost::optional<ShardHandle> _handle;
    // (M)  connection string for the host(s)
    boost::optional<std::string> _host;
    // (O) is it draining chunks?
    boost::optional<bool> _draining;
    // (O) shard tags
    boost::optional<std::vector<std::string>> _tags;
    // (O) topologyTime
    boost::optional<Timestamp> _topologyTime;
    // (O) repl set config version.
    long long _replSetConfigVersion = kUninitializedReplSetConfigVersion;
};

}  // namespace mongo
