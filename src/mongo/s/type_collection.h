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
     *     BSONObj query = QUERY(CollectionInfo::ns("mynamspace") <<
     *                           CollectionInfo::unique(true));
     *     collDoc = conn->findOne(query);
     *     CollectionType coll;
     *     coll.fromBSON(collDoc);
     *     if (! coll.isValid()) {
     *         /// take action
     *     }
     *     // use coll
     *
     */
    class CollectionType {
        MONGO_DISALLOW_COPYING(CollectionType);
    public:

        //
        // schema declarations
        //

        // Name of the collection in the config server.
        static const string ConfigNS;

        // Field names and types in the collection type.
        static BSONField<string> ns;          // collection's namespace
        static BSONField<string> primary;     // primary db when not sharded
        static BSONField<BSONObj> keyPattern; // sharding key, if sharded
        static BSONField<bool> unique;        // sharding key unique?
        static BSONField<Date_t> createdAt;   // when collecation was created
        static BSONField<bool> noBalance;     // true if balancing is disabled
        static BSONField<OID> epoch;          // disambiguate ns (drop/recreate)

        // Deprecated fields should only be used in parseBSON calls. Exposed here for testing only.
        static BSONField<OID> DEPRECATED_lastmodEpoch;
        static BSONField<Date_t> DEPRECATED_lastmod;
        static BSONField<bool> DEPRECATED_dropped;

        //
        // collection type methods
        //

        CollectionType();
        ~CollectionType();

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returs false and fills in the optional 'errMsg' string.
         */
        bool isValid(string* errMsg) const;

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
        void cloneTo(CollectionType* other);

        /**
         * Returns a string representation of the current internal state.
         */
        string toString() const;

        //
        // individual field accessors
        //

        void setNS(const StringData& ns) { _ns = string(ns.data(), ns.size()); }
        const string& getNS() const { return _ns; }
        void setPrimary(const StringData& name) { _primary = string(name.data(), name.size()); }
        const string& getPrimary() const { return _primary; }
        void setKeyPattern(const BSONObj keyPattern) { _keyPattern = keyPattern.getOwned(); }
        BSONObj getKeyPattern() const { return _keyPattern; }
        void setUnique(bool unique) { _unique = unique; }
        bool getUnique() const { return _unique; }
        void setCreatedAt(const Date_t& time) { _createdAt = time; }
        Date_t getCreatedAt() const { return _createdAt; }
        void setNoBalance(bool noBalance) { _noBalance = noBalance; }
        bool getNoBalance() const { return _noBalance; }
        void setEpoch(OID oid) { _epoch = oid; }
        OID getEpoch() const { return _epoch; }

    private:
        string _ns;            // mandatory namespace
        string _primary;       // either or with _keyPattern
        BSONObj _keyPattern;   // sharding parttern if sharded
        bool _unique;          // not optional if sharded, index is unique
        Date_t _createdAt;     // mandatory creation time
        bool _noBalance;       // optional, if sharded, disable balancing
        OID _epoch;            // mandatory, to disambiguate collection incarnations

    };

} // namespace mongo
