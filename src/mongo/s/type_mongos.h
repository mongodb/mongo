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
     * config.mongos collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(MongosType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(MongosType::ConfigNS, query);
     *
     *     // Process the response.
     *     MongosType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class MongosType {
        MONGO_DISALLOW_COPYING(MongosType);
    public:

        //
        // schema declarations
        //

        // Name of the mongos collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the mongos collection type.
        static const BSONField<std::string> name;
        static const BSONField<Date_t> ping;
        static const BSONField<int> up;
        static const BSONField<bool> waiting;
        static const BSONField<std::string> mongoVersion;
        static const BSONField<int> configVersion;

        //
        // mongos type methods
        //

        MongosType();
        ~MongosType();

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
        void cloneTo(MongosType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setName(const StringData& name) {
            _name = name.toString();
            _isNameSet = true;
        }

        void unsetName() { _isNameSet = false; }

        bool isNameSet() const { return _isNameSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getName() const {
            dassert(_isNameSet);
            return _name;
        }

        void setPing(const Date_t ping) {
            _ping = ping;
            _isPingSet = true;
        }

        void unsetPing() { _isPingSet = false; }

        bool isPingSet() const { return _isPingSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const Date_t getPing() const {
            dassert(_isPingSet);
            return _ping;
        }

        void setUp(const int up) {
            _up = up;
            _isUpSet = true;
        }

        void unsetUp() { _isUpSet = false; }

        bool isUpSet() const { return _isUpSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        int getUp() const {
            dassert(_isUpSet);
            return _up;
        }

        void setWaiting(const bool waiting) {
            _waiting = waiting;
            _isWaitingSet = true;
        }

        void unsetWaiting() { _isWaitingSet = false; }

        bool isWaitingSet() const { return _isWaitingSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        bool getWaiting() const {
            dassert(_isWaitingSet);
            return _waiting;
        }

        // Optional Fields
        void setMongoVersion(const StringData& mongoVersion) {
            _mongoVersion = mongoVersion.toString();
            _isMongoVersionSet = true;
        }

        void unsetMongoVersion() { _isMongoVersionSet = false; }

        bool isMongoVersionSet() const {
            return _isMongoVersionSet || mongoVersion.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        std::string getMongoVersion() const {
            if (_isMongoVersionSet) {
                return _mongoVersion;
            } else {
                dassert(mongoVersion.hasDefault());
                return mongoVersion.getDefault();
            }
        }
        void setConfigVersion(const int configVersion) {
            _configVersion = configVersion;
            _isConfigVersionSet = true;
        }

        void unsetConfigVersion() { _isConfigVersionSet = false; }

        bool isConfigVersionSet() const {
            return _isConfigVersionSet || configVersion.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        int getConfigVersion() const {
            if (_isConfigVersionSet) {
                return _configVersion;
            } else {
                dassert(configVersion.hasDefault());
                return configVersion.getDefault();
            }
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _name;     // (M)  "host:port" for this mongos
        bool _isNameSet;
        Date_t _ping;     // (M)  last time it was seen alive
        bool _isPingSet;
        int _up;     // (M)  uptime at the last ping
        bool _isUpSet;
        bool _waiting;     // (M)  for testing purposes
        bool _isWaitingSet;
        std::string _mongoVersion;     // (O)  the mongodb version of the pinging mongos
        bool _isMongoVersionSet;
        int _configVersion;     // (O)  the config version of the pinging mongos
        bool _isConfigVersionSet;
    };

} // namespace mongo
