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

#include "mongo/db/structure/btree/btree.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/expression_params.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/hash_key_generator.h"

namespace mongo {

    HashKeyGenerator::HashKeyGenerator( const std::string& hashedField,
                                        HashSeed seed,
                                        int hashVersion,
                                        bool isSparse )
        : _hashedField( hashedField ),
          _seed( seed ),
          _hashVersion( hashVersion ),
          _isSparse( isSparse ) {
    }

    void HashKeyGenerator::getKeys( const BSONObj& obj, BSONObjSet* keys ) const {
        const char* cstr = _hashedField.c_str();
        BSONElement fieldVal = obj.getFieldDottedOrArray(cstr);
        uassert(16766, "Error: hashed indexes do not currently support array values",
                fieldVal.type() != Array );

        if (!fieldVal.eoo()) {
            BSONObj key = BSON( "" << makeSingleHashKey(fieldVal, _seed, _hashVersion));
            keys->insert(key);
        }
        else if (!_isSparse) {
            BSONObj nullObj = BSON("" << BSONNULL);
            keys->insert(BSON("" << makeSingleHashKey(nullObj.firstElement(), _seed, _hashVersion)));
        }

    }

    long long int HashKeyGenerator::makeSingleHashKey(const BSONElement& e, HashSeed seed, int v) {
        massert(16767, "Only HashVersion 0 has been defined" , v == 0 );
        return BSONElementHasher::hash64(e, seed);
    }


}  // namespace mongo
