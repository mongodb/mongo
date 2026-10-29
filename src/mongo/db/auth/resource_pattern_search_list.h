// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace mongo::auth {
using namespace std::literals::string_view_literals;
/**
 * A ResourcePatternSearchList represents up to kMaxResourcePatternLookups elements
 * of ResourcePattern objects representing the breakdown of the target ResourcePattern
 * into the subpatterns which may potentially match it.
 *
 * The search lists are as follows, depending on the type of "target":
 *
 * target is ResourcePattern::forAnyResource(tenantId):
 *   searchList = { ResourcePattern::forAnyResource(tenantId),
 *                  ResourcePattern::forAnyResource(boost::none) }
 * target is ResourcePattern::forClusterResource(tenantId):
 *   searchList = { ResourcePattern::forAnyResource(tenantId),
 *                  ResourcePattern::forClusterResource(tenantId),
 *                  ResourcePattern::forAnyResource(boost::none),
 *                  ResourcePattern::forClusterResource(boost::none) }
 * target is a database, tenantId_db:
 *   searchList = { ResourcePattern::forAnyResource(tenantId),
 *                  ResourcePattern::forAnyNormalResource(tenantId),
 *                  ResourcePattern::forAnyResource(boost::none),
 *                  ResourcePattern::forAnyNormalResource(boost::none),
 *                  tenantId_db }
 * target is a non-system collection, tenantId_db.coll:
 *   searchList = { ResourcePattern::forAnyResource(tenantId),
 *                  ResourcePattern::forAnyNormalResource(tenantId),
 *                  ResourcePattern::forAnyResource(boost::none),
 *                  ResourcePattern::forAnyNormalResource(boost::none),
 *                  tenantId_db,
 *                  tenantId_*.coll,
 *                  tenantId_db.coll }
 * target is a system buckets collection, tenantId_db.system.buckets.coll:
 *   searchList = { ResourcePattern::forAnyResource(tenantId),
 *                  ResourcePattern::forAnyResource(boost::none),
 *                  ResourcePattern::forAnySystemBuckets(tenantId),
 *                  ResourcePattern::forAnySystemBucketsInDatabase(tenantId, "db"),
 *                  ResourcePattern::forAnySystemBucketsInAnyDatabase(tenantId, "coll"),
 *                  ResourcePattern::forExactSystemBucketsCollection(tenantId, "db", "coll"),
 *                  tenantId_*.system.buckets.coll,
 *                  tenantId_db.system.buckets.coll }
 * target is a system collection, tenantId_db.system.coll:
 *   searchList = { ResourcePattern::forAnyResource(tenantId),
 *                  ResourcePattern::forAnyResource(boost::none),
 *                  tenantId_*.system.coll,
 *                  tenantId_db.system.coll }
 */
class ResourcePatternSearchList {
private:
    static constexpr std::string_view kSystemBucketsPrefix = "system.buckets."sv;
    static constexpr std::size_t kMaxResourcePatternLookups = 10;
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
                       nss.coll().starts_with(kSystemBucketsPrefix)) {
                // System bucket patterns behave similar to any/db/coll/exact patterns,
                // But with a fixed "system.buckets." prefix to the collection name.
                std::string_view coll = nss.coll().substr(kSystemBucketsPrefix.size());
                _list[_size++] = ResourcePattern::forExactSystemBucketsCollection(
                    NamespaceStringUtil::deserialize(nss.dbName(), coll));
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

        if (target.tenantId()) {
            // Add base tenant versions of top-level matching types.
            // This is so that a system user (with no tenant ID) with privileges on these general
            // match types will be able to take actions on tenant data.
            // Note that we'll only get this far if that user is authorized for the useTenant action
            // on the cluster resource.
            const auto oldSize = _size;
            for (size_type i = 0; i < oldSize; ++i) {
                if (_list[i].isAnyResourcePattern()) {
                    _list[_size++] = ResourcePattern::forAnyResource(boost::none);
                } else if (_list[i].isAnyNormalResourcePattern()) {
                    _list[_size++] = ResourcePattern::forAnyNormalResource(boost::none);
                } else if (_list[i].isClusterResourcePattern()) {
                    _list[_size++] = ResourcePattern::forClusterResource(boost::none);
                }
            }
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
