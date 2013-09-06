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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This class represents the layout and contents of documents contained in the
     * config.version collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(VersionType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(VersionType::ConfigNS, query);
     *
     *     // Process the response.
     *     VersionType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class VersionType {
        MONGO_DISALLOW_COPYING(VersionType);
    public:

        //
        // schema declarations
        //

        // Name of the version collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the version collection type.
        static const BSONField<int> minCompatibleVersion;
        static const BSONField<int> currentVersion;
        static const BSONField<BSONArray> excludingMongoVersions;
        static const BSONField<OID> clusterId;
        static const BSONField<int> version_DEPRECATED;
        static const BSONField<OID> upgradeId;
        static const BSONField<BSONObj> upgradeState;

        //
        // version type methods
        //

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
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(VersionType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setMinCompatibleVersion(const int minCompatibleVersion) {
            _minCompatibleVersion = minCompatibleVersion;
            _isMinCompatibleVersionSet = true;
        }

        void unsetMinCompatibleVersion() { _isMinCompatibleVersionSet = false; }

        bool isMinCompatibleVersionSet() const { return _isMinCompatibleVersionSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        int getMinCompatibleVersion() const {
            dassert(_isMinCompatibleVersionSet);
            return _minCompatibleVersion;
        }

        void setCurrentVersion(const int currentVersion) {
            _currentVersion = currentVersion;
            _isCurrentVersionSet = true;
        }

        void unsetCurrentVersion() { _isCurrentVersionSet = false; }

        bool isCurrentVersionSet() const { return _isCurrentVersionSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        int getCurrentVersion() const {
            dassert(_isCurrentVersionSet);
            return _currentVersion;
        }

        void setExcludingMongoVersions(const BSONArray& excludingMongoVersions) {
            _excludingMongoVersions = excludingMongoVersions;
            _isExcludingMongoVersionsSet = true;
        }

        void unsetExcludingMongoVersions() { _isExcludingMongoVersionsSet = false; }

        bool isExcludingMongoVersionsSet() const {
            return _isExcludingMongoVersionsSet || excludingMongoVersions.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        const BSONArray getExcludingMongoVersions() const {
            if (_isExcludingMongoVersionsSet) {
                return _excludingMongoVersions;
            } else {
                dassert(excludingMongoVersions.hasDefault());
                return excludingMongoVersions.getDefault();
            }
        }

        void setClusterId(const OID clusterId) {
            _clusterId = clusterId;
            _isClusterIdSet = true;
        }

        void unsetClusterId() { _isClusterIdSet = false; }

        bool isClusterIdSet() const { return _isClusterIdSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const OID getClusterId() const {
            dassert(_isClusterIdSet);
            return _clusterId;
        }

        // Optional Fields
        void setUpgradeId(OID upgradeId) {
            _upgradeId = upgradeId;
            _isUpgradeIdSet = true;
        }

        void unsetUpgradeId() { _isUpgradeIdSet = false; }

        bool isUpgradeIdSet() const {
            return _isUpgradeIdSet || upgradeId.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        OID getUpgradeId() const {
            if (_isUpgradeIdSet) {
                return _upgradeId;
            } else {
                dassert(upgradeId.hasDefault());
                return upgradeId.getDefault();
            }
        }
        void setUpgradeState(const BSONObj& upgradeState) {
            _upgradeState = upgradeState.getOwned();
            _isUpgradeStateSet = true;
        }

        void unsetUpgradeState() { _isUpgradeStateSet = false; }

        bool isUpgradeStateSet() const {
            return _isUpgradeStateSet || upgradeState.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        BSONObj getUpgradeState() const {
            if (_isUpgradeStateSet) {
                return _upgradeState;
            } else {
                dassert(upgradeState.hasDefault());
                return upgradeState.getDefault();
            }
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        int _minCompatibleVersion;     // (M)  minimum compatible version
        bool _isMinCompatibleVersionSet;
        int _currentVersion;     // (M)  current version
        bool _isCurrentVersionSet;
        BSONArray _excludingMongoVersions;     // (O)  range of disallowed versions to upgrade to
        bool _isExcludingMongoVersionsSet;
        OID _clusterId;     // (M)  clusterId
        bool _isClusterIdSet;
        OID _upgradeId;     // (O)  upgrade id of current or last upgrade
        bool _isUpgradeIdSet;
        BSONObj _upgradeState;     // (O)  upgrade state of current or last upgrade
        bool _isUpgradeStateSet;
    };

} // namespace mongo
