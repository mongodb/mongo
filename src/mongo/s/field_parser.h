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

#include "mongo/bson/bson_field.h"
#include "mongo/bson/util/misc.h" // for Date_t
#include "mongo/db/jsobj.h"

namespace mongo {

    class FieldParser {
    public:
        /**
         * Returns true and fills in 'out' with the contents of the field described by 'field'
         * or with the value in 'def', depending on whether the field is present and has the
         * correct type in 'doc' or not, respectively. Otherwise, if the field exists but has
         * the wrong type, returns false.
         *
         * NOTE ON BSON OWNERSHIP:
         *
         *   The caller must assume that this class will point to data inside 'doc' without
         *   copying it. In practice this means that 'doc' MUST EXIST for as long as 'out'
         *   stays in scope.
         */
        static bool extract(BSONObj doc,
                            const BSONField<bool>& field,
                            bool def,
                            bool* out);

        static bool extract(BSONObj doc,
                            const BSONField<BSONArray>& field,
                            const BSONArray& def,
                            BSONArray* out);

        static bool extract(BSONObj doc,
                            const BSONField<BSONObj>& field,
                            const BSONObj& def,
                            BSONObj* out);

        static bool extract(BSONObj doc,
                            const BSONField<Date_t>& field,
                            const Date_t def,
                            Date_t* out);

        static bool extract(BSONObj doc,
                            const BSONField<string>& field,
                            const string& def,
                            string* out);

        static bool extract(BSONObj doc,
                            const BSONField<OID>& field,
                            const OID& def,
                            OID* out);
    };

} // namespace mongo
