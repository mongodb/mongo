/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/storage/key_string.h"

namespace mongo {

class CollectionPtr;
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
    // Common
    //

    static void validateDocumentCommon(const CollectionPtr& collection,
                                       const BSONObj& obj,
                                       const BSONObj& keyPattern);

    //
    // 2d
    //

    static void get2DKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                          const BSONObj& obj,
                          const TwoDIndexingParams& params,
                          KeyStringSet* keys,
                          KeyString::Version keyStringVersion,
                          Ordering ordering,
                          const boost::optional<RecordId>& id = boost::none);

    //
    // FTS
    //

    static void getFTSKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                           const BSONObj& obj,
                           const fts::FTSSpec& ftsSpec,
                           KeyStringSet* keys,
                           KeyString::Version keyStringVersion,
                           Ordering ordering,
                           const boost::optional<RecordId>& id = boost::none);

    //
    // Hash
    //

    /**
     * Generates keys for hash access method.
     */
    static void getHashKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                            const BSONObj& obj,
                            const BSONObj& keyPattern,
                            HashSeed seed,
                            int hashVersion,
                            bool isSparse,
                            const CollatorInterface* collator,
                            KeyStringSet* keys,
                            KeyString::Version keyStringVersion,
                            Ordering ordering,
                            bool ignoreArraysAlongPath,
                            const boost::optional<RecordId>& id = boost::none);

    /**
     * Hashing function used by both getHashKeys and the cursors we create.
     * Exposed for testing in dbtests/namespacetests.cpp and
     * so mongo/db/index_legacy.cpp can use it.
     */
    static long long int makeSingleHashKey(const BSONElement& e, HashSeed seed, int v);

    //
    // S2
    //

    /**
     * Generates keys for S2 access method.
     */
    static void getS2Keys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                          const BSONObj& obj,
                          const BSONObj& keyPattern,
                          const S2IndexingParams& params,
                          KeyStringSet* keys,
                          MultikeyPaths* multikeyPaths,
                          KeyString::Version keyStringVersion,
                          SortedDataIndexAccessMethod::GetKeysContext context,
                          Ordering ordering,
                          const boost::optional<RecordId>& id = boost::none);
};

}  // namespace mongo
