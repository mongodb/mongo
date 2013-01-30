// fts_index_format.h

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

#include "mongo/db/fts/fts_spec.h"

namespace mongo {

    namespace fts {

        class FTSIndexFormat {
        public:

            static void getKeys( const FTSSpec& spec,
                                 const BSONObj& document,
                                 BSONObjSet* keys );

            /*
             * Helper method to get return entry from the FTSIndex as a BSONObj
             * @param weight, the weight of the term in the entry
             * @param term, the string term in the entry
             * @param indexPrefix, the fields that go in the index first
             */
            static BSONObj getIndexKey( double weight,
                                        const string& term,
                                        const BSONObj& indexPrefix );

        private:
            /*
             * Helper method to get return entry from the FTSIndex as a BSONObj
             * @param b, reference to the BSONOBjBuilder
             * @param weight, the weight of the term in the entry
             * @param term, the string term in the entry
             */
            static void _appendIndexKey( BSONObjBuilder& b, double weight, const string& term );
        };

    }
}
