// @file keypattern.cpp

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

#include "mongo/db/keypattern.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/db/hasher.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    BSONObj KeyPattern::extractSingleKey(const BSONObj& doc ) const {
        if ( _pattern.isEmpty() )
            return BSONObj();

        if ( mongoutils::str::equals( _pattern.firstElement().valuestrsafe() , "hashed" ) ){
            BSONElement fieldVal = doc.getFieldDotted( _pattern.firstElementFieldName() );
            return BSON( _pattern.firstElementFieldName() <<
                         BSONElementHasher::hash64( fieldVal ,
                                                    BSONElementHasher::DEFAULT_HASH_SEED ) );
        }

        return doc.extractFields( _pattern );
    }

    bool KeyPattern::isSpecial() const {
        BSONForEach(e, _pattern) {
            int fieldVal = e.numberInt();
            if ( fieldVal != 1 && fieldVal != -1 ){
                return true;
            }
        }
        return false;
    }

    BoundList KeyPattern::keyBounds( const FieldRangeSet& queryConstraints ) const {
        typedef vector< pair< shared_ptr<BSONObjBuilder> ,
                              shared_ptr<BSONObjBuilder> > > BoundBuilders;
        BoundBuilders builders;
        builders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ),
                                       shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
        BSONObjIterator i( _pattern );
        // until equalityOnly is false, we are just dealing with equality (no range or $in querys).
        bool equalityOnly = true;
        while( i.more() ) {
            BSONElement e = i.next();
            const FieldRange &fr = queryConstraints.range( e.fieldName() );
            int number = (int) e.number(); // returns 0.0 if not numeric
            bool forward = ( number >= 0 );
            if ( equalityOnly ) {
                if ( fr.equality() ) {
                    for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                        j->first->appendAs( fr.min(), "" );
                        j->second->appendAs( fr.min(), "" );
                    }
                }
                else {
                    equalityOnly = false;

                    BoundBuilders newBuilders;
                    const vector<FieldInterval> &intervals = fr.intervals();
                    for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i ) {
                        BSONObj first = i->first->obj();
                        BSONObj second = i->second->obj();

                        if ( forward ) {
                            for( vector<FieldInterval>::const_iterator j = intervals.begin(); j != intervals.end(); ++j ) {
                                uassert( 16449, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < MAX_IN_COMBINATIONS );
                                newBuilders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ), shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_lower._bound, "" );
                                newBuilders.back().second->appendAs( j->_upper._bound, "" );
                            }
                        }
                        else {
                            for( vector<FieldInterval>::const_reverse_iterator j = intervals.rbegin(); j != intervals.rend(); ++j ) {
                                uassert( 16450, "combinatorial limit of $in partitioning of result set exceeded", newBuilders.size() < MAX_IN_COMBINATIONS );
                                newBuilders.push_back( make_pair( shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ), shared_ptr<BSONObjBuilder>( new BSONObjBuilder() ) ) );
                                newBuilders.back().first->appendElements( first );
                                newBuilders.back().second->appendElements( second );
                                newBuilders.back().first->appendAs( j->_upper._bound, "" );
                                newBuilders.back().second->appendAs( j->_lower._bound, "" );
                            }
                        }
                    }
                    builders = newBuilders;
                }
            }
            else {
                for( BoundBuilders::const_iterator j = builders.begin(); j != builders.end(); ++j ) {
                    j->first->appendAs( forward ? fr.min() : fr.max(), "" );
                    j->second->appendAs( forward ? fr.max() : fr.min(), "" );
                }
            }
        }
        BoundList ret;
        for( BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i )
            ret.push_back( make_pair( i->first->obj(), i->second->obj() ) );
        return ret;
    }

} // namespace mongo
