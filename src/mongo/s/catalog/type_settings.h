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

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/optional.hpp>
#include <string>
#include <utility>

#include "mongo/bson/bson_field.h"
#include "mongo/db/write_concern_options.h"

namespace mongo {

struct WriteConcernOptions;
class BSONObj;
template <typename T>
class StatusWith;

using BoostTimePair = std::pair<boost::posix_time::ptime, boost::posix_time::ptime>;

/**
 * This class represents the layout and contents of documents contained in the
 * config.settings collection. All manipulation of documents coming from that
 * collection should be done with this class.
 *
 * Usage Example:
 *
 *     // Contact the config. 'conn' has been obtained before.
 *     DBClientBase* conn;
 *     BSONObj query = QUERY(SettingsType::exampleField(SettingsType::ExampleFieldName));
 *     exampleDoc = conn->findOne(SettingsType::ConfigNS, query);
 *
 *     // Process the response.
 *     StatusWith<SettingsType> exampleResult = SettingsType::fromBSON(exampleDoc);
 *     if (!exampleResult.isOK()) {
 *         if (exampleResult.getStatus() == ErrorCodes::NoSuchKey) {
 *             // exampleDoc has no key set or is empty
 *         }
 *         // handle error -- exampleResult.getStatus()
 *     }
 *     SettingsType exampleType = exampleResult.getValue();
 */
class SettingsType {
public:
    // Name of the settings collection in the config server.
    static const std::string ConfigNS;

    static const std::string BalancerDocKey;
    static const std::string ChunkSizeDocKey;

    // Field names and types in the settings collection type.
    static const BSONField<std::string> key;
    static const BSONField<long long> chunkSizeMB;
    static const BSONField<bool> balancerStopped;
    static const BSONField<BSONObj> balancerActiveWindow;
    static const BSONField<bool> deprecated_secondaryThrottle;
    static const BSONField<BSONObj> migrationWriteConcern;
    static const BSONField<bool> waitForDelete;

    /**
     * Returns OK if all mandatory fields have been set and their corresponding
     * values are valid.
     */
    Status validate() const;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Constructs a new ShardType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<SettingsType> fromBSON(const BSONObj& source);

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    /**
     * Returns the write concern to use for balancing.
     * Uses *deprecated* secondary throttle if migration write concern is not set.
     */
    std::unique_ptr<WriteConcernOptions> getWriteConcern() const;

    /**
     * Returns true if either 'now' is in the balancing window or
     * if no balancing window exists.
     */
    bool inBalancingWindow(const boost::posix_time::ptime& now) const;

    bool isKeySet() const {
        return _key.is_initialized();
    }
    const std::string& getKey() const {
        return _key.get();
    }
    void setKey(const std::string& key);

    bool isChunkSizeMBSet() const {
        return _chunkSizeMB.is_initialized();
    }
    long long getChunkSizeMB() const {
        return _chunkSizeMB.get();
    }
    void setChunkSizeMB(const long long chunkSizeMB);

    bool isBalancerStoppedSet() const {
        return _balancerStopped.is_initialized();
    }
    bool getBalancerStopped() const {
        return _balancerStopped.get();
    }
    void setBalancerStopped(const bool balancerStopped);

    bool isBalancerActiveWindowSet() const {
        return _balancerActiveWindow.is_initialized();
    }
    const BoostTimePair& getBalancerActiveWindow() const {
        return _balancerActiveWindow.get();
    }
    void setBalancerActiveWindow(const BSONObj& balancerActiveWindow);

    bool isSecondaryThrottleSet() const {
        return _secondaryThrottle.is_initialized();
    }
    bool getSecondaryThrottle() const {
        return _secondaryThrottle.get();
    }
    void setSecondaryThrottle(const bool secondaryThrottle);

    bool isMigrationWriteConcernSet() const {
        return _migrationWriteConcern.is_initialized();
    }
    const WriteConcernOptions& getMigrationWriteConcern() const {
        return _migrationWriteConcern.get();
    }
    void setMigrationWriteConcern(const BSONObj& migrationWriteConcern);

    bool isWaitForDeleteSet() const {
        return _waitForDelete.is_initialized();
    }
    bool getWaitForDelete() const {
        return _waitForDelete.get();
    }
    void setWaitForDelete(const bool waitForDelete);

private:
    /**
     * Used to parse balancing 'activeWindow'.
     * See '_balancerActiveWindow' member variable doc comments below.
     */
    StatusWith<BoostTimePair> _parseBalancingWindow(const BSONObj& balancingWindowObj);

    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M)  key determining the type of options to use
    boost::optional<std::string> _key;

    // (S)  size of the chunks in our cluster in MB
    //      Required if key is chunkSize
    boost::optional<long long> _chunkSizeMB;

    // (O)  is balancer enabled or disabled (default)
    //      Defaults to false.
    boost::optional<bool> _balancerStopped;

    // (O)  if present, balancerActiveWindow is an interval during the day
    //      when the balancer should be active.
    //      Stored as (<start time>, <stop time>) pair. strftime format is %H:%M
    boost::optional<BoostTimePair> _balancerActiveWindow;

    // (O)  only migrate chunks as fast as at least one secondary can keep up with
    boost::optional<bool> _secondaryThrottle;

    // (O)  detailed write concern for *individual* writes during migration.
    // From side: deletes during cleanup.
    // To side: deletes to clear the incoming range, deletes to undo migration at abort,
    //          and writes during cloning.
    boost::optional<WriteConcernOptions> _migrationWriteConcern;

    // (O)  synchronous migration cleanup.
    boost::optional<bool> _waitForDelete;
};

}  // namespace mongo
