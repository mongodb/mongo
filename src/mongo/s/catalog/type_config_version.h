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
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/mongo_version_range.h"

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the
 * config.version collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class VersionType {
public:
    // Name of the version collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the version collection type.
    static const BSONField<int> minCompatibleVersion;
    static const BSONField<int> currentVersion;
    static const BSONField<BSONArray> excludingMongoVersions;
    static const BSONField<OID> clusterId;
    static const BSONField<OID> upgradeId;
    static const BSONField<BSONObj> upgradeState;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Clears the internal state.
     */
    void clear();

    /**
     * Copies all the fields present in 'this' to 'other'.
     */
    void cloneTo(VersionType* other) const;

    /**
     * Constructs a new ChangeLogType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<VersionType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    int getMinCompatibleVersion() const {
        return _minCompatibleVersion.get();
    }
    void setMinCompatibleVersion(const int minCompatibleVersion);

    int getCurrentVersion() const {
        return _currentVersion.get();
    }
    void setCurrentVersion(const int currentVersion);

    const OID& getClusterId() const {
        return _clusterId.get();
    }
    bool isClusterIdSet() const {
        return _clusterId.is_initialized();
    }
    void setClusterId(const OID& clusterId);

    const std::vector<MongoVersionRange> getExcludingMongoVersions() const {
        if (!isExcludingMongoVersionsSet()) {
            return std::vector<MongoVersionRange>();
        }
        return _excludingMongoVersions.get();
    }
    bool isExcludingMongoVersionsSet() const {
        return _excludingMongoVersions.is_initialized();
    }
    void setExcludingMongoVersions(const std::vector<MongoVersionRange>& excludingMongoVersions);

    const OID& getUpgradeId() const {
        return _upgradeId.get();
    }
    bool isUpgradeIdSet() const {
        return _upgradeId.is_initialized();
    }
    void setUpgradeId(const OID& upgradeId);

    const BSONObj& getUpgradeState() const {
        return _upgradeState.get();
    }
    bool isUpgradeStateSet() const {
        return _upgradeState.is_initialized();
    }
    void setUpgradeState(const BSONObj& upgradeState);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) minimum compatible version
    boost::optional<int> _minCompatibleVersion;
    // (M) current version
    boost::optional<int> _currentVersion;
    // (S) clusterId -- required if current version > UpgradeHistory::UpgradeHistory_NoEpochVersion
    boost::optional<OID> _clusterId;
    // (O) range of disallowed versions to upgrade to
    boost::optional<std::vector<MongoVersionRange>> _excludingMongoVersions;
    // (O) upgrade id of current or last upgrade
    boost::optional<OID> _upgradeId;
    // (O)  upgrade state of current or last upgrade
    boost::optional<BSONObj> _upgradeState;
};

}  // namespace mongo
