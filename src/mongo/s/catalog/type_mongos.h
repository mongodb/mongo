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
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the
 * config.mongos collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class MongosType {
public:
    // Name of the mongos collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the mongos collection type.
    static const BSONField<std::string> name;
    static const BSONField<Date_t> ping;
    static const BSONField<long long> uptime;
    static const BSONField<bool> waiting;
    static const BSONField<std::string> mongoVersion;
    static const BSONField<long long> configVersion;

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

    const Date_t& getPing() const {
        return _ping.get();
    }
    void setPing(const Date_t& ping);

    long long getUptime() const {
        return _uptime.get();
    }
    void setUptime(const long long uptime);

    bool getWaiting() const {
        return _waiting.get();
    }
    bool isWaitingSet() const {
        return _waiting.is_initialized();
    }
    void setWaiting(const bool waiting);

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
    void setConfigVersion(const long long configVersion);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) "host:port" for this mongos
    boost::optional<std::string> _name;
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
};

}  // namespace mongo
