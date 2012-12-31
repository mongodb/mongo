// fts_util.h

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

#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/record.h"
#include "mongo/util/unordered_fast_key_table.h"

namespace mongo {

    namespace fts {

        extern const std::string WILDCARD;
        extern const std::string INDEX_NAME;

        /**
         * destructive!
         */
        inline void makeLower( std::string* s ) {
            std::string::size_type sz = s->size();
            for ( std::string::size_type i = 0; i < sz; i++ )
                (*s)[i] = (char)tolower( (int)(*s)[i] );
        }

        /*
         * ScoredLocation stores the total score for a document (record *) wrt a search
         *
         */
        struct ScoredLocation {
            ScoredLocation( Record* r, double sc )
                : rec(r), score(sc) {
            }

            Record* rec;
            double score;

            bool operator<( const ScoredLocation& other ) const {
                if ( other.score < score )
                    return true;
                if ( other.score > score )
                    return false;
                return rec < other.rec;
            }
        };

        // scored location comparison is done based on score
        class ScoredLocationComp {
        public:
            bool operator() (const ScoredLocation& lhs, const ScoredLocation& rhs) const {
                return (lhs.score > rhs.score);
            }
        };

        struct _be_hash {
            size_t operator()( const BSONElement& e ) const {
                return static_cast<size_t>( BSONElementHasher::hash64( e, 17 ) );
            }
        };

        struct _be_equals {
            bool operator()( const BSONElement& a, const BSONElement& b ) const {
                return a == b;
            }
        };

        struct _be_convert {
            BSONElement operator()( const BSONObj& o ) const {
                const BSONElement& x = o.firstElement();
                BSONElement y( x.rawdata() );
                return y;
            }
        };

        struct _be_convert_other {
            BSONObj operator()( const BSONElement& e ) const {
                return e.wrap();
            }
        };

        template< typename V >
        class BSONElementMap : public UnorderedFastKeyTable<BSONElement,
                                                            BSONObj,
                                                            V,
                                                            _be_hash,
                                                            _be_equals,
                                                            _be_convert,
                                                            _be_convert_other > {
        };


    }
}

