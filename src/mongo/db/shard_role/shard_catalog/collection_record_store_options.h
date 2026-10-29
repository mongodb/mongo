// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/util/modules.h"

namespace mongo {
using namespace std::literals::string_view_literals;

static constexpr auto kDefaultTimeseriesCollectionCompressor = "zstd"sv;

/**
 * Each Collection is backed by a RecordStore in the storage layer. Translates 'CollectionOptions'
 * into 'RecordStore::Options' for a Collection's RecordStore taking into account also
 * `recordIdsReplicated`.
 */
RecordStore::Options getRecordStoreOptions(const NamespaceString& nss,
                                           const CollectionOptions& collectionOptions,
                                           bool recordIdsReplicated);

}  // namespace mongo
