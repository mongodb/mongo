// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/util/modules.h"

namespace mongo::wildcard_planning {
/**
 * Owns WildcardProjection object for CoreIndexInfo since the latter holds only reference to it.
 */
struct WildcardIndexEntryMock {
    WildcardIndexEntryMock(BSONObj keyPattern, BSONObj wp, std::set<FieldRef> multiKeyPathSet)
        : emptyMultiKeyPaths{},
          wildcardProjection{WildcardKeyGenerator::createProjectionExecutor(keyPattern, wp)},
          indexEntry{} {
        std::string indexName{"wc_1"};
        const auto type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
        indexEntry = std::make_unique<IndexEntry>(keyPattern,
                                                  type,
                                                  IndexConfig::kLatestIndexVersion,
                                                  false,
                                                  emptyMultiKeyPaths,
                                                  multiKeyPathSet,
                                                  true,
                                                  false,
                                                  CoreIndexInfo::Identifier{indexName},
                                                  BSONObj(),
                                                  &wildcardProjection);
    }

    MultikeyPaths emptyMultiKeyPaths;
    WildcardProjection wildcardProjection;
    std::unique_ptr<IndexEntry> indexEntry;
};
}  // namespace mongo::wildcard_planning
