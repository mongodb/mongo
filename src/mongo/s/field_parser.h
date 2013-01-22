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

        enum FieldState {
            // The field is present but has the wrong type
            FIELD_INVALID = 0,

            // The field is present and has the correct type
            FIELD_SET,

            // The field is absent in the BSON object but set from default
            FIELD_DEFAULT,

            // The field is absent and no default was specified
            FIELD_NONE
        };

        static FieldState extract(BSONObj doc,
                            const BSONField<bool>& field,
                            bool def,
                            bool* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<BSONArray>& field,
                            const BSONArray& def,
                            BSONArray* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<BSONObj>& field,
                            const BSONObj& def,
                            BSONObj* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<Date_t>& field,
                            const Date_t def,
                            Date_t* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<string>& field,
                            const string& def,
                            string* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<OID>& field,
                            const OID& def,
                            OID* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<int>& field,
                            const int& def,
                            int* out,
                            string* errMsg = NULL);

        static FieldState extract(BSONObj doc,
                            const BSONField<long long>& field,
                            const long long& def,
                            long long* out,
                            string* errMsg = NULL);

        /**
         * The following extractNumber methods do implicit conversion between any numeric type and
         * the BSONField type.  This can be useful when an exact numeric type is not needed, for
         * example if the field is sometimes modified from the shell which can change the type.
         */
        static FieldState extractNumber(BSONObj doc,
                                  const BSONField<int>& field,
                                  const int& def,
                                  int* out,
                                  string* errMsg = NULL);

        static FieldState extractNumber(BSONObj doc,
                                  const BSONField<long long>& field,
                                  const long long& def,
                                  long long* out,
                                  string* errMsg = NULL);

        /**
         * The following extract methods are templatized to handle extraction of vectors and
         * maps of sub-objects.  Keys in the map should be StringData compatible.
         *
         * It's possible to nest extraction of vectors and maps to any depth, i.e:
         *
         * vector<map<string,vector<string> > > val;
         * FieldParser::extract(doc, field, val, &val);
         */
        template<typename T>
        static FieldState extract(BSONObj doc,
                            const BSONField<vector<T> >& field,
                            const vector<T>& def,
                            vector<T>* out,
                            string* errMsg = NULL);

        template<typename K, typename T>
        static FieldState extract(BSONObj doc,
                            const BSONField<map<K, T> >& field,
                            const map<K, T>& def,
                            map<K, T>* out,
                            string* errMsg = NULL);
    };

} // namespace mongo

// Inline functions for templating
#include "field_parser-inl.h"

