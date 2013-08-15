/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * "Types" are the interface to a known data structure that will be serialized to and
     * deserialized from BSON.
     */
    class BSONSerializable {
    public:

        virtual ~BSONSerializable() {}

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returns false and fills in the optional 'errMsg' string.
         */
        virtual bool isValid( std::string* errMsg ) const = 0;

        /** Returns the BSON representation of the entry. */
        virtual BSONObj toBSON() const = 0;

        /**
         * Clears and populates the internal state using the 'source' BSON object if the
         * latter contains valid values. Otherwise sets errMsg and returns false.
         */
        virtual bool parseBSON( const BSONObj& source, std::string* errMsg ) = 0;

        /** Clears the internal state. */
        virtual void clear() = 0;

        /** Returns a string representation of the current internal state. */
        virtual std::string toString() const = 0;
    };

} // namespace mongo
