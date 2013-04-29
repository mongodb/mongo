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

#include "mongo/db/btree.h"
#include "mongo/db/btreecursor.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/hash_index_cursor.h"
#include "mongo/db/queryutil.h"

namespace mongo {

    long long int HashAccessMethod::makeSingleKey(const BSONElement& e, HashSeed seed, int v) {
        massert(16767, "Only HashVersion 0 has been defined" , v == 0 );
        return BSONElementHasher::hash64(e, seed);
    }

    BSONObj HashAccessMethod::getMissingField(const IndexDetails& details) {
        BSONObj infoObj = details.info.obj();
        int hashVersion = infoObj["hashVersion"].numberInt();
        HashSeed seed = infoObj["seed"].numberInt();

        // Explicit null valued fields and missing fields are both represented in hashed indexes
        // using the hash value of the null BSONElement.  This is partly for historical reasons
        // (hash of null was used in the initial release of hashed indexes and changing would alter
        // the data format).  Additionally, in certain places the hashed index code and the index
        // bound calculation code assume null and missing are indexed identically.
        BSONObj nullObj = BSON("" << BSONNULL);
        return BSON("" << makeSingleKey(nullObj.firstElement(), seed, hashVersion));
    }

    HashAccessMethod::HashAccessMethod(IndexDescriptor* descriptor)
        : BtreeBasedAccessMethod(descriptor) {

        const string HASHED_INDEX_TYPE_IDENTIFIER = "hashed";

        //change these if single-field limitation lifted later
        uassert(16763, "Currently only single field hashed index supported." ,
                1 == descriptor->getNumFields());
        uassert(16764, "Currently hashed indexes cannot guarantee uniqueness. Use a regular index.",
                !descriptor->unique());

        //Default _seed to 0 if "seed" is not included in the index spec
        //or if the value of "seed" is not a number
        _seed = descriptor->getInfoElement("seed").numberInt();

        //In case we have hashed indexes based on other hash functions in
        //the future, we store a hashVersion number. If hashVersion changes,
        // "makeSingleKey" will need to change accordingly.
        //Defaults to 0 if "hashVersion" is not included in the index spec
        //or if the value of "hashversion" is not a number
        _hashVersion = descriptor->getInfoElement("hashVersion").numberInt();

        //Get the hashfield name
        BSONElement firstElt = descriptor->keyPattern().firstElement();
        massert(16765, "error: no hashed index field",
                firstElt.str().compare(HASHED_INDEX_TYPE_IDENTIFIER) == 0);
        _hashedField = firstElt.fieldName();

        BSONObj nullObj = BSON("" << BSONNULL);
        _missingKey = BSON("" << makeSingleKey(nullObj.firstElement(), _seed, _hashVersion));
    }

    Status HashAccessMethod::newCursor(IndexCursor** out) {
        *out = new HashIndexCursor(_hashedField, _seed, _hashVersion, _descriptor);
        return Status::OK();
    }

    void HashAccessMethod::getKeys(const BSONObj& obj, BSONObjSet* keys) {
        const char* cstr = _hashedField.c_str();
        BSONElement fieldVal = obj.getFieldDottedOrArray(cstr);
        uassert(16766, "Error: hashed indexes do not currently support array values",
                fieldVal.type() != Array );

        if (!fieldVal.eoo()) {
            BSONObj key = BSON( "" << makeSingleKey( fieldVal , _seed , _hashVersion  ) );
            keys->insert( key );
        } else if (!_descriptor->isSparse()) {
            keys->insert( _missingKey.copy() );
        }
    }

}  // namespace mongo
