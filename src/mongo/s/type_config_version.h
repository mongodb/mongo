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
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This class represents the layout and contents of the single document contained in the
     * config.version collection. All manipulation of this document should be done using this class.
     *
     * Usage Example:
     *
     *     DBClientBase* conn;
     *     BSONObj query = QUERY();
     *     versionDoc = conn->findOne(VersionType::ConfigNS, query);
     *
     *     // Process the response.
     *     VersionType versionInfo;
     *     if (!versionInfo.parseBSON(versionDoc)) {
     *         // Can't parse document, take action
     *     }
     *     if (!versionInfo.isValid()) {
     *         // Can't use version, take action.
     *     }
     *     // use 'versionInfo'
     *
     */
    class VersionType {
        MONGO_DISALLOW_COPYING(VersionType);
    public:

        // Name of the versions collection in the config schema
        static const string ConfigNS;

        // Field names and types of the version document
        static const BSONField<int> minCompatibleVersion;
        static const BSONField<int> currentVersion;
        static const BSONField<BSONArray> excludingMongoVersions;
        static const BSONField<OID> clusterId;
        static const BSONField<OID> upgradeId;
        static const BSONField<BSONObj> upgradeState;

        // Transition to new format v2.2->v2.4
        // We eventually will not use version, minVersion and maxVersion instead
        static const BSONField<int> version_DEPRECATED;

        VersionType();
        ~VersionType();

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returns false and fills in the optional 'errMsg' string.
         */
        bool isValid(std::string* errMsg) const;

        /**
         * Returns the BSON representation of the entry.
         */
        BSONObj toBSON() const;

        /**
         * Clears and populates the internal state using the 'source' BSON object if the
         * latter contains valid values. Otherwise sets errMsg and returns false.
         */
        bool parseBSON(const BSONObj& source, std::string* errMsg);

        /**
         * Clones to another version entry
         */
        void cloneTo(VersionType* other) const;

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors and helpers
        //

        OID getClusterId() const;
        void setClusterId(const OID& clusterId);

        int getMinCompatibleVersion() const;
        void setMinCompatibleVersion(int version);

        int getCurrentVersion() const;
        void setCurrentVersion(int version);

        //
        // Range exclusions
        //
        // The type knows very little about these ranges, mostly because it's impossible right now
        // to do generic BSON parsing of more complex types without template magick.  All
        // interpretation of versions happens in the upgrade code.
        //

        const BSONArray& getExcludedRanges() const { return _excludes; }
        void setExcludedRanges(const BSONArray& excludes) { _excludes = excludes; }

        OID getUpgradeId() const;
        void setUpgradeId(const OID& upgradeId);

        BSONObj getUpgradeState() const;
        void setUpgradeState(const BSONObj& upgradeState);

    private:

        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        int _minVersion; // (M) minimum compatible version
        int _currentVersion; // (M) current version
        OID _clusterId; // (M) clusterId
        BSONArray _excludes; // (O) mongodb versions excluded from the cluster
        OID _upgradeId; // (O) upgrade id of current or last upgrade
        BSONObj _upgradeState; // (S) upgrade state of current or last upgrade
    };

} // namespace mongo
