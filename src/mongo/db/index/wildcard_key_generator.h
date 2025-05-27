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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This class is responsible for generating an aggregation projection based on the keyPattern and
 * pathProjection specs, and for subsequently extracting the set of all path-value pairs for each
 * document.
 *
 * This key generator supports generating index keys for a compound or a single-field wildcard
 * index. If 'keyPattern' is compound, the generator will delagate the index key generation of
 * regular fields to two 'BtreeKeyGenerator'. At last it combines all these three parts
 * (prefix/suffix of regular fields and the wildcard field) into one 'KeyString'.
 */
class WildcardKeyGenerator {
public:
    static constexpr StringData kSubtreeSuffix = WildcardNames::WILDCARD_FIELD_NAME_SUFFIX;

    /**
     * Returns an owned ProjectionExecutor identical to the one that WildcardKeyGenerator will use
     * internally when generating the keys for the $** index, as defined by the 'keyPattern' and
     * 'pathProjection' arguments.
     */
    static WildcardProjection createProjectionExecutor(BSONObj keyPattern, BSONObj pathProjection);

    WildcardKeyGenerator(BSONObj keyPattern,
                         BSONObj pathProjection,
                         const CollatorInterface* collator,
                         key_string::Version keyStringVersion,
                         Ordering ordering,
                         boost::optional<KeyFormat> rsKeyFormat = boost::none);

    /**
     * Returns a pointer to the key generator's underlying ProjectionExecutor.
     */
    const WildcardProjection* getWildcardProjection() const {
        return &_proj;
    }

    /**
     * Applies the appropriate Wildcard projection to the input doc, and then adds one key-value
     * pair to the set 'keys' for each leaf node in the post-projection document:
     *      { '': 'path.to.field', '': <collation-aware-field-value> }
     * Also adds one entry to 'multikeyPaths' for each array encountered in the post-projection
     * document, in the following format:
     *      { '': 1, '': 'path.to.array' }
     */
    void generateKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
                      BSONObj inputDoc,
                      KeyStringSet* keys,
                      KeyStringSet* multikeyPaths,
                      const boost::optional<RecordId>& id = boost::none) const;

private:
    WildcardProjection _proj;
    const CollatorInterface* _collator;
    const BSONObj _keyPattern;
    const key_string::Version _keyStringVersion;
    const Ordering _ordering;
    const boost::optional<KeyFormat> _rsKeyFormat;
    boost::optional<BtreeKeyGenerator> _preBtreeGenerator = boost::none;
    boost::optional<BtreeKeyGenerator> _postBtreeGenerator = boost::none;
};
}  // namespace mongo
