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
        static const BSONField<int> minVersion;
        static const BSONField<int> maxVersion;
        static const BSONField<OID> clusterId;

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

        int getMaxCompatibleVersion() const;
        void setMaxCompatibleVersion(int version);

        bool isCompatibleVersion(int version) const;

        // If there is no version document but other config collections exist,
        // this is the default version we use
        void setDefaultVersion();

        // If there is no data in the config server, this is the default version we use
        void setEmptyVersion();

        bool isEmptyVersion() const;

        // Checks whether two versions are equivalent
        bool isEquivalentTo(const VersionType& other) const;

    private:

        int _minVersion;
        int _maxVersion;
        OID _clusterId;
    };

} // namespace mongo
