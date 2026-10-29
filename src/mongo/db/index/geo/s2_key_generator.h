// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/geo/s2_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

[[MONGO_MOD_PUBLIC]];
namespace mongo::index2dsphere {
/**
 * Generates keys for S2 access method, used for 2dsphere index type.
 */
void getS2Keys(SharedBufferFragmentBuilder& pooledBufferBuilder,
               const BSONObj& obj,
               const BSONObj& keyPattern,
               const S2IndexingParams& params,
               KeyStringSet* keys,
               MultikeyPaths* multikeyPaths,
               key_string::Version keyStringVersion,
               SortedDataIndexAccessMethod::GetKeysContext context,
               Ordering ordering,
               const boost::optional<RecordId>& id = boost::none);
}  // namespace mongo::index2dsphere
