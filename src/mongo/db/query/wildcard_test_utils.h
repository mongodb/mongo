/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"

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
                                                  nullptr,
                                                  BSONObj(),
                                                  nullptr,
                                                  &wildcardProjection);
    }

    MultikeyPaths emptyMultiKeyPaths;
    WildcardProjection wildcardProjection;
    std::unique_ptr<IndexEntry> indexEntry;
};
}  // namespace mongo::wildcard_planning
