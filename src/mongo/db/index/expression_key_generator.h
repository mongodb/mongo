/**
*    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include <vector>
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/hasher.h"

namespace mongo {

    struct TwoDIndexingParams;
    struct S2IndexingParams;

    namespace fts {
        class FTSSpec;
    } // namespace fts



    //
    // 2D
    //

    /**
     * Generate keys for 2d access method.
     * Finds the key objects and/or locations for a geo-indexed object.
     */
    void get2DKeys(const BSONObj &obj, const TwoDIndexingParams& params,
                   BSONObjSet* keys, std::vector<BSONObj>* locs);



    //
    // FTS
    //

    /**
     * Generates keys for FTS access method
     */
    void getFTSKeys(const BSONObj &obj, const fts::FTSSpec& ftsSpec, BSONObjSet* keys);



    //
    // Hash
    //

    /**
     * Generates keys for hash access method.
     */
    void getHashKeys(const BSONObj& obj, const std::string& hashedField, HashSeed seed,
                     int hashVersion, bool isSparse, BSONObjSet* keys);

    /**
     * Hashing function used by both getHashKeys and the cursors we create.
     * Exposed for testing in dbtests/namespacetests.cpp and
     * so mongo/db/index_legacy.cpp can use it.
     */
    long long int makeSingleHashKey(const BSONElement& e, HashSeed seed, int v);



    //
    // Haystack
    //

    /**
     * Generates keys for hay stack access method.
     */
    void getHaystackKeys(const BSONObj& obj, const std::string& geoField,
                         const std::vector<std::string>& otherFields,
                         double bucketSize,
                         BSONObjSet* keys);

    /**
     * Returns a hash of a BSON element.
     * Used by getHaystackKeys and HaystackAccessMethod::searchCommand.
     */
    int hashHaystackElement(const BSONElement& e, double bucketSize);



    /**
     * Joins two strings using underscore as separator.
     * Used by getHaystackKeys and HaystackAccessMethod::searchCommand.
     */
    std::string makeHaystackString(int hashedX, int hashedY);



    //
    // S2
    //

    /**
     * Generates keys for S2 access method.
     */
    void getS2Keys(const BSONObj& obj, const BSONObj& keyPattern, const S2IndexingParams& params,
                   BSONObjSet* keys);

}  // namespace mongo
