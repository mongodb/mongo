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

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CollectionPtr;
class CollatorInterface;

namespace fts {

class FTSSpec;

}  // namespace fts

/**
 * Do not use this class or any of its methods directly.
 *
 * The key generation of btree-indexed expression indices is kept outside of the access method for
 * testing and for upgrade compatibility checking.
 *
 * The key generators of 2d- and 2dsphere-indexed expressions are kept separate for code ownership
 * reasons.
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
    // FTS
    //

    static void getFTSKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                           const BSONObj& obj,
                           const fts::FTSSpec& ftsSpec,
                           KeyStringSet* keys,
                           key_string::Version keyStringVersion,
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
                            int hashVersion,
                            bool isSparse,
                            const CollatorInterface* collator,
                            KeyStringSet* keys,
                            key_string::Version keyStringVersion,
                            Ordering ordering,
                            bool ignoreArraysAlongPath,
                            const boost::optional<RecordId>& id = boost::none);

    /**
     * Hashing function used by both getHashKeys and the cursors we create.
     * Exposed for testing in dbtests/namespacetests.cpp and
     * so mongo/db/index_legacy.cpp can use it.
     */
    static long long int makeSingleHashKey(const BSONElement& e, int v);
};

}  // namespace mongo
