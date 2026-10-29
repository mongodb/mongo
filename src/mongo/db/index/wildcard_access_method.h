// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/record_id.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cstddef>
#include <memory>

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];
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

    /*
     * We should make a new Ordering for wildcard key generator because the index keys generated for
     * wildcard indexes include a "$_path" field prior to the wildcard field and the Ordering passed
     * in does not account for the "$_path" field.
     */
    static Ordering makeOrdering(const BSONObj& pattern);

private:
    void doGetKeys(OperationContext* opCtx,
                   const CollectionPtr& collection,
                   const IndexCatalogEntry* entry,
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
