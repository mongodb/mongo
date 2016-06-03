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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/multikey_paths.h"

namespace mongo {

class CollatorInterface;
struct TwoDIndexingParams;
struct S2IndexingParams;

namespace fts {

class FTSSpec;

}  // namespace fts

/**
 * Do not use this class or any of its methods directly.  The key generation of btree-indexed
 * expression indices is kept outside of the access method for testing and for upgrade
 * compatibility checking.
 */
class ExpressionKeysPrivate {
public:
    //
    // 2d
    //

    static void get2DKeys(const BSONObj& obj,
                          const TwoDIndexingParams& params,
                          BSONObjSet* keys,
                          std::vector<BSONObj>* locs);

    //
    // FTS
    //

    static void getFTSKeys(const BSONObj& obj, const fts::FTSSpec& ftsSpec, BSONObjSet* keys);

    //
    // Hash
    //

    /**
     * Generates keys for hash access method.
     */
    static void getHashKeys(const BSONObj& obj,
                            const std::string& hashedField,
                            HashSeed seed,
                            int hashVersion,
                            bool isSparse,
                            const CollatorInterface* collator,
                            BSONObjSet* keys);

    /**
     * Hashing function used by both getHashKeys and the cursors we create.
     * Exposed for testing in dbtests/namespacetests.cpp and
     * so mongo/db/index_legacy.cpp can use it.
     */
    static long long int makeSingleHashKey(const BSONElement& e, HashSeed seed, int v);

    //
    // Haystack
    //

    /**
     * Generates keys for haystack access method.
     */
    static void getHaystackKeys(const BSONObj& obj,
                                const std::string& geoField,
                                const std::vector<std::string>& otherFields,
                                double bucketSize,
                                BSONObjSet* keys);

    /**
     * Returns a hash of a BSON element.
     * Used by getHaystackKeys and HaystackAccessMethod::searchCommand.
     */
    static int hashHaystackElement(const BSONElement& e, double bucketSize);

    /**
     * Joins two strings using underscore as separator.
     * Used by getHaystackKeys and HaystackAccessMethod::searchCommand.
     */
    static std::string makeHaystackString(int hashedX, int hashedY);

    //
    // S2
    //

    /**
     * Generates keys for S2 access method.
     */
    static void getS2Keys(const BSONObj& obj,
                          const BSONObj& keyPattern,
                          const S2IndexingParams& params,
                          BSONObjSet* keys,
                          MultikeyPaths* multikeyPaths);
};

}  // namespace mongo
