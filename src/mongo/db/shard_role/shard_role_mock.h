// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

#include <absl/container/flat_hash_map.h>
#include <absl/container/inlined_vector.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Anything under the shard_role_mock is meant to be used in testing.
 */
namespace [[MONGO_MOD_PUBLIC]] shard_role_mock {

/**
 * Given an already acquired Collection instance from the catalog, register the collection as
 * acquisition within the shard role api and return the related handler. The returned acquisition
 * won't have any collection descriptions nor collection filter. It won't also provide any
 * versioning check nor change the read source. This is useful for testing, where acquisitions are
 * needed either on mocked collection or as placehoder, but we don't need both replication and
 * sharding.
 */
CollectionAcquisition acquireCollectionMocked(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              CollectionPtr collptr);

CollectionAcquisition acquireCollectionMocked(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              ConsistentCollection collection);
}  // namespace shard_role_mock
}  // namespace mongo
