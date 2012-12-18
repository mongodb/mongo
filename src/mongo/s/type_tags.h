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
     * config.tags collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     unique_ptr<DbClientCursor> cursor;
     *     BSONObj query = QUERY(TagsType::ns("mydb.mycoll"));
     *     cursor.reset(conn->query(TagsType::ConfigNS, query, ...));
     *
     *     // Process the response.
     *     while (cursor->more()) {
     *         tagDoc = cursor->next();
     *         TagsType tag;
     *         tag.fromBSON(dbDoc);
     *         if (! tag.isValid()) {
     *             // Can't use 'tag'. Take action.
     *         }
     *         // use 'tag'
     *     }
     */
    class TagsType {
        MONGO_DISALLOW_COPYING(TagsType);
    public:

        //
        // schema declarations
        //

        // Name of the tags collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the tags collection type.
        static BSONField<std::string> ns;  // namespace this tag is for
        static BSONField<std::string> tag; // tag name
        static BSONField<BSONObj> min;     // first key of the tag, including
        static BSONField<BSONObj> max;     // last key of the tag, non-including

        //
        // tags type methods
        //

        TagsType();
        ~TagsType();

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
        void cloneTo(TagsType* other);

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setNS(const StringData& ns) { _ns = ns.toString(); }
        const std::string& getNS() const { return _ns; }

        void setTag(const StringData& tag) { _tag = tag.toString(); }
        const std::string& getTag() const { return _tag; }

        void setMin(const BSONObj& min) { _min = min.getOwned(); }
        BSONObj getMin() const { return _min; }

        void setMax(const BSONObj& max) { _max = max.getOwned(); }
        BSONObj getMax() const { return _max; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        string _ns;   // (M) namespace this tag is for
        string _tag;  // (M) tag name
        BSONObj _min; // (M) first key of the tag, including
        BSONObj _max; // (M) last key of the tag, non-including
    };

}  // namespace mongo
