// mongo/db/hashindex.cpp

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

#include "mongo/db/hashindex.h"

#include "mongo/db/btreecursor.h"
#include "mongo/db/json.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    const string HashedIndexType::HASHED_INDEX_TYPE_IDENTIFIER = "hashed";

    HashedIndexType::HashedIndexType( const IndexPlugin* plugin , const IndexSpec* spec ) :
            IndexType( plugin , spec ) , _keyPattern( spec->keyPattern ) {

        //change these if single-field limitation lifted later
        uassert( 16241 , "Currently only single field hashed index supported." ,
                _keyPattern.toBSON().nFields() == 1 );
        uassert( 16242 , "Currently hashed indexes cannot guarantee uniqueness. Use a regular index." ,
                ! (spec->info).getField("unique").booleanSafe() );

        //Default _seed to 0 if "seed" is not included in the index spec
        //or if the value of "seed" is not a number
        _seed = (spec->info).getField("seed").numberInt();

        //Default _isSparse to false if "sparse" is not included in the index spec
        //or if the value of "sparse" is not a boolean
        _isSparse = (spec->info).getField("sparse").booleanSafe();

        //In case we have hashed indexes based on other hash functions in
        //the future, we store a hashVersion number. If hashVersion changes,
        // "makeSingleKey" will need to change accordingly.
        //Defaults to 0 if "hashVersion" is not included in the index spec
        //or if the value of "hashversion" is not a number
        _hashVersion = (spec->info).getField("hashVersion").numberInt();

        //Get the hashfield name
        BSONElement firstElt = spec->keyPattern.firstElement();
        massert( 16243 , "error: no hashed index field" ,
                firstElt.str().compare( HASHED_INDEX_TYPE_IDENTIFIER ) == 0 );
        _hashedField = firstElt.fieldName();
    }

    HashedIndexType::~HashedIndexType() { }

    IndexSuitability HashedIndexType::suitability( const FieldRangeSet& queryConstraints ,
                                                   const BSONObj& order ) const {
        if ( queryConstraints.isPointIntervalSet( _hashedField ) )
            return HELPFUL;
        return USELESS;
    }

    void HashedIndexType::getKeys( const BSONObj &obj, BSONObjSet &keys ) const {
        string hashedFieldCopy = string( _hashedField );
        const char* hashedFieldCopyPtr = hashedFieldCopy.c_str();
        BSONElement fieldVal = obj.getFieldDottedOrArray( hashedFieldCopyPtr );

        uassert( 16244 , "Error: hashed indexes do not currently support array values" , fieldVal.type() != Array );

        if ( ! fieldVal.eoo() ) {
            BSONObj key = BSON( "" << makeSingleKey( fieldVal , _seed , _hashVersion  ) );
            keys.insert( key );
        }
        else if (! _isSparse ) {
            BSONObj nullobj = BSON( _hashedField << BSONNULL );
            BSONElement nullElt = nullobj.firstElement();
            BSONObj key = BSON( "" << makeSingleKey( nullElt , _seed , _hashVersion  ) );
            keys.insert( key );
        }
    }

    shared_ptr<Cursor> HashedIndexType::newCursor( const BSONObj& query ,
            const BSONObj& order , int numWanted ) const {

        //Use FieldRangeSet to parse the query into a vector of intervals
        //These should be point-intervals if this cursor is ever used
        //So the FieldInterval vector will be, e.g. <[1,1], [3,3], [6,6]>
        FieldRangeSet frs( "" , query , true, true );
        const vector<FieldInterval>& intervals = frs.range( _hashedField.c_str() ).intervals();

        //Force a match of the query against the actual document by giving
        //the cursor a matcher with an empty indexKeyPattern.  This insures the
        //index is not used as a covered index.
        //NOTE: this forcing is necessary due to potential hash collisions
        const shared_ptr< CoveredIndexMatcher > forceDocMatcher(
                new CoveredIndexMatcher( query , BSONObj() ) );

        //Construct a new query based on the hashes of the previous point-intervals
        //e.g. {a : {$in : [ hash(1) , hash(3) , hash(6) ]}}
        BSONObjBuilder newQueryBuilder;
        BSONObjBuilder inObj( newQueryBuilder.subobjStart( _hashedField ) );
        BSONArrayBuilder inArray( inObj.subarrayStart("$in") );
        vector<FieldInterval>::const_iterator i;
        for( i = intervals.begin(); i != intervals.end(); ++i ){
            if ( ! i->equality() ){
                const shared_ptr< BtreeCursor > exhaustiveCursor(
                        BtreeCursor::make( nsdetails( _spec->getDetails()->parentNS().c_str()),
                                           *( _spec->getDetails() ),
                                           BSON( "" << MINKEY ) ,
                                           BSON( "" << MAXKEY ) ,
                                           true ,
                                           1 ) );
                exhaustiveCursor->setMatcher( forceDocMatcher );
                return exhaustiveCursor;
            }
            inArray.append( makeSingleKey( i->_lower._bound , _seed , _hashVersion ) );
        }
        inArray.done();
        inObj.done();
        BSONObj newQuery = newQueryBuilder.obj();

        //Use the point-intervals of the new query to create a Btree cursor
        FieldRangeSet newfrs( "" , newQuery , true, true );
        shared_ptr<FieldRangeVector> newVector(
                new FieldRangeVector( newfrs , *_spec , 1 ) );

        const shared_ptr< BtreeCursor > cursor(
                BtreeCursor::make( nsdetails( _spec->getDetails()->parentNS().c_str()),
                        *( _spec->getDetails() ), newVector, 0, 1 ) );
        cursor->setMatcher( forceDocMatcher );
        return cursor;
    }


    /* This class registers HASHED_INDEX_NAME in a global map of special index types
     * Using this pattern, any index with the pattern, {fieldname : HASHED_INDEX_NAME}
     * will be recognized as a HashedIndexType and the associated methods will be used.
     */
    class HashedIndexPlugin : public IndexPlugin {
    public:

        HashedIndexPlugin() : IndexPlugin( HashedIndexType::HASHED_INDEX_TYPE_IDENTIFIER ) {}

        virtual IndexType* generate( const IndexSpec* spec ) const {
            return new HashedIndexType( this , spec );
        }

    } hashedIndexPlugin;


    long long int HashedIndexType::makeSingleKey( const BSONElement& e ,
                                                  HashSeed seed ,
                                                  HashVersion v ) {
        massert( 16245 , "Only HashVersion 0 has been defined" , v == 0 );
        return BSONElementHasher::hash64( e , seed );
    }

}

