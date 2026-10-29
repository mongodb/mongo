// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/geo/2d_common.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/modules.h"
#include "mongo/util/shared_buffer_fragment.h"

[[MONGO_MOD_PUBLIC]];
namespace mongo::index2d {

/**
 * Generates keys for 2d access method, used for 2d index type.
 */
void get2DKeys(SharedBufferFragmentBuilder& pooledBufferBuilder,
               const BSONObj& obj,
               const TwoDIndexingParams& params,
               KeyStringSet* keys,
               key_string::Version keyStringVersion,
               Ordering ordering,
               const boost::optional<RecordId>& id = boost::none);
}  // namespace mongo::index2d
