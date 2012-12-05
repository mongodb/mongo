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
     * config.changelog collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(ChangelogType::changeID("host.local-2012-11-21T19:14:10-8"))
     *     logEntryDoc = conn->findOne(ChangelogType::ConfigNS, query);
     *
     *     // Process the response.
     *     ChangelogType logEntry;
     *     logEntry.fromBSON(logEntryDoc);
     *     if (! logEntry.isValid()) {
     *         // Can't use 'logEntry'. Take action.
     *     }
     *     // use 'logEntry'
     *
     */
    class ChangelogType {
        MONGO_DISALLOW_COPYING(ChangelogType);
    public:

        //
        // schema declarations
        //

        // Name of the collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the changelog type.
        static BSONField<std::string> changeID;     // id for this change "<hostname>-<current_time>-<increment>"
        static BSONField<std::string> server;       // hostname of server that we are making the change on.  Does not include port.
        static BSONField<std::string> clientAddr;   // hostname:port of the client that made this change
        static BSONField<Date_t> time;              // time this change was made
        static BSONField<std::string> what;         // description of the change
        static BSONField<std::string> ns;           // database or collection this change applies to
        static BSONField<BSONObj> details;          // A BSONObj containing extra information about some operations

        //
        // changelog type methods
        //

        ChangelogType();
        ~ChangelogType();

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
         * latter contains valid values. Otherwise clear the internal state.
         */
        void parseBSON(BSONObj source);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(ChangelogType* other);

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setChangeID(const StringData& changeID) { _changeID = changeID.toString(); }
        const std::string& getChangeID() const { return _changeID; }

        void setServer(const StringData& server) { _server = server.toString(); }
        const std::string& getServer() const { return _server; }

        void setClientAddr(const StringData& clientAddr) { _clientAddr = clientAddr.toString(); }
        const std::string& getClientAddr() const { return _clientAddr; }

        void setTime(const Date_t& time) { _time = time; }
        Date_t getTime() const { return _time; }

        void setWhat(const StringData& what) { _what = what.toString(); }
        const std::string& getWhat() const { return _what; }

        void setNS(const StringData& ns) { _ns = ns.toString(); }
        const std::string& getNS() const { return _ns; }

        void setDetails(const BSONObj details) { _details = details.getOwned(); }
        BSONObj getDetails() const { return _details; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _changeID;   // (M) id for this change "<hostname>-<current_time>-<increment>"
        std::string _server;     // (M) hostname of server that we are making the change on.  Does not include port.
        std::string _clientAddr; // (M) hostname:port of the client that made this change
        Date_t _time;            // (M) time this change was made
        std::string _what;       // (M) description of the change
        std::string _ns;         // (M) database or collection this change applies to
        BSONObj _details;        // (M) A BSONObj containing extra information about some operations
    };

} // namespace mongo
