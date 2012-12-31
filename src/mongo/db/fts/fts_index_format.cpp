// fts_index_format.cpp

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

#include "mongo/pch.h"

#include "mongo/base/init.h"
#include "mongo/db/fts/fts_index_format.h"

namespace mongo {

    namespace fts {

        namespace {
            BSONObj nullObj;
            BSONElement nullElt;
        }

        MONGO_INITIALIZER( FTSIndexFormat )( InitializerContext* context ) {
            BSONObjBuilder b;
            b.appendNull( "" );
            nullObj = b.obj();
            nullElt = nullObj.firstElement();
            return Status::OK();
        }

        void FTSIndexFormat::getKeys( const FTSSpec& spec,
                                      const BSONObj& obj,
                                      BSONObjSet* keys ) {

            int extraSize = 0;
            vector<BSONElement> extrasBefore;
            vector<BSONElement> extrasAfter;

            // compute the non FTS key elements
            for ( unsigned i = 0; i < spec.numExtraBefore(); i++ ) {
                BSONElement e = obj.getFieldDotted(spec.extraBefore(i));
                if ( e.eoo() )
                    e = nullElt;
                extrasBefore.push_back(e);
                extraSize += e.size();
            }
            for ( unsigned i = 0; i < spec.numExtraAfter(); i++ ) {
                BSONElement e = obj.getFieldDotted(spec.extraAfter(i));
                if ( e.eoo() )
                    e = nullElt;
                extrasAfter.push_back(e);
                extraSize += e.size();
            }


            TermFrequencyMap term_freqs;
            spec.scoreDocument( obj, &term_freqs );

            // create index keys from raw scores
            // only 1 per string
            for ( TermFrequencyMap::const_iterator i = term_freqs.begin();
                  i != term_freqs.end();
                  ++i ) {

                const string& term = i->first;
                double weight = i->second;

                // guess the total size of the btree entry based on the size of the weight, term tuple
                int guess =
                    5 /* bson overhead */ +
                    10 /* weight */ +
                    8 /* term overhead */ +
                    term.size() +
                    extraSize;

                BSONObjBuilder b(guess); // builds a BSON object with guess length.
                for ( unsigned k = 0; k < extrasBefore.size(); k++ )
                    b.appendAs( extrasBefore[k], "" );
                _appendIndexKey( b, weight, term );
                for ( unsigned k = 0; k < extrasAfter.size(); k++ )
                    b.appendAs( extrasAfter[k], "" );
                BSONObj res = b.obj();

                verify( guess >= res.objsize() );

                keys->insert( res );
            }
        }

        BSONObj FTSIndexFormat::getIndexKey( double weight,
                                             const string& term,
                                             const BSONObj& indexPrefix ) {
            BSONObjBuilder b;

            BSONObjIterator i( indexPrefix );
            while ( i.more() )
                b.appendAs( i.next(), "" );

            _appendIndexKey( b, weight, term );
            return b.obj();
        }

        void FTSIndexFormat::_appendIndexKey( BSONObjBuilder& b, double weight, const string& term ) {
            verify( weight >= 0 && weight <= MAX_WEIGHT ); // FTSmaxweight =  defined in fts_header
            b.append( "", term );
            b.append( "", weight );
        }
    }
}
