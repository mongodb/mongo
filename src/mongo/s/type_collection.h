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
     * config.collections collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(CollectionType::ns("db.coll") <<
     *                           CollectionType::unique(true));
     *     collDoc = conn->findOne(CollectionType::ConfigNS, query);
     *
     *     // Process the response.
     *     CollectionType coll;
     *     coll.fromBSON(collDoc);
     *     if (! coll.isValid()) {
     *         // Can't use 'coll'. Take action.
     *     }
     *     if (coll.isDropped()) {
     *         // Coll doesn't exist, Take action.
     *     }
     *
     *     // use 'coll'
     *
     */
    class CollectionType {
        MONGO_DISALLOW_COPYING(CollectionType);
    public:

        //
        // schema declarations
        //

        // Name of the collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the collection type.
        static BSONField<std::string> ns;      // collection's namespace
        static BSONField<std::string> primary; // primary db when not sharded
        static BSONField<BSONObj> keyPattern;  // sharding key, if sharded
        static BSONField<bool> unique;         // sharding key unique?
        static BSONField<Date_t> updatedAt;    // when collection was created
        static BSONField<bool> noBalance;      // true if balancing is disabled
        static BSONField<OID> epoch;           // disambiguate ns (drop/recreate)
        // To-be-deprecated, not yet
        static BSONField<bool> dropped;        // true if we should ignore this collection entry

        // Deprecated fields should only be used in parseBSON calls. Exposed here for testing only.
        static BSONField<OID> DEPRECATED_lastmodEpoch;
        static BSONField<Date_t> DEPRECATED_lastmod;

        //
        // collection type methods
        //

        CollectionType();
        ~CollectionType();

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
        void cloneTo(CollectionType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setNS(const StringData& ns) { _ns = ns.toString(); }
        const std::string& getNS() const { return _ns; }

        void setPrimary(const StringData& name) { _primary = name.toString(); }
        const std::string& getPrimary() const { return _primary; }

        void setKeyPattern(const BSONObj keyPattern) { _keyPattern = keyPattern.getOwned(); }
        BSONObj getKeyPattern() const { return _keyPattern; }

        void setUnique(bool unique) { _unique = unique; }
        bool isUnique() const { return _unique; }

        void setUpdatedAt(const Date_t& time) { _updatedAt = time; }
        Date_t getUpdatedAt() const { return _updatedAt; }

        void setNoBalance(bool noBalance) { _noBalance = noBalance; }
        bool getNoBalance() const { return _noBalance; }

        void setEpoch(OID oid) { _epoch = oid; }
        OID getEpoch() const { return _epoch; }

        void setDropped(bool dropped) { _dropped = dropped; }
        bool isDropped() const { return _dropped; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _ns;       // (M) namespace
        std::string _primary;  // (S) either/or with _keyPattern
        BSONObj _keyPattern;   // (S) sharding pattern if sharded
        bool _unique;          // (S) mandatory if sharded, index is unique
        Date_t _updatedAt;     // (M) last updated time
        bool _noBalance;       // (S) optional if sharded, disable balancing
        OID _epoch;            // (M) disambiguates collection incarnations
        bool _dropped;         // (O) if true, ignore this entry
    };

} // namespace mongo
