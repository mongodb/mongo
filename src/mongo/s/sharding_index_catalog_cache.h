/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/index_version.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using IndexCatalogTypeMap = StringMap<IndexCatalogType>;

class ShardingIndexesCatalogCache {
public:
    ShardingIndexesCatalogCache(CollectionIndexes collectionIndexes, IndexCatalogTypeMap&& indexes)
        : _collectionIndexes(std::move(collectionIndexes)), _indexes(std::move(indexes)) {}

    bool empty() const;

    CollectionIndexes getCollectionIndexes() const;

    size_t numIndexes() const;

    bool contains(StringData name) const;

    void add(const IndexCatalogType& index, const CollectionIndexes& collectionIndexes);

    void remove(StringData name, const CollectionIndexes& collectionIndexes);

    template <typename Callable>
    void forEachIndex(Callable&& handler) const {
        for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
            if (!handler(it->second))
                return;
        }
    }

    template <typename Callable>
    void forEachGlobalIndex(Callable&& handler) const {
        for (auto it = _indexes.begin(); it != _indexes.end(); it++) {
            auto options = IndexOptionsType::parse(IDLParserContext("forEachGlobalIndexCtx"),
                                                   it->second.getOptions());
            if (options.getGlobal() && !handler(it->second)) {
                return;
            }
        }
    }

private:
    CollectionIndexes _collectionIndexes;
    IndexCatalogTypeMap _indexes;
};

}  // namespace mongo
