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
     * This class represents the layout and contents of documents contained in the
     * config.mongos collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     unique_ptr<DbClientCursor> cursor;
     *     BSONObj query = QUERY(MongosType::name("localhost:27017"));
     *     cursor.reset(conn->query(MongosType::ConfigNS, query, ...));
     *
     *     // Process the response.
     *     while (cursor->more()) {
     *         mongosDoc = cursor->next();
     *         MongosType mongos;
     *         mongos.fromBSON(mongosDoc);
     *         if (! mongos.isValid()) {
     *             // Can't use 'mongos'. Take action.
     *         }
     *         // use 'mongos'
     *     }
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
        static BSONField<string> name;      // "host:port" for this mongos
        static BSONField<Date_t> ping;      // last time it was seen alive
        static BSONField<int> up;           // uptime at the last ping
        static BSONField<bool> waiting;     // for testing purposes
        static BSONField<string> mongoVersion; // version of mongos
        static BSONField<int> configVersion; // config version of mongos

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
        bool parseBSON(BSONObj source, std::string* errMsg);

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

        void setName(const StringData& name) { _name = name.toString(); }
        const std::string& getName() const { return _name; }

        void setPing(const Date_t& time) { _ping = time; }
        Date_t getPing() const { return _ping; }

        void setUp(int up) { _up = up; }
        int getUp() const { return _up; }

        void setWaiting(bool waiting) { _waiting = waiting; }
        bool getWaiting() const { return _waiting; }

        void setMongoVersion(const std::string& mongoVersion) { _mongoVersion = mongoVersion; }
        const std::string getMongoVersion() const { return _mongoVersion; }

        void setConfigVersion(int configVersion) { _configVersion = configVersion; }
        int getConfigVersion() const { return _configVersion; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        string _name;                // (M) "host:port" for this mongos
        Date_t _ping;                // (M) last time it was seen alive
        int _up;                     // (M) uptime at the last ping
        bool _waiting;               // (M) for testing purposes
        string _mongoVersion;        // (M) the mongodb version of the pinging mongos
        int _configVersion;          // (M) the config version of the pinging mongos
    };

}  // namespace mongo
