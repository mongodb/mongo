// XXX THIS FILE IS DEPRECATED.  PLEASE DON'T MODIFY.

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

        // Explicit null valued fields and missing fields are both represented in hashed indexes
        // using the hash value of the null BSONElement.  This is partly for historical reasons
        // (hash of null was used in the initial release of hashed indexes and changing would alter
        // the data format).  Additionally, in certain places the hashed index code and the index
        // bound calculation code assume null and missing are indexed identically.
        BSONObj nullObj = BSON( "" << BSONNULL );
        _missingKey = BSON( "" << makeSingleKey( nullObj.firstElement(), _seed, _hashVersion ) );
    }

    HashedIndexType::~HashedIndexType() { }

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
            keys.insert( _missingKey.copy() );
        }
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

