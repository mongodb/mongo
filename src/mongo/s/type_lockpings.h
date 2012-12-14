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
     * config.lockpings collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     unique_ptr<DbClientCursor> cursor;
     *     BSONObj query = QUERY(LockpingsType::name("localhost:27017"));
     *     cursor.reset(conn->query(LockpingsType::ConfigNS, query, ...));
     *
     *     // Process the response.
     *     while (cursor->more()) {
     *         lockPingDoc = cursor->next();
     *         LockpingsType lockPing;
     *         lockPing.fromBSON(lockPingDoc);
     *         if (! lockPing.isValid()) {
     *             // Can't use 'lockPing'. Take action.
     *         }
     *         // use 'lockPing'
     *     }
     */

    class LockpingsType {
        MONGO_DISALLOW_COPYING(LockpingsType);
    public:

        //
        // schema declarations
        //

        // Name of the lockpings collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the lockpings collection type.
        static BSONField<string> process; // string describing the process holding the lock
        static BSONField<Date_t> ping;    // last time the holding process updated this document

        //
        // lockpings type methods
        //

        LockpingsType();
        ~LockpingsType();

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
        void cloneTo(LockpingsType* other);

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setProcess(const StringData& process) { _process = process.toString(); }
        const std::string& getProcess() const { return _process; }

        void setPing(const Date_t& time) { _ping = time; }
        Date_t getPing() const { return _ping; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _process; // (M) string describing the process holding the lock
        Date_t _ping;         // (M) last time the holding process updated this document
    };

}  // namespace mongo
