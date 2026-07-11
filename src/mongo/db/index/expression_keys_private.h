// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

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
                            bool isSetSparseByUser,
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
