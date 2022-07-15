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

#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/index_bounds.h"

namespace mongo {

/**
 * Class which is responsible for generating and providing access to Wildcard index keys. Any index
 * created with { "$**": ±1 } or { "path.$**": ±1 } uses this class.
 *
 * $** indexes store a special metadata key for each path in the index that is multikey. This class
 * provides an interface to access the multikey metadata: see getMultikeyPaths().
 */
class WildcardAccessMethod final : public SortedDataIndexAccessMethod {
public:
    WildcardAccessMethod(IndexCatalogEntry* wildcardState,
                         std::unique_ptr<SortedDataInterface> btree);

    /**
     * Returns 'true' if the index should become multikey on the basis of the passed arguments.
     * Because it is possible for a $** index to generate multiple keys per document without any of
     * them lying along a multikey (i.e. array) path, this method will only return 'true' if one or
     * more multikey metadata keys have been generated; that is, if the 'multikeyMetadataKeys'
     * vector is non-empty.
     */
    bool shouldMarkIndexAsMultikey(size_t numberOfKeys,
                                   const KeyStringSet& multikeyMetadataKeys,
                                   const MultikeyPaths& multikeyPaths) const final;

    /**
     * Returns a pointer to the WildcardProjection owned by the underlying WildcardKeyGenerator.
     */
    const WildcardProjection* getWildcardProjection() const {
        return _keyGen.getWildcardProjection();
    }

private:
    void doGetKeys(OperationContext* opCtx,
                   const CollectionPtr& collection,
                   SharedBufferFragmentBuilder& pooledBufferBuilder,
                   const BSONObj& obj,
                   GetKeysContext context,
                   KeyStringSet* keys,
                   KeyStringSet* multikeyMetadataKeys,
                   MultikeyPaths* multikeyPaths,
                   const boost::optional<RecordId>& id) const final;

    const WildcardKeyGenerator _keyGen;
};
}  // namespace mongo
