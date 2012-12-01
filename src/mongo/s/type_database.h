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
     * config.database collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(DatabaseType::name("mydb"));
     *     dbDoc = conn->findOne(DatbaseType::ConfigNS, query);
     *
     *     // Process the response.
     *     DatabaseType db;
     *     string errMsg;
     *     if (!db.parseBSON(dbDoc, &errMsg) || !db.isValid(&errMsg)) {
     *         // Can't use 'db'. Take action.
     *     }
     *     // use 'db'
     *
     */
    class DatabaseType {
        MONGO_DISALLOW_COPYING(DatabaseType);
    public:

        //
        // schema declarations
        //

        // Name of the database collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the database collection type.
        static BSONField<std::string> name;     // database's name
        static BSONField<std::string> primary;  // primary shard for the database
        static BSONField<bool> draining;        // is the database being removed?

        // This field was last used in 2.2 series (version 3).
        static BSONField<bool> DEPRECATED_partitioned;

        // These fields were last used in 1.4 series (version 2).
        static BSONField<std::string> DEPRECATED_name;
        static BSONField<bool> DEPRECATED_sharded;

        //
        // database type methods
        //

        DatabaseType();
        ~DatabaseType();

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
        bool parseBSON(BSONObj source, std::string* errMsg);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(DatabaseType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setName(const StringData& name) { _name = name.toString(); }
        const std::string& getName() const { return _name; }

        void setPrimary(const StringData& shard) { _primary = shard.toString(); }
        const std::string& getPrimary() const { return _primary; }

        void setDraining(bool draining) { _draining = draining; }
        bool getDraining() const { return _draining; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        string _name;     // (M) database name
        string _primary;  // (M) primary shard for the database
        bool _draining;   // (O) is this database about to be deleted?
    };

} // namespace mongo
