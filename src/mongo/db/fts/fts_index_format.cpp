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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/base/init.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/util/mongoutils/str.h"

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
                uassert( 16675, "cannot have a multi-key as a prefix to a text index",
                         e.type() != Array );
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

            uassert( 16732,
                     mongoutils::str::stream() << "too many unique keys for a single document to"
                     << " have a text index, max is " << term_freqs.size() << obj["_id"],
                     term_freqs.size() <= 400000 );

            long long keyBSONSize = 0;
            const int MaxKeyBSONSizeMB = 4;

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

                keyBSONSize += res.objsize();

                uassert( 16733,
                         mongoutils::str::stream()
                         << "trying to index text where term list is too big, max is "
                         << MaxKeyBSONSizeMB << "mb " << obj["_id"],
                         keyBSONSize <= ( MaxKeyBSONSizeMB * 1024 * 1024 ) );

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
