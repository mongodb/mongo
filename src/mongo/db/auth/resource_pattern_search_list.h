/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <array>
#include <cstddef>

#include <boost/preprocessor/control/iif.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/namespace_string_util.h"

namespace mongo::auth {
/**
 * A ResourcePatternSearchList represents up to kMaxResourcePatternLookups elements
 * of ResourcePattern objects representing the breakdown of the target ResourcePattern
 * into the subpatterns which may potentially match it.
 *
 * The seach lists are as follows, depending on the type of "target":
 *
 * target is ResourcePattern::forAnyResource():
 *   searchList = { ResourcePattern::forAnyResource() }
 * target is the ResourcePattern::forClusterResource():
 *   searchList = { ResourcePattern::forAnyResource(), ResourcePattern::forClusterResource() }
 * target is a database, db:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnyNormalResource(),
 *                  db }
 * target is a non-system collection, db.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnyNormalResource(),
 *                  db,
 *                  coll,
 *                  db.coll }
 * target is a system buckets collection, db.system.buckets.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  ResourcePattern::forAnySystemBuckets(),
 *                  ResourcePattern::forAnySystemBucketsInDatabase("db"),
 *                  ResourcePattern::forAnySystemBucketsInAnyDatabase("coll"),
 *                  ResourcePattern::forExactSystemBucketsCollection("db", "coll"),
 *                  system.buckets.coll,
 *                  db.system.buckets.coll }
 * target is a system collection, db.system.coll:
 *   searchList = { ResourcePattern::forAnyResource(),
 *                  system.coll,
 *                  db.system.coll }
 */
class ResourcePatternSearchList {
private:
    static constexpr StringData kSystemBucketsPrefix = "system.buckets."_sd;
    static constexpr std::size_t kMaxResourcePatternLookups = 7;
    using ListType = std::array<ResourcePattern, kMaxResourcePatternLookups>;

public:
    ResourcePatternSearchList() = delete;
    explicit ResourcePatternSearchList(const ResourcePattern& target) {
        _list[_size++] = ResourcePattern::forAnyResource(target.tenantId());
        if (target.isExactNamespacePattern()) {
            const auto& nss = target.ns();

            // Normal collections can be matched by anyNormalResource, or their database's resource.
            if (nss.isNormalCollection()) {
                // But even normal collections in non-normal databases should not be matchable with
                // ResourcePattern::forAnyNormalResource. 'local' and 'config' are
                // used to store special system collections, which user level
                // administrators should not be able to manipulate.
                if (!nss.isLocalDB() && !nss.isConfigDB()) {
                    _list[_size++] = ResourcePattern::forAnyNormalResource(target.tenantId());
                }
                _list[_size++] = ResourcePattern::forDatabaseName(nss.dbName());
            } else if ((nss.coll().size() > kSystemBucketsPrefix.size()) &&
                       nss.coll().startsWith(kSystemBucketsPrefix)) {
                // System bucket patterns behave similar to any/db/coll/exact patterns,
                // But with a fixed "system.buckets." prefix to the collection name.
                StringData coll = nss.coll().substr(kSystemBucketsPrefix.size());
                _list[_size++] = ResourcePattern::forExactSystemBucketsCollection(
                    NamespaceStringUtil::parseNamespaceFromRequest(nss.dbName(), coll));
                _list[_size++] = ResourcePattern::forAnySystemBuckets(target.tenantId());
                _list[_size++] = ResourcePattern::forAnySystemBucketsInDatabase(nss.dbName());
                _list[_size++] =
                    ResourcePattern::forAnySystemBucketsInAnyDatabase(target.tenantId(), coll);
            }

            // All collections can be matched by a collection resource for their name
            _list[_size++] = ResourcePattern::forCollectionName(target.tenantId(), nss.coll());
        } else if (target.isDatabasePattern()) {
            if (!target.ns().isLocalDB() && !target.ns().isConfigDB()) {
                _list[_size++] = ResourcePattern::forAnyNormalResource(target.tenantId());
            }
        }

        if (!target.isAnyResourcePattern()) {
            _list[_size++] = target;
        }
        dassert(_size <= _list.size());
    }

    using const_iterator = ListType::const_iterator;
    using size_type = ListType::size_type;

    const_iterator cbegin() const noexcept {
        return _list.cbegin();
    }

    const_iterator cend() const noexcept {
        return _list.cbegin() + _size;
    }

    size_type size() const noexcept {
        return _size;
    }

    bool empty() const noexcept {
        return _size > 0;
    }

private:
    ListType _list;
    size_type _size{0};
};

}  // namespace mongo::auth
