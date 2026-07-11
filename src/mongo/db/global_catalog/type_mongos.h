// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the
 * config.mongos collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MongosType {
public:
    // Name of the mongos collection in the config server.
    static const NamespaceString ConfigNS;

    // Field names and types in the mongos collection type.
    static const BSONField<std::string> name;
    static const BSONField<Date_t> created;
    static const BSONField<Date_t> ping;
    static const BSONField<long long> uptime;
    static const BSONField<bool> waiting;
    static const BSONField<std::string> mongoVersion;
    static const BSONField<long long> configVersion;
    static const BSONField<BSONArray> advisoryHostFQDNs;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Constructs a new MongosType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<MongosType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getName() const {
        return _name.get();
    }
    void setName(const std::string& name);

    const Date_t& getCreated() const {
        return _created.get();
    }
    void setCreated(const Date_t& created);

    const Date_t& getPing() const {
        return _ping.get();
    }
    void setPing(const Date_t& ping);

    long long getUptime() const {
        return _uptime.get();
    }
    void setUptime(long long uptime);

    bool getWaiting() const {
        return _waiting.get();
    }
    bool isWaitingSet() const {
        return _waiting.is_initialized();
    }
    void setWaiting(bool waiting);

    const std::string& getMongoVersion() const {
        return _mongoVersion.get();
    }
    bool isMongoVersionSet() const {
        return _mongoVersion.is_initialized();
    }
    void setMongoVersion(const std::string& mongoVersion);

    long long getConfigVersion() const {
        return _configVersion.get();
    }
    void setConfigVersion(long long configVersion);

    std::vector<std::string> getAdvisoryHostFQDNs() const {
        return _advisoryHostFQDNs.value_or(std::vector<std::string>());
    }
    void setAdvisoryHostFQDNs(const std::vector<std::string>& advisoryHostFQDNs);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) "host:port" for this mongos
    boost::optional<std::string> _name;
    // (M) Time of mongos creation
    boost::optional<Date_t> _created;
    // (M) last time it was seen alive
    boost::optional<Date_t> _ping;
    // (M) uptime at the last ping
    boost::optional<long long> _uptime;
    // (M) used to indicate if we are going to sleep after ping. For testing purposes
    boost::optional<bool> _waiting;
    // (O) the mongodb version of the pinging mongos
    boost::optional<std::string> _mongoVersion;
    // (O) the config version of the pinging mongos
    boost::optional<long long> _configVersion;
    // (O) the results of hostname canonicalization on the pinging mongos
    boost::optional<std::vector<std::string>> _advisoryHostFQDNs;
};

}  // namespace mongo
